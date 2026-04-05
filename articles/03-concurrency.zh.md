# 反向改写：把 Rust 转回 C++

## Part 3：真正难的地方 —— mini-redis

*Su Qingyue*

---

## 为什么会选它

前两篇的结果多少会让人有一个很自然的反驳：

“你挑的都是顺序程序。真正到异步、并发、共享状态这些地方，Rust 和 C++ 的差别才会拉开。”

这个反驳很合理。所以第三篇我干脆去选一个本来就会把问题逼到边上的例子：Tokio 官方教学项目 [mini-redis](https://github.com/tokio-rs/mini-redis)。

如果前面那套判断有破绽，这里最容易暴露。

## 这个项目里到底有什么

mini-redis 虽然是教学项目，但覆盖面一点也不小：

- 异步 TCP accept loop
- 并发连接处理
- 带过期时间的 key-value 存储
- pub/sub 广播
- 优雅停机

它几乎把 Tokio 的那一套核心异步原语都碰了一遍：`spawn`、`select!`、`Notify`、`Semaphore`、`broadcast`、`StreamMap`、`async_stream::stream!` 等等。

| 组件 | Rust 行数 | C++ 行数 | 说明 |
|------|----------:|----------:|------|
| Frame codec | 302 | 265 | RESP 编解码，逻辑几乎一样 |
| Command parser | 149 | 110 | 结构接近 |
| Connection | 236 | 240 | 缓冲策略略有变化 |
| Database | 369 | 387 | 自己补了广播通道 |
| Commands | 945 | 572 | 一批类型层包装塌掉了 |
| Server | 370 | 255 | 停机协调更直接 |
| Shutdown | 49 | 65 | 换了实现方式 |
| Client + CLI | 973 | 372 | API 更收 |
| **总计** | **3,393** | **2,266** | **约少 33%** |

数字上看，C++ 版本依然更短。但这一轮开始，必须给它加一个脚注：**不是所有地方都还能机械地一一替换。**

## 大部分地方还是能顺着转

Tokio 到 C++ 这边，基本运行时映射选的是 standalone ASIO + C++20 coroutine：

- `tokio::spawn` → `asio::co_spawn`
- `async fn` → `asio::awaitable<T>`
- `accept().await` → `co_await async_accept()`

数据库部分也没有什么神秘变化：

- `Arc<Mutex<State>>` → `shared_ptr<Shared>` + `std::mutex`
- 过期时间集合依然是有序结构
- 背景清理任务依然是同一套思路

换句话说，项目里前面七成、八成的部分，仍然可以沿着前两篇那种“找到对应结构，再把它换成 C++ 习惯用法”的路径走。

真正开始卡住的，是 `subscribe`。

## 第一个真正不再机械的点：`tokio::select!`

订阅模式下，客户端会进入一个长生命周期循环，同时等三类事件：

1. 任一已订阅频道的新消息
2. 客户端自己发来的新命令
3. 服务端停机信号

Rust 这里的写法非常干净：

```rust
select! {
    Some((channel, msg)) = subscriptions.next() => { ... }
    res = dst.read_frame() => { ... }
    _ = shutdown.recv() => { return Ok(()) }
}
```

这里的关键不只是 `select!` 本身，还包括它前面那套 `StreamMap`、`Pin<Box<dyn Stream>>` 和 `async_stream::stream!` 组成的动态 fan-in 结构。

问题在于：**C++20 coroutine 这边没有一个真正常量级可替代的 `select!`。**

ASIO 确实有两个 awaitable 之间 race 的办法，但它并不直接覆盖“订阅集合会动态变化”的 fan-in。这个缺口不是“C++ 做不到”，而是“你不能像 Rust 那样，直接把它当成现成原语拿来用”。

## 实际怎么补上的

最后的做法，是自己造了一个共享唤醒点：一个一直挂在 `time_point::max()` 的 `steady_timer`。

这玩意本质上被拿来当异步世界里的条件变量。三类事件谁来了，就 cancel 它，把主循环唤醒：

**广播消息到了。** `BroadcastSender` 给订阅者队列塞数据的同时，也通过弱引用去 cancel 共享 timer。

**socket 可读。** 主循环进入等待前注册 `async_wait(wait_read)`，一旦有网络数据到达，就 cancel timer。

**服务端停机。** 另起一个 watcher coroutine，收到停机信号后同样 cancel timer。

于是主循环就变成：

1. 先尽量把已经到达的广播消息清掉
2. 尝试做非阻塞 socket 读取
3. 解析已经缓冲下来的命令
4. 重置 timer
5. 等下一次 cancel

从行为上看，这套是对的，也没有轮询。但它已经不是“按语法表替换”了，而是你得真的理解：

- 协作式调度是怎么运行的
- timer cancel 为什么能拿来做信号
- socket readability 和非阻塞读如何配合
- 动态订阅集合怎么聚合成一个唤醒源

这一步开始，翻译就从“对应关系”进入了“并发设计”。

## 一个很小、但很说明问题的 bug

转写中间还碰到过一个挺典型的问题：连接限制器。

Rust 里用的是 `Semaphore`，等待 permit 时是 `acquire().await`，也就是异步暂停。

一开始的 C++ 版本图省事，直接用了 `std::counting_semaphore::acquire()`。这个调用在多线程环境下只是堵一个线程；但在单线程 `io_context` 里，它会把整个事件循环一把堵死。表现出来就是：服务端看起来“什么都没报错，但就是不动了”。

这个 bug 修起来不难，但它提醒了我一件事：

**Rust 的类型和 async 语义在这里不只是“写法不同”，它会直接把一类误用排除掉。**

C++ 里两个调用看起来都像普通函数调用；Rust 里同步和异步 acquire 根本不是一个层面的东西。

## 哪些抽象在这里明显塌掉了

| Rust | C++ | 发生了什么 |
|------|-----|------------|
| `Pin<Box<dyn Stream<Item=Bytes>>>` | 消失 | 直接对接收器做轮询 |
| `StreamMap<String, Messages>` | `vector<Subscription>` | 不再包成统一 stream |
| `async_stream::stream!` | 消失 | 生成器包装没了 |
| `Bytes` | `std::string` | 这里深拷贝成本可接受 |
| `Send + Sync` | 默认假定 | 单线程上下文里不再显式出现 |
| `broadcast::Receiver` 转 `Stream` | `try_recv()` | 直接拉消息而不是套适配层 |

这些减少的行数，仍然不太像“C++ 更有表达力”，而更像“Rust 为了让编译器能验证并发正确性，必须把一层结构写出来；C++ 这边没有那套证明要求，所以很多包装就塌了”。

## 这一篇最重要的修正

到 mini-redis 为止，我觉得前两篇的说法需要被修正得更精确一点：

**不是说 Rust 和 C++ 在所有地方都只是“机械可互译”。**

更像是：

- 顺序逻辑、数据结构、协议处理这些地方，大量部分确实可以局部替换；
- 但一旦碰到 `tokio::select!` 这种“并发组合本身就是一等原语”的地方，C++ 需要自己把那层组合能力搭出来。

这不是形式上的表达力空白，因为最后程序还是能写出来，也能通过同样的测试。真正的差别更接近：

**Rust 把某些并发组合方式做成了现成的、可直接拿来推理的原语；C++ 则要求你回到更底层，把它自己搭起来。**

## 这轮数据

| 指标 | 数值 |
|------|------|
| Rust 源码行数 | 3,393 |
| C++ 源码行数 | 2,266 |
| 行数变化 | -33% |
| 行为测试 | 10 / 10 |
| 涉及 pub/sub 的测试 | 4 个 |
| 涉及过期逻辑的测试 | 1 个 |
| 明确需要架构重做的地方 | 1 处：`select!` 动态 fan-in |

### 新增的映射表

| Rust | C++ |
|------|-----|
| `tokio::spawn` | `asio::co_spawn` |
| `tokio::select!` | 共享 `steady_timer`，由多个来源 cancel |
| `async fn` | `asio::awaitable<T>` |
| `tokio::sync::broadcast` | 自定义 `BroadcastSender / Receiver` |
| `tokio::sync::Notify` | `steady_timer` cancel 唤醒 |
| `Semaphore` | 原子计数 / 自己控制等待 |
| `StreamMap` | 订阅列表 |
| `async_stream::stream!` | 直接去掉 |
| `Bytes` | `std::string` |
| `signal::ctrl_c()` | `asio::signal_set` |

## 到这里，能比较稳地说什么

mini-redis 没有推翻前两篇，但它逼着我把表述收得更细。

如果只问“能不能把 Rust 程序反过来变成工作正常的 C++ 程序”，答案仍然是可以。

如果继续追问“这个过程是不是全程机械”，答案就变成：**不是。至少在并发组合这一层，Rust 提供的原语会明显改变你组织程序的自然方式。**

所以这篇更像是在说：

- 算法和行为层面，差距没想象中那么夸张；
- 并发组合的抽象层面，Rust 确实给了更顺手的工具；
- 而那些顺手工具背后，仍然和类型系统、借用规则、异步运行时的整体设计绑在一起。

### 下一篇

第四篇会看 fish shell。它本身经历过 C++ → Rust 的真实重写。把其中一部分再转回 C++，就能看到“来回一趟之后，留下来的到底是什么”。

---

*这次转写对应的完整材料在 [`03-mini-redis`](../03-mini-redis/)。目前整理出的 10 个行为测试全部通过，覆盖了 key-value、pub/sub、过期和订阅管理。*
