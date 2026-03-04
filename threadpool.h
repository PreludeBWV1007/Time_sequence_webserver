#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "locker.h"

// 一般类：声明放 .h，定义放 .cpp 是对的。编译时每个 .cpp 单独编译，最后链接；只要在某个 .cpp 里有一份函数定义，链接器就能找到。
// 模板类：实现必须和声明放在一起，否则在编译 main.cpp 时，只看到 .h 里的声明，没有函数体，无法实例化。

// 线程池类，将它定义为模板类是为了代码复用，模板参数T是任务类
template<typename T>
class threadpool {
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T* request);

private:
    // 【线程池设计模式：static 入口 + 转调成员函数】所有线程共同通过worker的接口，共享同一套队列，由锁和信号量来实现并发run任务。

    // 底层：所有线程在“刚启动时”统一使用的入口：签名是 C 的 void* (*)(void*)，才可以通过pthread_create。
    // static：pthread_create 要的是“普通函数指针”void* (*)(void*)，非静态成员函数有一个隐藏的 this 参数，调用约定不同，不能当这种函数指针用；静态成员函数没有 this，签名就是 void* (*)(void*)，可以当线程入口，然后把this按照参数传进去。
    // void* arg：传 this（转成 void*），入口里把 void* 转回 threadpool*，worker 用 arg 找到“是哪个 pool”，再调 pool->run()。
    // 单例性：每个threadpool只有一个worker，不同线程都是传入相同的this，但每个线程的run()都是独立的，互不干扰，执行任务都是不同的，这一点通过互斥锁和信号量来实现。
    static void* worker(void* arg);

    // 业务逻辑设计
    // 所有工作线程拿到的都是同一个 pool（同一个 this），进的是同一份 run() 代码、操作同一套队列和成员。
    // 并发安全：
    //  1. 互斥锁：同一时刻只有一个线程能对队列做「取任务 / 判空 / pop」；
    //  2. 信号量：表示「有任务可做」，没任务时线程在 wait() 上阻塞，有任务时被唤醒去抢锁、取任务。
    void run();

private:
    // 线程的数量
    int m_thread_number;  
    
    // 描述线程池的数组，大小为m_thread_number    
    pthread_t * m_threads;

    // 请求队列中最多允许的、等待处理的请求的数量  
    int m_max_requests; 
    
    // 请求队列
    std::list< T* > m_workqueue; // 任务队列：存的是 T*，本项目里就是 http_conn*

    // 保护请求队列的互斥锁
    locker m_queuelocker;   

    // 是否有任务需要处理
    sem m_queuestat;

    // 是否结束线程          
    bool m_stop;                    
};

template< typename T >
threadpool< T >::threadpool(int thread_number, int max_requests) : 
        m_thread_number(thread_number), m_max_requests(max_requests), 
        m_stop(false), m_threads(NULL) {

    if((thread_number <= 0) || (max_requests <= 0) ) {
        throw std::exception();
    }

    m_threads = new pthread_t[m_thread_number];
    if(!m_threads) {
        throw std::exception();
    }

    // 创建thread_number 个线程，并将他们设置为脱离线程。
    for ( int i = 0; i < thread_number; ++i ) {
        printf( "create the %dth thread\n", i);
        if(pthread_create(m_threads + i, NULL, worker, this ) != 0) {
            delete [] m_threads;
            throw std::exception();
        }
        
        if( pthread_detach( m_threads[i] ) ) {
            delete [] m_threads;
            throw std::exception();
        }
    }
}

template< typename T >
threadpool< T >::~threadpool() {
    delete [] m_threads;
    m_stop = true;
}

template< typename T >
bool threadpool< T >::append( T* request )
{
    // 加锁，m_workqueue被多个线程共享，主线程写，工作线程读
    m_queuelocker.lock();
    if ( m_workqueue.size() > m_max_requests ) {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

template< typename T >
void* threadpool< T >::worker( void* arg ) // pthread_create 要求入口是 void* (*)(void*)，所以需要这个静态函数
{
    threadpool* pool = ( threadpool* )arg; // 传入的其实是this，this是一个指针，自然后面被给到了pool。这个形式也只是为了匹配pthread的函数指针。
    pool->run();
    return pool;
}

template< typename T >
void threadpool< T >::run() {

    while (!m_stop) {
        m_queuestat.wait(); // 等待信号量，若为0，阻塞
        m_queuelocker.lock();
        if ( m_workqueue.empty() ) {
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if ( !request ) {
            continue;
        }
        request->process();
    }

}

#endif