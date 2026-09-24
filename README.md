# RDMA 原型

基于 Rust、C shim、libfabric 和 Soft-RoCE 的独立 RDMA 原型，采用 **verbs provider + `FI_EP_MSG` + Send/Receive**，为后续分片传输和 Dragonfly 集成验证通信基础。

目前已实现 C 传输封装、Rust 原始 FFI 绑定，以及 Rust 单连接客户端和服务端。`src/transport.rs` 统一管理连接、注册缓冲和资源清理；`src/bin/server.rs` 与 `src/bin/clients.rs` 分别处理两端的协议状态。完成 `hello → world → ack` 后退出。独立 C 示例仍保留，可用于交叉验证。

## 编译与测试

需要安装 Rust 工具链、C 编译器、pkg-config，以及 libfabric 开发头文件和库。shim 请求 libfabric API 1.11，当前不使用 RMA，也不交换远端内存 key。

以下命令均在包含 `Cargo.toml` 的目录中执行：

```bash
cargo build
cargo test --all-targets
bash tests/run_shim_tests.sh
```

C 生命周期测试使用可控的 provider 操作表和查询函数替身，无须 RDMA 设备。测试覆盖 domain 筛选、分配失败、`EAGAIN`、缓冲区边界、连接事件、CQ 错误、资源占用、初始化失败回滚和关闭失败。

启用地址、未定义行为和内存泄漏检查：

```bash
SANITIZE=1 bash tests/run_shim_tests.sh
```

部分跟踪沙箱不支持 LeakSanitizer。在此环境中，可以暂时关闭泄漏检查，运行地址和未定义行为检查：

```bash
ASAN_OPTIONS=detect_leaks=0 SANITIZE=1 bash tests/run_shim_tests.sh
```

内存泄漏检查应在沙箱外另行执行。上述模拟测试通过不代表真实 RXE 收发已经验证通过，实际通信需要运行下面的双进程示例。

## 双进程 RXE 收发示例

先确认 libfabric 能发现目标 RXE domain：

```bash
fi_info -p verbs -t FI_EP_MSG
```

然后编译独立 C 示例：

```bash
cc -std=c11 -Wall -Wextra -Werror -I naive \
  examples/shim_pingpong.c naive/fabric_shim.c \
  $(pkg-config --cflags --libs libfabric) -o /tmp/rdma_shim_pingpong
```

在两个终端中分别运行服务端和客户端。将下面的地址和 domain 占位符替换成实际值，并在服务端启动后 **5 秒内**启动客户端。

服务端：

```bash
/tmp/rdma_shim_pingpong server <server-ip> 7471 <server-local-domain>
```

客户端：

```bash
/tmp/rdma_shim_pingpong client <server-ip> 7471 <client-local-domain>
```

单机双进程测试使用相同的 RXE 网卡 IP 和 domain。最后一个 domain 参数可以省略；指定时，它始终筛选运行该进程的本地设备，客户端也不例外。

示例交互流程：

```text
客户端发送 hello
    → 服务端检查 hello，返回 world
    → 客户端检查 world，发送 ack
    → 服务端检查 ack
    → 双方完成相关收发并清理资源
```

双方都会观察发送和接收的 CQ 完成事件。每个轮询阶段使用单调时钟设置 5 秒截止时间；该截止时间限制应用层轮询，不能中断 provider 内部正在执行的同步调用。

## Rust 双进程测试与资源对象

`src/transport.rs` 将裸句柄放进有明确生命周期的 Rust 结构体，供客户端和服务端共同使用；两个二进制程序只处理参数、协议状态和事件循环：

| 对象 | 持有的资源 | 主要方法 |
| --- | --- | --- |
| `Info` | `*mut ffi::Info` 查询或请求配置 | `local()`、`remote()` |
| `Fabric` | `*mut ffi::Fabric` C 包装对象指针 | `open()`、`close()` |
| `Listener` | 借用 `Fabric`，持有监听 EQ 和 passive EP | `bind()`、`poll()`、`wait_request()` |
| `Request` | 借用监听器，持有 `event.info` | 未处理请求在释放时尝试拒绝 |
| `Connection` | 借用 `Fabric`，持有 domain、连接 EQ、TX/RX CQ、active EP、缓冲槽位 | `from_request()`、`connect()`、`post_recv()`、`send()`、`poll()`、`close()` |
| `Slot` | 固定地址的 `Box<[u8]>`、MR、操作 context、在途标记 | 连接初始化时创建，完成后复用 |
| `Exchange` | hello/world/ack 协议状态，不持有底层资源 | `on_event()` |

例如 `Fabric { raw: *mut ffi::Fabric }` 中的 `raw` 是 C 函数写入的地址。返回 `Fabric` 对象不会关闭该地址对应的资源；`close()` 才执行关闭，`Drop` 作为遗漏清理或初始化失败时的兜底。`Listener<'f>` 和 `Connection<'f>` 的 `&'f Fabric` 借用使父对象不能先被 Rust 销毁。

