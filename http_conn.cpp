#include "http_conn.h"
#include "tick_processor.h"
#include "tick_server.h"
#include <stdlib.h>
#include <string>
#include <cstring>

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站的根目录
const char* doc_root = "/home/larryli/projects/webserver/resources";

int setnonblocking( int fd ) { // 把 fd 设为非阻塞。非阻塞 + epoll 才能实现真正的 I/O 多路复用，否则一次 recv 会阻塞整个进程。
    int old_option = fcntl( fd, F_GETFL ); // fcntl是文件操作函数，用于获取文件描述符的属性。
    int new_option = old_option | O_NONBLOCK; // 设置模式的时候，使用或运算符，将 O_NONBLOCK 选项添加到 old_option 中。
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}

// 向epoll中添加需要监听的文件描述符
// one_shot=false：不设 EPOLLONESHOT，fd 会一直处于“监听”状态，每次有事件（例如有新连接）epoll 都会再返回它，适合只在主循环里用、不交给线程池的 fd。
// one_shot=true：设 EPOLLONESHOT，fd 被 epoll_wait 返回并交给你处理一次之后，内核就暂时不再用这个 fd 上报事件，直到你后面用 EPOLL_CTL_MOD 再“挂上去”一次。
void addfd( int epollfd, int fd, bool one_shot ) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP; // 监听功能，需要注册两个事件，EPOLLIN：表示有数据可读，EPOLLRDHUP：表示读端关闭连接
    if(one_shot)
    {
        // 防止同一个通信被不同的线程处理
        event.events |= EPOLLONESHOT; // 防止同一个通信被不同的线程处理
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event); // 添加到epoll中
    // 设置文件描述符非阻塞
    setnonblocking(fd);  // 设置文件描述符非阻塞
    // IO多路复用的两点：非阻塞 + epoll
    // 非阻塞：通过fcntl操纵fd的状态
    // epoll：通过epoll_ctl操纵fd的事件
}

// 从epoll中移除监听的文件描述符
void removefd( int epollfd, int fd ) {
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    close(fd);
}

// ‼️：在 EPOLLONESHOT 下，fd 被触发一次后就“失效”，不会再收到 EPOLLIN/EPOLLOUT；
// 修改文件描述符，进行“重置”。使EPOLLIN/EPOLLOUT事件能被触发
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP; // EPOLLET：边缘触发，EPOLLONESHOT：防止同一个通信被不同的线程处理，EPOLLRDHUP：表示读端关闭连接
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}

// 所有的客户数
int http_conn::m_user_count = 0;
// 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
int http_conn::m_epollfd = -1; // static需要单独初始化

// 关闭连接
void http_conn::close_conn() {
    // 防御性编程，因为close_conn()会在多个地方被调用
    if(m_sockfd != -1) { // 避免重复关闭，若m_sockfd已经为-1，说明连接已经关闭
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1; // 第一次关闭连接后，置为-1
        m_user_count--; // 关闭一个连接，将客户总数量-1
    }
}

// 初始化连接,外部调用初始化套接字地址
// 做几件事：1. 设置接收端口复用 2. 添加到epoll中 3. 增加用户计数 4. 初始化连接
void http_conn::init(int sockfd, const sockaddr_in& addr){
    m_sockfd = sockfd;
    m_address = addr;
    
    // 端口复用
    int reuse = 1; // 对 accept 得到的连接套接字 再设 SO_REUSEADDR，一般没有和 listenfd 同样的必要性，真正关键的是 listenfd 上的那一次。
    setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) ); // setsockopt：设置套接字选项, SOL_SOCKET：套接字选项所在的协议层, SO_REUSEADDR：允许重用本地地址
    addfd( m_epollfd, sockfd, true );
    m_user_count++;
    init(); // 使用了重载函数，初始化连接
}

void http_conn::init()
{

    bytes_to_send = 0;
    bytes_have_send = 0;

    m_check_state = CHECK_STATE_REQUESTLINE;    // 初始状态为检查请求行
    m_linger = false;       // 默认不保持链接  Connection : keep-alive保持连接

    m_method = GET;         // 默认请求方式为GET
    m_url = 0;              
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;

    bzero(m_read_buf, READ_BUFFER_SIZE); // bzero是字节操作函数，将m_read_buf清零，大小为READ_BUFFER_SIZE，<string.h>中
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}

