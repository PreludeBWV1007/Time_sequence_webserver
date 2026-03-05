/**
 * 方案 A：用确定性 sin 信号 + 已知算法（累加）验证时序系统正确性。
 * 输入：step_id=1..N，value=sin(2π·(step_id-1)/N)，即一个周期等间隔采样。
 * 算法：状态为累加和 result = result + value。
 * 预期：一个周期 sin 和为 0，故 expected = 0；若系统按序处理且计算正确，state.getResult() ≈ 0。
 */
#include "../singlethreadpool.h"
#include <iostream>
#include <cmath>
#include <thread>
#include <string>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const int N = 100;   // 一个周期 100 个点
static const double kTol = 1e-5;

static void run_sin_test() {
    TickQueue queue(256);
    TickState state;

    auto on_tick = [&state](double old_result, const TickData& td) {
        double new_result = old_result + td.getValue();
        state.update(td, new_result);
    };

    {
        singlethreadpool pool(queue, state, on_tick);
        // 单生产者、按 step_id 顺序 push，保证队列顺序 = 1,2,...,N，所有 update 都会被接受
        std::thread producer([&queue]() {
            for (int step = 1; step <= N; ++step) {
                double value = std::sin(2.0 * M_PI * (step - 1) / N);
                queue.push(TickData(value, step));
            }
        });
        producer.join();
        queue.set_stop();
    }  // pool 析构，join 消费者，确保全部处理完再读 state

    double got = state.getResult();
    double expected = 0.0;   // 一个周期 sum sin = 0
    bool pass = std::fabs(got - expected) <= kTol;
    std::cout << "signal: sin(2π·(step-1)/" << N << "), step=1.." << N << std::endl;
    std::cout << "algorithm: result += value (running sum)" << std::endl;
    std::cout << "expected = " << expected << ", got = " << got << " (tol=" << kTol << ")" << std::endl;
    std::cout << (pass ? "PASS: 时序系统在本信号与算法下结果与预期一致。" : "FAIL: 结果与预期不符。") << std::endl;
}

int main() {
    run_sin_test();
    return 0;
}