`Connection::poll()` 先更新收发槽位状态，再返回事件。`serve()` 将事件交给 `Exchange::on_event()`，业务回调不会重入 FFI 轮询。接收完成后复制消息到 `Vec<u8>`，因此回调不直接访问仍被 provider 借用的内存。

在两个终端中分别运行 Rust 服务端和客户端（当前客户端文件名为 `clients.rs`，可执行程序名为 `clients`）：

```bash
# 终端 A：Rust 服务端，端口默认 7471，等待时限默认 30 秒
cargo run --bin server -- <本机RXE网卡IP> 7471 <本地RXE-domain> 30

# 终端 B：Rust 客户端，连接服务端并校验 world
cargo run --bin clients -- <服务端RXE网卡IP> 7471 <客户端本地RXE-domain> 30
```

服务端参数格式为 `server <local-rxe-ip> [port] [local-domain] [timeout-seconds]`，客户端为 `clients <server-rxe-ip> [port] [local-domain] [timeout-seconds]`。端口默认 `7471`，超时默认 `30` 秒，范围为 `1–3600` 秒。单机测试时两端使用同一个 RXE 网卡 IP 和 domain。连接请求等待、预投递接收重试、连接建立及消息交换都有截止时间。显式清理会报告失败；若 provider 无法关闭资源，兜底逻辑保留可能仍被引用的缓冲，交由进程退出回收，避免提前释放。

客户端先通过 `Info::remote()` 查询服务端地址，再通过 `Connection::connect()` 创建资源、预投递接收并发起连接。收到 `Connected` 后才发送 `hello`。`hello` 的 TX 完成与 `world` 的 RX 完成可以按任意顺序到达；只有两者都满足，客户端才复用发送槽位发送 `ack`，并等待 ACK 的 TX 完成后关闭。

提交遇到 `EAGAIN` 时不会推进协议状态，会继续轮询后重试。错误或重复的 `world` 响应会被拒绝。对端关闭后停止提交新操作，但继续在截止时间内读取可能尚未观察到的完成事件。客户端测试使用模拟事件覆盖这些路径；它们不能替代真实 RXE 集成测试。

## 资源所有权与接口约定

- `shim_info_open()` 查询并保留一条通信配置，不建立连接。
- 连接请求事件将 `event.info` 的所有权交给调用方。服务端使用该配置创建 domain 和 active EP；接受或拒绝请求后，再释放配置。单独释放配置不会拒绝连接请求。
- 一组相互依赖的通信资源由同一个驱动线程操作。`src/ffi.rs` 提供的是需要调用方保证安全的原始 FFI，尚未封装异步接口和完整的 Rust 资源所有权管理。
- EQ/CQ 轮询返回 `1` 表示取得一个事件，返回 `0` 表示暂无事件，返回负值表示轮询失败。取得的事件本身也可能表示操作失败，必须检查错误字段。
- 连接事件的 `source` 是来源监听端点或通信端点的包装对象地址，仅用于身份比较，不能解引用。应在销毁来源句柄之前处理相关 EQ 事件。
- 每个 active EP 使用独立的 TX CQ 和 RX CQ。创建新 EP 时需要重新创建 CQ，不复用已经关闭端点的 CQ。
- 注册缓冲区仍由调用方拥有。注册期间不得改变底层分配地址或释放内存；接收完成前不得访问接收区域，发送完成前不得修改发送区域。并发收发使用的内存范围应互不重叠。shim 检查 MR 范围和访问权限，但无法阻止调用方直接访问内存。
- 已提交操作在完成前不能复用或释放。`EAGAIN` 表示操作没有成功提交，shim 不会保留该操作；调用方应推进事件后重试。CQ 容量限制可同时在途的操作数量。
- 超时或取消时，停止提交新操作并关闭整个 EP。libfabric 1.11 的 verbs MSG 不支持 `fi_cancel()`。
- EP 成功关闭后，provider 不再借用其操作缓冲。未完成操作的 context 仍可能被 CQ 中的旧条目引用，因此必须先关闭 CQ，再释放这些 context。EP 关闭后不再允许轮询其 CQ，也不应继续等待所有在途操作产生完成事件。
- 关闭函数只有成功时才释放句柄；资源仍被依赖或 provider 关闭失败时，句柄继续有效。组合式监听端点或通信端点创建函数如果失败，且回滚关闭也失败，可能返回非空句柄。服务端请求 EP 的绑定或启用失败时也会保留句柄，以便先拒绝请求，再关闭 EP，避免拒绝时访问已销毁的连接标识。此类句柄只能用于清理。

典型清理顺序：

```text
active EP
    → TX/RX CQ
    → 操作 context
    → MR
    → 调用方分配的缓冲区
    → passive EP（监听端点）
    → EQ
    → domain
    → fabric
```

共享父资源必须晚于所有依赖它的句柄释放。shim 本身不进行无限重试或等待，截止时间由调用方控制。

## 参考资料

[libfabric 1.11 verbs provider 官方文档](https://ofiwg.github.io/libfabric/v1.11.0/man/fi_verbs.7.html)
