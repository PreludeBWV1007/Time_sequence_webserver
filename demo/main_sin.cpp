/**
 * 方案 A：用确定性 sin 信号 + 已知算法（累加）验证时序系统正确性。
 * 输入：step_id=1..N，value=sin(2π·(step_id-1)/N)，即一个周期等间隔采样。
 * 算法：状态为累加和 result = result + value。
 * 预期：一个周期 sin 和为 0；TickQueue 为小根堆，故无论 push 顺序如何，消费者按 step_id 取到，结果仍 ≈ 0。
 */
#include "../singlethreadpool.h"
#include <iostream>
#include <cmath>
#include <thread>
#include <string>
#include <vector>
#include <random>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const int N = 100;   // 一个周期 100 个点
static const double kTol = 1e-5;

static void run_sin_test_ordered() {
    TickQueue queue(256);
    TickState state;
    auto on_tick = [&state](double old_result, const TickData& td) {
        double new_result = old_result + td.getValue();
        state.update(td, new_result);
    };
    {
        singlethreadpool pool(queue, state, on_tick);
        std::thread producer([&queue]() {
            for (int step = 1; step <= N; ++step) {
                double value = std::sin(2.0 * M_PI * (step - 1) / N);
                queue.push(TickData(value, step));
            }
        });
        producer.join();
        queue.set_stop();
    }
    double got = state.getResult();
    bool pass = std::fabs(got - 0.0) <= kTol;
    std::cout << "[ordered push] expected=0, got=" << got << " -> " << (pass ? "PASS" : "FAIL") << std::endl;
}

/** 乱序 push：先按随机顺序 push 全部 step 1..N，再启动消费者，堆内已满故弹出顺序为 1..100，结果仍 ≈ 0。 */
static void run_sin_test_disordered() {
    TickQueue queue(256);
    TickState state;
    auto on_tick = [&state](double old_result, const TickData& td) {
        double new_result = old_result + td.getValue();
        state.update(td, new_result);
    };
    std::vector<int> steps(N);
    for (int i = 0; i < N; ++i) steps[i] = i + 1;
    std::mt19937 rng(12345);
    std::shuffle(steps.begin(), steps.end(), rng);

    for (int step : steps) {
        double value = std::sin(2.0 * M_PI * (step - 1) / N);
        queue.push(TickData(value, step));
    }
    queue.set_stop();
    {
        singlethreadpool pool(queue, state, on_tick);
    }
    double got = state.getResult();
    bool pass = std::fabs(got - 0.0) <= kTol;
    std::cout << "[disordered push, consumer after] expected=0, got=" << got << " -> " << (pass ? "PASS" : "FAIL") << std::endl;
}

int main() {
    std::cout << "signal: sin(2π·(step-1)/" << N << "), step=1.." << N << "; algorithm: result += value (tol=" << kTol << ")" << std::endl;
    run_sin_test_ordered();
    run_sin_test_disordered();
    return 0;
}
