#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536   // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量，epoll_wait最多返回的事件数

// 添加文件描述符
extern void addfd( int epollfd, int fd, bool one_shot ); // extern可以将addfd函数声明为外部函数，这样就可以在其他文件中使用这个函数，而它的定义在http_conn.cpp文件中。之所以它不定义在此main.cpp中，是因为它是一个全局函数，如果定义在此main.cpp中，那么它就只能在此main.cpp中使用，而不能在其他文件中使用。
extern void removefd( int epollfd, int fd );

void addsig(int sig, void( handler )(int)){
    struct sigaction sa; // sigaction定义于<signal.h>头文件中，用于设置信号处理函数。
    memset( &sa, '\0', sizeof( sa ) ); // memset定义于<string.h>头文件中，用于将sa结构体中的所有字节设置为0。'\0'代表空字符，也就是0。
    sa.sa_handler = handler;
    sigfillset( &sa.sa_mask ); // sigfillset定义于<signal.h>头文件中，用于将sa结构体中的所有信号设置为1。
    assert( sigaction( sig, &sa, NULL ) != -1 ); // assert定义于<assert.h>头文件中，用于断言，如果条件为假，则程序终止。
}

int main( int argc, char* argv[] ) {
    
    if( argc <= 1 ) {
        printf( "usage: %s port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi( argv[1] );
    addsig( SIGPIPE, SIG_IGN ); // 向已关闭的 socket 写数据时，默认会终止进程。高并发服务器通常都会忽略 SIGPIPE，避免偶发客户端断开导致整个进程退出。

    threadpool< http_conn >* pool = NULL; // pool是线程池指针，指向一个threadpool<http_conn>对象
    try {
        pool = new threadpool<http_conn>; // 工作线程会调用 http_conn::process() 处理任务
    } catch( ... ) {
        return 1;
    }

    http_conn* users = new http_conn[ MAX_FD ]; // users是http_conn对象数组，用于存储所有连接的客户端信息

    int listenfd = socket( PF_INET, SOCK_STREAM, 0 ); // PF_INET：IPv4, SOCK_STREAM：TCP, 0：默认协议

    int ret = 0; // ret是返回值，用于存储系统调用的返回值
    struct sockaddr_in address; // address是sockaddr_in结构体，用于存储地址信息
    address.sin_addr.s_addr = INADDR_ANY; // INADDR_ANY：表示任何IP地址
    address.sin_family = AF_INET; // AF_INET：IPv4
    address.sin_port = htons( port ); // htons：将端口从主机字节顺序转换为网络字节顺序

    // 端口复用
    int reuse = 1; // reuse是端口复用标志，1：启用端口复用，0：禁用端口复用
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) ); // setsockopt：设置套接字选项, SOL_SOCKET：套接字选项所在的协议层, SO_REUSEADDR：允许重用本地地址
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    ret = listen( listenfd, 5 );

    // 创建epoll对象，和事件数组，添加
    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create( 5 ); // 5代表epoll_wait最多返回的事件数
    // 添加到epoll对象中
    addfd( epollfd, listenfd, false ); // 把listenfd添加到epoll对象中，one_shot代表只触发一次事件，避免多个线程处理同一个事件。
    http_conn::m_epollfd = epollfd; // 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的

    while(true) {
        
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 ); // -1代表无限等待，直到有事件发生。返回就绪事件数量。
        
        if ( ( number < 0 ) && ( errno != EINTR ) ) { // 系统调用被信号打断时会返回 -1 且 errno 为 EINTR，这种情况一般不当作致命错误。
            printf( "epoll failure\n" );
            break;
        }

        for ( int i = 0; i < number; i++ ) {
            
            int sockfd = events[i].data.fd;
            
            if( sockfd == listenfd ) {
                
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                
                if ( connfd < 0 ) {
                    printf( "errno is: %d\n", errno );
                    continue;
                } 

                if( http_conn::m_user_count >= MAX_FD ) {
                    close(connfd);
                    continue;
                }
                users[connfd].init( connfd, client_address);

            } else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) { // EPOLLRDHUP：表示读端关闭连接，EPOLLHUP：表示连接挂起，EPOLLERR：表示错误

                users[sockfd].close_conn();

            } else if(events[i].events & EPOLLIN) { // EPOLLIN：表示有数据可读

                if(users[sockfd].read()) { // http_conn的非阻塞读，读取客户端数据。
                    pool->append(users + sockfd); // 把 users + sockfd 作为任务交给线程池，由工作线程解析并生成响应。
                } else {
                    users[sockfd].close_conn();
                }

            }  else if( events[i].events & EPOLLOUT ) { // EPOLLOUT：表示有数据可写，是内核在检测该 socket 的发送缓冲区，如果有空位，则可写。而非“内核缓冲区已经有数据/被写入了数据了”。

                if( !users[sockfd].write() ) {
                    users[sockfd].close_conn();
                }

            }
        }
    }
    
    close( epollfd );
    close( listenfd );
    delete [] users;
    delete pool;
    return 0;
}