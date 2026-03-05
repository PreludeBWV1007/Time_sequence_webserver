#include "tick_processor.h"
#include <cmath>
#include <thread>
#include <chrono>

using namespace std;

// --- TickData ---
TickData::TickData(double value, int step_id) : value(value), step_id(step_id) {}
TickData::~TickData() = default;
double TickData::getValue() const { return value; }
int TickData::getStepId() const { return step_id; }

// --- TickState ---
int TickState::getStepId() const {
    state_locker.lock();
    int id = step_id;
    state_locker.unlock();
    return id;
}

double heavy_compute(const TickData& tick_data, double old_result) {
    // 模拟耗时操作
    this_thread::sleep_for(chrono::milliseconds(100));
    return old_result + tick_data.getValue();
}

bool TickState::update(const TickData& tick_data, double new_result) {
    state_locker.lock();
    if (tick_data.getStepId() <= step_id) {  // 拒绝乱序或重复：只接受步序更大的更新
        state_locker.unlock();
        return false;
    }
    result = new_result;
    step_id = tick_data.getStepId();
    state_locker.unlock();
    return true;
}

double TickState::getResult() const {
    state_locker.lock();
    double result = this->result;
    state_locker.unlock();
    return result;
}

bool TickQueue::try_pop(TickData& out) {
    queue_locker.lock();
    if (queue.empty()) {
        queue_locker.unlock();
        return false;
    }
    out = queue.top();
    queue.pop();
    queue_locker.unlock();
    return true;
}

// 以下：push / wait_and_pop 都先加同一把 queue_locker，故同一时刻只有一个线程在改 queue。
// “多 push”= 多个线程都可调 push，但会轮流抢锁；“单 pop”= 只有 singlethreadpool 那一个工作线程在调 wait_and_pop。

TickQueue::TickQueue(size_t max_cap) : stop(false), max_size(max_cap) {}
TickQueue::~TickQueue() { set_stop(); }

void TickQueue::set_stop() {
    queue_locker.lock();
    stop = true;
    queue_cond.broadcast();
    queue_locker.unlock();
}

// 调用方：只有 singlethreadpool::run() 会调本函数，即“单 pop”的入口在 singlethreadpool.cpp，不在此文件。当singlethreadpool被构造时，会有pthread_create(..., worker, this) 起一个工作线程，该线程执行 worker() → pool->run()。该线程会一直循环 wait_and_pop，直到 stop 被设置为 true。
bool TickQueue::wait_and_pop(TickData& out) {
    queue_locker.lock();   // 若锁被 push/其他线程占用则当前线程阻塞，直到拿到锁
    while (queue.empty() && !stop) {
        // get() 只把互斥量指针传给 cond，不负责加锁。
        queue_cond.wait(queue_locker.get()); // wait() 语义：先释放锁、再睡眠；被 signal 唤醒后重新抢锁再返回。
    }
    if (stop && queue.empty()) {
        queue_locker.unlock();
        return false;
    }
    out = queue.top();
    queue.pop();
    queue_locker.unlock();
    return true;
}

bool TickQueue::push(const TickData& tick_data) {
    queue_locker.lock();
    if (queue.size() >= max_size) {
        queue_locker.unlock();
        return false;
    }
    queue.push(tick_data);
    queue_cond.signal();  // 若有线程在 wait_and_pop 里等，唤醒一个
    queue_locker.unlock();
    return true;
}

size_t TickQueue::size() const {
    queue_locker.lock();
    size_t size = queue.size();
    queue_locker.unlock();
    return size;
}