// 【非阻塞】循环读取客户数据，直到无数据可读或者对方关闭连接
// 非阻塞的核心三点：1. while循环 2. socket包下的recv是非阻塞的 3. 针对bytes_read检测并选择继续或结束
// 当bytes_read == -1时，表示读取失败，需要检查errno，如果errno == EAGAIN || errno == EWOULDBLOCK，表示没有数据可读，需要继续读取
//     直接break，到达return true，继续 while(true)
// 当bytes_read == 0时，表示对方关闭连接
//     直接return false，关闭连接
// 当bytes_read > 0时，表示读取成功，将数据保存到m_read_buf + m_read_idx处，然后更新m_read_idx
// 最后，如果m_read_idx >= READ_BUFFER_SIZE，表示读取失败，直接return false
bool http_conn::read() {
    if( m_read_idx >= READ_BUFFER_SIZE ) {
        return false;
    }
    int bytes_read = 0;
    while(true) {
        // 从m_read_buf + m_read_idx索引出开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
        // recv：
        // sockfd【int】
        // buf【void*】：接收数据的缓冲区
        // len【size_t】：缓冲区大小（最多接收多少字节）
        // flags【int】：通常0
        // 另外，m_read_buf + m_read_idx等价于&m_read_buf[m_read_idx]，是char*的，但这里发生了隐式转换
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, 
        READ_BUFFER_SIZE - m_read_idx, 0 );
        if (bytes_read == -1) {
            if( errno == EAGAIN || errno == EWOULDBLOCK ) { // 暂时没数据（非阻塞模式的正常情况）
                // 没有数据
                break; // go to 146: return true
            }
            return false;   
        } else if (bytes_read == 0) {   // 对方关闭连接
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
}

// 解析一行，判断依据\r\n
// LINE_OK：找到完整行：\r\n
// LINE_BAD：行错误：只有 \r 没有 \n，或 \n 前面不是 \r
// LINE_OPEN：行数据尚且不完整：既不是 \r 也不是 \n，或 \r是最后一个字符
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    // 按行解析相关的是m_checked_idx和m_read_idx，前者为在前，往后移动解析，后者标志数据尾
    for ( ; m_checked_idx < m_read_idx; ++m_checked_idx ) {
        temp = m_read_buf[ m_checked_idx ];
        if ( temp == '\r' ) { // \r 是缓冲区的最后一个字符
            if ( ( m_checked_idx + 1 ) == m_read_idx ) { // 如果\r是最后一个字符，则行数据尚且不完整
                return LINE_OPEN;
            } else if ( m_read_buf[ m_checked_idx + 1 ] == '\n' ) { // 如果\r后面是\n，则找到完整行
                m_read_buf[ m_checked_idx++ ] = '\0'; // 将\r和\n替换为\0，表示字符串结束，这样就能用 C 字符串函数处理这一行
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } else if( temp == '\n' )  { // 如果\r后面是\n，则找到完整行
            if( ( m_checked_idx > 1) && ( m_read_buf[ m_checked_idx - 1 ] == '\r' ) ) { // 如果\n前面是\r，则找到完整行
                m_read_buf[ m_checked_idx-1 ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 解析HTTP请求行，获得请求方法，目标URL,以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t"); // 判断第二个参数中的字符哪个在text中最先出现
    if (! m_url) { 
        return BAD_REQUEST;
    }
    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';    // 置位空字符，字符串结束符
    char* method = text;
    if ( strcasecmp(method, "GET") == 0 ) { // 忽略大小写比较
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }
    // /index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_version = strpbrk( m_url, " \t" );
    if (!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    if (strcasecmp( m_version, "HTTP/1.1") != 0 ) {
        return BAD_REQUEST;
    }
    /**
     * http://192.168.110.129:10000/index.html
    */
    if (strncasecmp(m_url, "http://", 7) == 0 ) {   
        m_url += 7;
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
        m_url = strchr( m_url, '/' );
    }
    if ( !m_url || m_url[0] != '/' ) {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER; // 检查状态变成检查头
    return NO_REQUEST;
}

// 解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {   
    // 遇到空行，表示头部字段解析完毕
    if( text[0] == '\0' ) {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if ( m_content_length != 0 ) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            m_linger = true;
        }
    } else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    } else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // 处理Host头部字段
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    } else {
        printf( "oop! unknow header %s\n", text );
    }
    return NO_REQUEST;
}

// 我们没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content( char* text ) {
    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主状态机，进行状态解析，返回请求解析结果。具体地，在 m_read_buf 里“按阶段”解析出一个完整 HTTP 请求；如果还没收全就返回 NO_REQUEST，收全了就调用 do_request() 去定位资源文件并返回对应结果码。
http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK; 
    HTTP_CODE ret = NO_REQUEST; 
    char* text = 0;
    // ‼️while+主从状态机来实现双状态解析：“行模式” / “body模式”
    // 【行模式】每次循环都先 parse_line()，只有拿到一整行（LINE_OK）才进入 switch 解析该行。parse_line()：先预先进行判断，当前行是否完整，不完整的话，就继续读取下一行。完整则读取，并改造，这样get_line() 拿到的就是一条以 \0 结尾的 C 字符串行
    // 【body模式】body 不靠 \r\n 分行，所以不再调用 parse_line()；而是直接用 parse_content() 按长度判断是否收全。
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
                || ((line_status = parse_line()) == LINE_OK)) { 
        // 获取数据：三要素 1. 数据源【m_read_buf】 2. 纵向偏移【m_start_line】 3. 横向偏移【m_checked_idx】
        text = get_line(); // char* get_line() { return m_read_buf + m_start_line; } m_read_buf是socket 读进来的原始字节，m_start_line是当前要解析的这一行
        m_start_line = m_checked_idx; // 更新当前要解析的这一行的起始位置
        printf( "got 1 http line: %s\n", text );

        switch ( m_check_state ) {
            case CHECK_STATE_REQUESTLINE: { // 解析请求行，例如 "GET /index.html HTTP/1.1"
                ret = parse_request_line( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {
                ret = parse_headers( text ); // 解析请求头，更新成员变量（m_linger/m_content_length/m_host 等）
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                } else if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT: { // 用 m_read_idx >= m_content_length + m_checked_idx 判断 body 是否完整到达（m_checked_idx 此时已经指向 body 起始附近）。
                ret = parse_content( text ); // 
                if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

// 在 process_read() 判定“请求完整”后被调用，作用是把 URL 映射到本地文件，检查文件是否可用，然后把文件 mmap 到内存，为后续 writev 发送做准备。
http_conn::HTTP_CODE http_conn::do_request()
{
    // 时序接口：/state 返回当前 step_id、result 与按序处理结果；/tick?step=&value= 入队，step 为客户端时序
    if ( m_url && strcmp( m_url, "/state" ) == 0 ) {
        return STATE_REQUEST;
    }
    if ( m_url && strncmp( m_url, "/tick", 5 ) == 0 ) {
        double value = 1.0;
        const char* val_str = strstr( m_url, "value=" );
        if ( val_str )
            value = atof( val_str + 6 );
        int step = 0;
        const char* step_str = strstr( m_url, "step=" );
        if ( step_str )
            step = atoi( step_str + 5 );
        if ( !step_str || step < 0 )  // 未传 step 或非法（负值）：退化为服务器自增序号；step=0 为合法起始步
            step = g_tick_step_id++;
        if ( g_tick_queue )
            g_tick_queue->push( TickData( value, step ) );
        return TICK_REQUEST;
    }

    // "/home/nowcoder/webserver/resources"
    strcpy( m_real_file, doc_root ); // 拼出真实文件路径：doc_root + m_url
    int len = strlen( doc_root );
    // m_url：请求行解析出的路径，（例如 "/index.html"）
    // m_real_file：最终路径，例如 "/home/nowcoder/webserver/resources/index.html"）
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 ); // strncpy：将m_url复制到m_real_file + len处，长度为FILENAME_LEN - len - 1
    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    if ( stat( m_real_file, &m_file_stat ) < 0 ) { // stat() 判断文件是否存在、并拿到文件元信息，在<stat.h>中
        return NO_RESOURCE;
    }

    // 判断访问权限
    // 三位八进制就对应 rwxrwxrwx，例如 chmod 754 file，owner=7=4+2+1 → rwx，group=5=4+1 → r-x，other=4=4 → r--
    // 对应着，754的4，代表other只有4，那么在S_IROTH中，定义宏为0000004，前面的四个0代表八进制，而后面的004，代表other的4，也就是，r--
    // 
    if ( ! ( m_file_stat.st_mode & S_IROTH ) ) { // 假设st_mode是0754，那么证明，owner是rwx、group是r-x、others是r--，与S_IROTH，即0004，来&，结果为1，说明others有读权限。那么，如果要判断，owner是否有写权限，那么就是0754 & 0200。
        //   0754：111 101 100
        // & 0400: 100 000 000
        // 结果：100 000 000，即0400，非0
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode ) ) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open( m_real_file, O_RDONLY );
    // 创建内存映射
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    // mmap：把文件的一部分或全部映射到进程的虚拟地址空间里，直接用返回的指针就可以高效访问内容，不用自己read()到缓冲区
    // 参数：
    // 第一个：映射起始地址，传 0 表示“由内核选一块空闲虚拟地址”，返回值就是这块区域的起始地址。
    // 第二个：映射长度（字节）
    // 第三个：保护标志（PROT_READ）：只读
    // 第四个：映射类型（MAP_PRIVATE）：私有映射，对映射内容的修改不会反映到原始文件。
    // 第五个：文件描述符（fd）：要映射的文件。
    // 第六个：文件内偏移（0）：从文件开头映射，所以是 0。
    // 【mmap的底层原理】
    // 在调用mmap的时候，文件仍在外存上，只是在进程的虚拟地址空间里，划出一段“虚地址区间”，让这段区间“背后对应的是磁盘上那个文件”。
    // 映射好后，当使用m_file_address读取文件内容时，操作系统会在“虚地址区间”里取，如果没有，就会触发缺页中断，然后操作系统从磁盘上读取文件内容到“虚地址区间”里。
    // 所以说，文件实体一直在硬盘上，映射后，当实际读取到它的时候，按需把对应的一页页数据从磁盘（或页缓存）载入物理内存，再映射到你的虚地址上；不是在 mmap 调用时整份文件拷贝到用户空间。
    close( fd ); // 不会解除已经建立好的 mmap；解除要等 munmap(m_file_address, ...) 或进程退出
    return FILE_REQUEST;
}

// 对内存映射区执行munmap操作
void http_conn::unmap() {
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}

// 写HTTP响应
// EPOLLIN：可读，表示：1. 接收缓冲区里已经有数据，可以 read()/recv() 而不阻塞 2. 对端调用了 close()，读会返回 0。
// EPOLLOUT：可写，表示：发送缓冲区有空间，write()/send()/writev() 至少能写一部分而不阻塞；你代码里：events[i].events & EPOLLOUT 时调用 users[sockfd].write()，把响应头 + 文件发出去；若一次写不完（writev 返回 -1 且 errno == EAGAIN），就继续监听 EPOLLOUT，下次再写。
bool http_conn::write()
{
    int temp = 0;
    
    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        modfd( m_epollfd, m_sockfd, EPOLLIN ); // 若为 0，说明上一次已经发完，这里只是再被调了一次：把 fd 改回监听 EPOLLIN（继续收请求），init() 清空连接状态，返回 true。
        init();
        return true;
    }

    while(1) {
        // 分散写
        temp = writev(m_sockfd, m_iv, m_iv_count); // 将m_iv数组中的数据写入m_sockfd，m_iv_count是数组长度，返回的是写入的字节数
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) { // EAGAIN：内核写缓冲区满（非阻塞下常见），不能继续写。
                modfd( m_epollfd, m_sockfd, EPOLLOUT ); // fd修改为继续监听可写，可以往里写数据（写缓冲区有空间）
                return true;
            }
            unmap(); // 如果是其他错误，则解除内存映射，并返回 false
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if (bytes_have_send >= m_iv[0].iov_len) // 头全部发完
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else // 头未发完，继续发头，头发完再发体
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        if (bytes_to_send <= 0)
        {
            // 没有数据要发送了
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN); // 改为可读

            if (m_linger) // keep-alive的标志
            {
                init(); // 清空本连接解析/发送状态
                return true; // 保留连接
            }
            else
            {
                return false;
            }
        }

    }

    
}

// 往写缓冲区 m_write_buf 里拼 HTTP 响应：状态行、各种头、空行、body，都通过 add_response 按格式追加，不直接发 socket。
// 真正发数据是后面 write() 里用 writev 把 m_write_buf（和 mmap 的文件）一起发出去。
// 【待!需要逐行学习】
bool http_conn::add_response( const char* format, ... ) {
    if( m_write_idx >= WRITE_BUFFER_SIZE ) {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) ) {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    return true;
}

// 生成HTTP状态行，HTTP/1.1 200 OK\r\n，status是数字，title是OK、Not Found
bool http_conn::add_status_line( int status, const char* title ) {
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

// 拼 HTTP 响应头：Content-Length、Content-Type、Connection（keep-alive 或 close）、空行。
bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 预写入，在内存里“预写好”响应 + 告诉后面“要发哪几块、一共多少字节”。
// 不调用 send / write / writev，不做任何网络 I/O，只做两件事：
// 1. 拼响应内容：用 add_status_line、add_headers、add_content 等把状态行、头、正文按 HTTP 格式写进 m_write_buf，并维护 m_write_idx。
// 2. 设置“待发送”的元数据：1. 错误/纯文本响应 2. FILE_REQUEST
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case TICK_REQUEST: {
            const char* body = "OK\n";
            add_status_line( 200, ok_200_title );
            add_headers( (int)strlen( body ) );
            if ( ! add_content( body ) ) return false;
            break;
        }
        case STATE_REQUEST: {
            int sid = g_tick_state ? g_tick_state->getStepId() : 0;
            double r = g_tick_state ? g_tick_state->getResult() : 0.0;
            std::string disp = g_tick_state ? g_tick_state->getDisplay() : "";
            char body_buf[8192];
            const char* pre = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>时序信号 · 状态</title><style>"
                "*{box-sizing:border-box}body{margin:0;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#f5f5f7;color:#1d1d1f;min-height:100vh}.page{max-width:960px;margin:0 auto;padding:32px 24px}.page h1{font-size:28px;font-weight:600;margin:0 0 8px;letter-spacing:-0.5px}.page .sub{font-size:14px;color:#86868b;margin:0 0 24px;line-height:1.5}.layout{display:flex;gap:24px;margin-top:8px}.main{flex:1;min-width:0}.card{background:#fff;border-radius:12px;padding:24px;box-shadow:0 2px 12px rgba(0,0,0,.06)}#stateChart{max-width:100%;height:320px}#stateChartHint{font-size:13px;color:#86868b;margin:12px 0 0}.sidebar{width:280px;flex-shrink:0}.sidebar .card{padding:24px}.sidebar h3{margin:0 0 20px;font-size:15px;font-weight:600;color:#1d1d1f}.stat{margin:12px 0;font-size:13px}.label{color:#86868b;margin-right:6px}.val{font-weight:500;color:#1d1d1f}.btn{display:inline-block;margin-top:16px;padding:10px 20px;background:#0071e3;color:#fff;text-decoration:none;border-radius:10px;font-size:14px;font-weight:500;transition:background .2s}.btn:hover{background:#0077ed}.sidebar pre{font-size:11px;max-height:260px;overflow:auto;background:#f5f5f7;padding:12px;border-radius:8px;margin:16px 0 0;color:#1d1d1f}</style></head><body><div class=\"page\"><h1>处理后信号</h1><p class=\"sub\">按 step 顺序的处理结果，与发送端信号趋势一致。</p><div class=\"layout\"><main class=\"main\"><div class=\"card\"><canvas id=\"stateChart\" width=\"640\" height=\"320\"></canvas></div><p id=\"stateChartHint\"></p></main><aside class=\"sidebar\"><div class=\"card\"><h3>状态</h3><p class=\"stat\"><span class=\"label\">step_id</span><span class=\"val\">%d</span></p><p class=\"stat\"><span class=\"label\">result</span><span class=\"val\">%.6f</span></p><a href=\"/state\" class=\"btn\">刷新</a><pre id=\"stateDisplay\">";
            const char* suf = "</pre></div></aside></div></div><script src=\"https://cdn.jsdelivr.net/npm/chart.js\"></script><script>"
                "function drawStateChart(){var p=document.getElementById('stateDisplay');var hint=document.getElementById('stateChartHint');"
                "if(!p){if(hint)hint.textContent='缺少 stateDisplay';return;}var raw=(p.innerText||p.textContent).trim();"
                "var t=raw.split(String.fromCharCode(10));var s=[],v=[];for(var i=0;i<t.length;i++){var m=t[i].match(/^(\\d+):\\s*([\\d.-]+)/);if(m){s.push(parseInt(m[1]));v.push(parseFloat(m[2]));}}"
                "if(s.length===0){if(hint)hint.textContent='暂无处理数据，请先发送信号';return;}if(hint)hint.textContent='';"
                "var c=document.getElementById('stateChart').getContext('2d');if(typeof Chart==='undefined'){if(hint)hint.textContent='Chart.js 未加载';return;}"
                "new Chart(c,{type:'line',data:{labels:s,datasets:[{label:'processed',data:v,borderColor:'#0071e3',fill:false,tension:0.1}]},options:{responsive:true,maintainAspectRatio:false,scales:{y:{beginAtZero:false}}}});}"
                "if(document.readyState==='loading'){document.addEventListener('DOMContentLoaded',function(){setTimeout(drawStateChart,50);});}else{setTimeout(drawStateChart,50);}"
                "</script></body></html>";
            int n = snprintf( body_buf, sizeof(body_buf), pre, sid, r );
            if ( n > 0 && n < (int)sizeof(body_buf) ) {
                size_t avail = sizeof(body_buf) - (size_t)n - strlen(suf) - 1;
                size_t copy_len = disp.size() < avail ? disp.size() : avail;
                if ( copy_len > 0 )
                    memcpy( body_buf + n, disp.c_str(), copy_len );
                n += (int)copy_len;
                body_buf[n] = '\0';
                strncat( body_buf, suf, sizeof(body_buf) - (size_t)n - 1 );
            }
            int len = (int)strlen( body_buf );
            add_status_line( 200, ok_200_title );
            add_headers( len );
            if ( len > 0 && ! add_content( body_buf ) ) return false;
            break;
        }
        case FILE_REQUEST:
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;

            bytes_to_send = m_write_idx + m_file_stat.st_size;

            return true;
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

// 线程池里工作线程调用的入口：对“已经读到数据的连接”做“解析请求 → 生成响应 → 切到可写监听”。
// 不在这里真正往 socket 写数据，只是把响应准备好，并让 epoll 在可写时再调 write()。
void http_conn::process() {
    // 1. 解析HTTP请求
    HTTP_CODE read_ret = process_read(); // 从 m_read_buf 里按主状态机解析，返回 HTTP_CODE（如 GET_REQUEST、BAD_REQUEST、FILE_REQUEST 等）。
    if ( read_ret == NO_REQUEST ) { // 说明请求还不完整，需要继续接收数据
        modfd( m_epollfd, m_sockfd, EPOLLIN ); // 继续监听可读，等下次 EPOLLIN 时主线程再 read()，再投进线程池，再次进 process() 接着解析。
        return;
    }
    
    // 2. 生成响应
    // 若write_ret=false，拼响应失败（如写缓冲区满），close_conn() 关连接并 return。
    // 若write_ret=true，拼响应成功，响应已在 m_write_buf（和可选的 m_file_address）里准备好，等待真正发出去。
    bool write_ret = process_write( read_ret ); // process_write(read_ret) 根据解析结果往 m_write_buf 里拼响应（状态行、头、错误页或只拼头），并设置 m_iv / bytes_to_send（若是 FILE_REQUEST 还会把 mmap 的文件块挂到 m_iv[1]）。
    if ( !write_ret ) {
        close_conn();
    }

    // 3. 切到可写监听
    // 主循环里 events[i].events & EPOLLOUT 时就会调 users[sockfd].write()
    // 用 writev 把 m_iv 里的数据（头 + 可选文件）非阻塞地发出去；
    // 若一次发不完（EAGAIN），会继续监听 EPOLLOUT，下次再写。
    modfd( m_epollfd, m_sockfd, EPOLLOUT);
}
// 模拟Proactor的主线程-线程池架构：
// 主线程：epoll_wait 得到 EPOLLIN 后，users[sockfd].read() 把数据读进 m_read_buf，然后 pool->append(users + sockfd)，把这个连接的 http_conn* 当作一个任务丢进线程池的任务队列。
// 线程池：有若干工作线程，从队列里取任务。任意一个空闲的工作线程取到这个任务后，会调用 request->process()（即 http_conn::process()），也就是由“当时抢到这个任务”的那一个线程负责：解析请求、process_write 填好响应、modfd(..., EPOLLOUT)。
// 主线程：再次 epoll_wait 时可能得到 EPOLLOUT，主线程里调 users[sockfd].write() 把数据发出去（写是在主线程完成的，但“准备响应”是在工作线程的 process() 里做的）。