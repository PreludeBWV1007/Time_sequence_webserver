/**
 * 时序模块小 demo：多线程往 TickQueue push，单线程池顺序消费并更新 TickState。
 * TickQueue 内部为按 step_id 的小根堆，故无论 push 顺序如何，消费者始终按 step_id 从小到大取到，result 为全量累加。
 */
#include "../singlethreadpool.h"  // 已含 tick_processor.h
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

static const int kProducerCount = 4;   // 模拟 4 个“客户端”线程
static const int kPushesPerProducer = 5;
static std::atomic<int> g_step_id{1};  // 全局递增的 step_id，保证入队顺序可被状态机按序接受

// 多线程共用一个 cout 会打乱顺序；加锁后每次打印完整一行，便于人工核对
static std::mutex g_cout_mutex;
static void log(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_cout_mutex);
    std::cout << msg << std::endl;
}

void producer(TickQueue& queue, int producer_id) {
    for (int i = 0; i < kPushesPerProducer; ++i) {
        int step = g_step_id++;
        double value = 1.0;  // 每步贡献 1.0，最终 result 约为 kProducerCount * kPushesPerProducer
        queue.push(TickData(value, step));
        log("producer " + std::to_string(producer_id) + " push step=" + std::to_string(step));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));  // 稍微错开，模拟并发
    }
}

int main() {
    TickQueue queue(128);
    TickState state;

    // 回调：用 old_result + tick_data 得到 new_result，再 update（只有 step_id 更大才接受）
    auto on_tick = [&state](double old_result, const TickData& td) {
        double new_result = old_result + td.getValue();
        if (state.update(td, new_result))
            log("consumer: step_id=" + std::to_string(td.getStepId()) + " result=" + std::to_string(static_cast<int>(new_result)));
    };

    singlethreadpool pool(queue, state, on_tick);

    std::thread producers[kProducerCount];
    for (int i = 0; i < kProducerCount; ++i)
        producers[i] = std::thread(producer, std::ref(queue), i);
    for (int i = 0; i < kProducerCount; ++i)
        producers[i].join();

    queue.set_stop();  // 通知消费者队列已关闭，消费完当前剩余后 wait_and_pop 返回 false，run() 退出
    // pool 析构会 join 消费者线程
    log("final: step_id=" + std::to_string(state.getStepId()) + " result=" + std::to_string(static_cast<int>(state.getResult())));
    return 0;
}
