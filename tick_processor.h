#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <iostream>
#include <chrono>
#include <atomic>
#include <functional>
#include <vector>
#include <memory>
#include <future>
#include <algorithm>
#include <numeric>
#include <random>
#include "locker.h"

using namespace std;

class TickData;  // 前向声明，供 TickState::update 使用

/** 共享状态：只由单消费者线程写，其他线程可加锁读。
 *  update() 时用 step_id 拒绝乱序（只接受 tick_data.getStepId() > step_id），保证状态按步序单调前进。 */
class TickState {
public:
    TickState() = default;
    ~TickState() = default;
    bool update(const TickData& tick_data, double new_result);
    int getStepId() const;
    double getResult() const;

private:
    mutable locker state_locker;  // mutable：getResult/getStepId 为 const 成员，但需在内部加锁读
    double result = 0;
    int step_id = 0;
};

/** 一条时序数据：第 step_id 步、数值 value。 */
class TickData {
public:
    TickData() : value(0), step_id(-1) {}
    TickData(double value, int step_id);
    ~TickData();
    double getValue() const;
    int getStepId() const;
private:
    double value;
    int step_id;
};

/** 有界阻塞队列，多生产者-单消费者（MPSC）。
 * 多个线程可并发调用 push（会轮流抢锁，互斥入队）；只有单线程应调用 wait_and_pop（如 singlethreadpool 的工作线程）。
 * 锁不禁止“多 push”，而是保证同一时刻只有一个线程在操作 queue，避免竞态。 */
class TickQueue {
public:
    explicit TickQueue(size_t max_size = 1000);
    ~TickQueue();
    bool push(const TickData& tick_data);
    /** 取出一条数据；若队列已关闭且为空则返回 false，否则取出并返回 true。 */
    bool wait_and_pop(TickData& out);
    bool try_pop(TickData& out);
    size_t size() const;
    /** 关闭队列，唤醒所有在 wait_and_pop 里阻塞的线程，使其返回 false。 */
    void set_stop();

private:
    std::queue<TickData> queue;
    mutable locker queue_locker;  // mutable：size() 为 const 成员，但需在内部加锁读 queue
    cond queue_cond;              // 队列空时 wait_and_pop 在此等待；push 后 signal 唤醒
    bool stop = false;
    size_t max_size;
};