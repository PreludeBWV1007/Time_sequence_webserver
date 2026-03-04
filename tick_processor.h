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

// 全局状态，新结果依赖旧状态和到达数据，产生新状态，更新。既给消费者用，也给观察者用（GET看一看的时候）。
// 访问方式
// 消费者线程：写独占，先读再写
// 观察者线程：加锁读？
class TickState {
public:
    TickState() = default;
    ~TickState() = default;
    bool update(const TickData& tick_data, double new_result);
    int getStepId() const;
    double getResult() const;

private:
    mutable locker state_locker;
    // 多类结果，可扩展为结构体
    double result = 0;
    int step_id = 0;
};

// 小数据量：用普通的class存，放栈上
// 大数据量：用智能指针
//    shared_ptr：数据很大，多地共享
//    unique_ptr：数据很大，消费者只消费一次，用move来进行队列元素操作
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

class TickQueue {
public:
    explicit TickQueue(size_t max_size = 1000);
    ~TickQueue();
    bool push(const TickData& tick_data);
    /** 取出一条数据；若队列已关闭且为空则返回 false，否则取出并返回 true */
    bool wait_and_pop(TickData& out);
    bool try_pop(TickData& out);
    size_t size() const;
    /** 关闭队列，唤醒所有等待的 wait_and_pop，使其返回 false */
    void set_stop();

private:
    std::queue<TickData> queue;
    mutable locker queue_locker;
    cond queue_cond;
    bool stop = false;
    size_t max_size;
};