# 时序模块 Demo

## 为何用“实际信号验证”而不是“看日志打印”

多线程下用 `cout` 或 `log()` 打印时，**谁先打印只表示谁先抢到锁**，并不表示事件真实发生的先后。即使给 cout 加锁保证一行不被打断，日志里“producer 3 push step=4”出现在“consumer step_id=1”前面，也未必表示 step=4 先于 step=1 入队，因此**无法从日志顺序反推真实的入队/处理顺序**，也就难以仅靠肉眼判断“时序对不对”。

要证明“在时序信号下系统是正确工作的”，需要一种**不依赖日志先后**的验证方式。思路是：

1. **输入确定**：使用已知、可复现的信号（例如 sin 一个周期等间隔采样），这样“理论上应该得到什么”是事先可算的。
2. **算法已知**：在系统中做的处理是简单、可推导的（例如累加），这样最终状态（如 result）与输入、顺序的关系是明确的。
3. **预期可算**：根据 1 和 2，在单线程、按步序执行的前提下算出预期结果（例如一个周期 sin 和为 0）。
4. **一次比较**：跑完系统后，用 `state.getResult()` 与预期比较；在合理容差内一致则 **PASS**，说明“按我们给的信号和算法，系统给出的结果与理论一致”，即**在此时序信号下系统行为正确**，而无需从日志里推断顺序。

因此我们增加 **demo_sin（方案 A）**：用 sin 信号 + 累加算法 + 理论预期 0 做一次可重复的、不依赖日志的验证；demo_tick 仍保留，用于观察多生产者下的日志与最终状态。

---

## TickQueue：小根堆（按 step_id）

`TickQueue` 内部为 **priority_queue（小根堆）**，比较键为 `step_id`。多线程可**乱序** push；`wait_and_pop` 始终取出当前堆中 **step_id 最小**的一条，故消费者得到的是按 step_id 递增的序列（可能缺号）。这样在“线程池多线程入队”时仍保证下游按序处理，且某步永久不到时不会阻塞后续步。

**线程安全**：`std::priority_queue` 本身**不是**线程安全的，多线程无锁并发 push/pop 会数据竞争。对多生产者-单消费者（MPSC）而言，线程安全是必须的。`TickQueue` 通过 **`queue_locker`** 在每次 `push`、`wait_and_pop`、`try_pop`、`size`、`set_stop` 时加锁，保证同一时刻只有一个线程在访问内部堆；并用 **`queue_cond`** 在队列空时阻塞消费者、在 push 后唤醒。因此多生产者与单消费者同时使用 `TickQueue` 是线程安全的，安全由封装在 `TickQueue` 内的锁与条件变量提供，而非 `priority_queue` 自带。

---

## 1. demo_tick：多生产者压测

多线程往 `TickQueue` push，单线程池顺序消费并更新 `TickState`。因队列为小根堆，无论 push 顺序如何，消费者按 step_id 1..20 取到，最终 result=20。

```bash
make demo_tick   # 或 make
./demo_tick
```

流程：4 个生产者各 push 5 条（step_id 全局递增，但调度导致入队顺序乱序），消费者从堆顶依次取最小 step_id 并累加。观察 `final: step_id=20 result=20`。

---

## 2. demo_sin：方案 A，sin 信号验证

**思路**：确定性 sin 信号 + 累加算法，预期一个周期和为 0；不依赖日志即可验证。

- **输入**：step_id=1..N，value = sin(2π·(step_id−1)/N)，N=100。
- **算法**：result += value。
- **预期**：expected=0；容差内一致则 **PASS**。

```bash
make demo_sin
./demo_sin
```

- **[ordered push]**：单生产者按 1..100 顺序 push，消费者按序取，got≈0 → PASS。
- **[disordered push, consumer after]**：先按随机顺序 push 全部 100 条，再启动消费者；堆内已满，弹出顺序为 1..100，got≈0 → PASS，验证小根堆重排有效。
