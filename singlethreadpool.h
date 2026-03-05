#ifndef SINGLETHREADPOOL_H
#define SINGLETHREADPOOL_H

#include "tick_processor.h"
#include <pthread.h>
#include <functional>

/** 单工作线程池：唯一的工作线程循环 wait_and_pop → getResult → heavy_compute(old, tick_data)。
 *  即 TickQueue 的“单消费者”；heavy_compute 回调内应调用 state.update(tick_data, new_result)。 */
class singlethreadpool {
public:
    singlethreadpool();
    singlethreadpool(TickQueue& queue, TickState& state, std::function<void(double, const TickData&)> heavy_compute);
    ~singlethreadpool();

private:
    static void* worker(void* arg);
    void run();

    bool stop = false;
    TickQueue* m_queue = nullptr;   // 外部队列，带参构造时使用
    TickState* m_state = nullptr;   // 外部状态，带参构造时使用
    std::function<void(double, const TickData&)> heavy_compute;
    pthread_t m_thread = 0;
    bool m_thread_started = false;
};

#endif