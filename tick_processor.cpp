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
    if (tick_data.getStepId() <= step_id) {
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
    out = queue.front();
    queue.pop();
    queue_locker.unlock();
    return true;
}

TickQueue::TickQueue(size_t max_cap) : stop(false), max_size(max_cap) {}
TickQueue::~TickQueue() { set_stop(); }

void TickQueue::set_stop() {
    queue_locker.lock();
    stop = true;
    queue_cond.broadcast();
    queue_locker.unlock();
}

bool TickQueue::wait_and_pop(TickData& out) {
    queue_locker.lock();
    while (queue.empty() && !stop) {
        queue_cond.wait(queue_locker.get());
    }
    if (stop && queue.empty()) {
        queue_locker.unlock();
        return false;
    }
    out = queue.front();
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
    queue_cond.signal();
    queue_locker.unlock();
    return true;
}

size_t TickQueue::size() const {
    queue_locker.lock();
    size_t size = queue.size();
    queue_locker.unlock();
    return size;
}