#include "singlethreadpool.h"

singlethreadpool::singlethreadpool() : stop(false), m_queue(nullptr), m_state(nullptr),
    heavy_compute(nullptr), m_thread(0), m_thread_started(false) {
}

singlethreadpool::singlethreadpool(TickQueue& queue, TickState& state,
    std::function<void(double, const TickData&)> fn)
    : stop(false), m_queue(&queue), m_state(&state), heavy_compute(std::move(fn)),
      m_thread(0), m_thread_started(true) {
    if (pthread_create(&m_thread, NULL, worker, this) != 0) {
        m_thread_started = false;
        m_thread = 0;
    }
}

singlethreadpool::~singlethreadpool() {
    stop = true;
    if (m_queue)
        m_queue->set_stop();
    if (m_thread_started && m_thread) {
        pthread_join(m_thread, NULL);
        m_thread_started = false;
    }
}

void* singlethreadpool::worker(void* arg) {
    singlethreadpool* pool = static_cast<singlethreadpool*>(arg);
    pool->run();
    return NULL;
}

void singlethreadpool::run() {
    if (!m_queue || !m_state || !heavy_compute)
        return;
    while (!stop) {
        TickData tick_data;
        if (!m_queue->wait_and_pop(tick_data))  // 唯一调用 wait_and_pop 的线程，即“单 pop”
            break;
        double old_result = m_state->getResult();
        heavy_compute(old_result, tick_data);   // 回调里应做 state.update(tick_data, new_result)
    }
}
