# 高并发 Web 服务器

基于 Linux epoll 的 Proactor 风格高并发 HTTP 服务器，使用线程池处理请求解析与响应组装，支持 GET 请求与静态资源服务。项目同时包含一套「顺序请求接收 + 单线程顺序处理」的时序处理模块（TickQueue / TickState / singlethreadpool），可用于需要严格按序更新状态的场景。

## 特性

- **Proactor 模型**：主线程负责所有 I/O（accept、read、write），工作线程仅做请求解析与响应组装，写回仍在主线程完成
- **epoll + 非阻塞 I/O**：边缘触发（ET）、EPOLLONESHOT，单 epoll 实例支撑高并发连接
- **线程池**：可配置工作线程数（默认 8）与最大待处理请求数（默认 10000），任务类型为 `http_conn*`
- **HTTP 解析**：主/从状态机解析请求行、头部与请求体，支持 GET、Keep-Alive
- **时序处理模块**：TickQueue（有界阻塞队列）、TickState（按 step_id 顺序更新）、singlethreadpool（单工作线程顺序消费），支持多生产者单消费者

## 架构概览

```
                    main thread (epoll_wait)
                           |
        +------------------+------------------+
        |                  |                  |
   listenfd            EPOLLIN            EPOLLOUT
        |                  |                  |
     accept()            read()              write()
        |                  |                  |
   users[fd].init()   pool->append()    writev() 发响应
                            |
                     threadpool<http_conn>
                            |
                     http_conn::process()
                     (parse + process_write + modfd EPOLLOUT)
```

- **连接**：`listenfd` 可读 → `accept` → `http_conn::init()`，并加入 epoll 监听
- **读**：EPOLLIN → `read()` 将数据读入 `m_read_buf`，然后 `append(users + sockfd)` 投递到线程池
- **处理**：工作线程执行 `process()`（`process_read` + `process_write`），最后 `modfd(..., EPOLLOUT)`，不在此处写 socket
- **写**：EPOLLOUT → 主线程 `write()`，用 `writev` 发送响应头与 mmap 的响应体

## 项目结构

```
.
├── main.cpp              # 入口：epoll 循环、accept/read/write 分发
├── http_conn.cpp         # HTTP 连接：读/写/解析/响应组装、epoll 工具函数
├── http_conn.h
├── threadpool.h          # 多线程任务池（模板类，任务为 http_conn*）
├── locker.h              # 互斥锁、条件变量、信号量封装
├── tick_processor.h      # 时序处理：TickState、TickData、TickQueue
├── tick_processor.cpp
├── singlethreadpool.h    # 单工作线程池，顺序消费 TickQueue 并执行 heavy_compute
├── singlethreadpool.cpp
├── resources/            # 静态资源根目录（可配置 doc_root）
├── test_presure/         # 压测工具 webbench
└── noactive/             # 非活动连接与定时器相关（lst_timer 等）
```

## 依赖与编译

- **环境**：Linux，支持 epoll（如 Ubuntu / CentOS）
- **编译**：C++17，需 pthread

```bash
# 使用 Makefile（推荐）
make
./server <端口号>

# 或手动编译
g++ -std=c++17 -pthread -o server main.cpp http_conn.cpp
```

编译产物为可执行文件 `server`。

## 运行与配置

```bash
./server 8080
```

- 默认网站根目录在 `http_conn.cpp` 中通过 `doc_root` 指定，如需修改请编辑该常量后重新编译。
- 压测可使用 `test_presure/webbench-1.5` 下的 webbench。

## 时序处理模块用法示例

适用于「请求按序入队、单线程按序处理并更新共享状态」的场景：

```cpp
#include "tick_processor.h"
#include "singlethreadpool.h"

TickQueue queue(1000);   // 有界队列，容量 1000
TickState state;

singlethreadpool pool(queue, state, [&state](double old_r, const TickData& td) {
    double r = old_r + td.getValue();  // 或调用自定义 heavy_compute
    state.update(td, r);
});

// 其他线程可并发 push
queue.push(TickData(1.0, 1));
queue.push(TickData(2.0, 2));

// 析构 pool 时会 set_stop 并 join 工作线程
```

当前主程序 `main.cpp` 仅启动 HTTP 服务，未集成 TickQueue/singlethreadpool；若需在 HTTP 路径中使用时序处理，可在读请求后构造 `TickData` 并 `queue.push()`，由 singlethreadpool 统一顺序处理。

## 许可证

请根据项目需要自行添加 LICENSE 文件。
