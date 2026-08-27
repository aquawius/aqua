#ifndef AQUA_UDP_TRANSPORT_H
#define AQUA_UDP_TRANSPORT_H

// UdpTransport：UDP 数据面的唯一传输类（server 与 client 共用，无继承层次）。
// 原 UdpSocketBase / UdpServer / UdpClient 三个传输类已合并到此处；
// 协议层（HELLO 握手 / 音频编解码）见 udp_server.h / udp_client.h。
//
// server 用法（固定端口）：
//   UdpTransport server(ioc);
//   server.bind("0.0.0.0", 9999);
//   server.start_receive(handler);
//   server.send_to(client_ep, data);          // 定向回复 / 广播
//
// client 用法（临时端口 + 默认发送目标）：
//   UdpTransport client(ioc);
//   client.set_remote(server_ip, udp_port);   // 内部自动 open，按远端地址族选 IPv4/IPv6
//   client.start_receive(handler);
//   client.send(data);                        // 发给 set_remote 指定的目标
//
// 生命周期模型：
//   所有异步 handler（收包完成、发送完成、stop 的关闭任务）只捕获
//   std::shared_ptr<State>，从不捕获 this。即使 transport 对象在 io_context
//   执行已排队 handler 之前被析构，State 与其内 socket 也由 shared_ptr 保活到
//   最后一个 handler 结束，不存在 use-after-free。
//
// 并发模型：
//   State 内的 socket、接收回调和发送泵只在本类 strand 上执行；用户态发送队列
//   由 tx_queue_mutex 保护，允许任意业务/音频线程直接入队。因此 io_context
//   可以安全地由多个线程 run()，transport 自身无需依赖"单 IO 线程"。
//   stopped / open 与统计计数器用 atomic，供任意线程读取。
//
// 使用约束：
//   停止后的 transport 不可复用（send / start_receive 会因 stopped 静默失败），
//   重连/重启请新建实例。

#include "aqua/net/udp/udp_config.h"

#include <asio.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace aqua::net {

// 传输层统计快照。
// 由 stats() 采集。计数项均为 atomic 的近似读值（relaxed），多线程并发下
// 各字段之间不保证一致，仅供监控/日志使用，不用于精确计量。
struct UdpTransportStats {
    std::uint64_t rx_packets { 0 }; // 成功收到的 datagram 数
    std::uint64_t rx_bytes { 0 }; // 成功收到的字节总数
    std::uint64_t rx_errors { 0 }; // 接收错误数（stop 流程的 operation_aborted 不计入）
    std::uint64_t tx_packets { 0 }; // 成功发送的 datagram 数
    std::uint64_t tx_bytes { 0 }; // 成功发送的字节总数
    std::uint64_t tx_errors { 0 }; // 发送失败数（如对端关闭触发的 ICMP 错误）
    std::uint64_t tx_dropped { 0 }; // 发送队列超限被丢弃的 datagram 数
    std::size_t tx_queue_depth { 0 }; // 采集时刻的发送队列深度
};

// UDP 传输：server 固定端口（bind）与 client 临时端口（open + set_remote）
// 两种打开方式共用同一套收发机器。不调用 UDP socket::connect()（不建立
// 内核过滤/错误上报语义），peer 的归属由上层协议按收包来源 endpoint 判定。
class UdpTransport {
public:
    // 收包回调。sender 为对端（NAT 映射后的）地址；data 指向内部预分配缓冲，
    // 仅在本次回调内有效（下一次 async_receive_from 会复用同一缓冲），
    // 需要跨回调保活的数据必须自行拷贝。回调在 strand 上触发，禁止阻塞。
    using ReceiveHandler = std::function<void(
        const asio::ip::udp::endpoint& sender,
        std::span<const std::byte> data)>;

    // 创建 transport（仅创建 State：strand + socket），不打开 socket；
    // 打开由 bind()（server 固定端口）或 open()/set_remote()（client）完成。
    explicit UdpTransport(asio::io_context& ioc);
    // 析构时自动 stop()：确保 socket 关闭、发送队列清空，防止异步链残留。
    ~UdpTransport();

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    // ---- 打开（两种方式二选一，打开后地址族不可切换）----

    // server：绑定固定端口。bind_ip 为 "0.0.0.0" 表示监听所有接口，
    // 支持 IPv4/IPv6 字面量（IPv6 为 v6-only，双栈请分别创建实例）。
    // 请求 SO_REUSEADDR 以支持"进程异常退出后立即重启"（Windows 不设置，
    // 见实现注释）。返回 false 表示绑定失败，修复后可重新调用。
    // 同一 endpoint 重复 bind 幂等成功；不同 endpoint 拒绝。
    bool bind(const std::string& bind_ip, std::uint16_t port);

    // client：打开并绑定 OS 分配的临时端口（0.0.0.0:0，IPv4）。
    // 幂等：已打开直接返回 true。需要 IPv6 时请直接 set_remote(ipv6, port)，
    // 它会按远端地址族选择并打开对应 socket。
    bool open();

    // ---- client 默认发送目标 ----

    // 设置默认发送目标（后续 send()/send_shared() 免参发送的对象）。
    // 未打开时自动按远端地址族 open()。远端端口为 0 时拒绝（非法目标）。
    bool set_remote(const asio::ip::udp::endpoint& remote);
    // 字符串版：解析 IP 字面量（不支持 DNS 主机名）；IPv6 可带方括号，如 [::1]。
    bool set_remote(const std::string& server_ip, std::uint16_t port);

    // 是否已设置默认发送目标（线程安全）。
    [[nodiscard]] bool has_remote() const noexcept;
    // 获取默认发送目标（线程安全；未设置时返回端口为 0 的 endpoint）。
    [[nodiscard]] asio::ip::udp::endpoint remote_endpoint() const noexcept;

    // ---- 收发 ----

    // 启动异步接收循环。必须在 socket 打开（bind/open/set_remote 成功）后调用，
    // 未打开或已停止时返回 false 且不启动。
    // handler 在 transport strand 上触发，禁止阻塞；handler 抛出的异常会被
    // 捕获并记录，不会终止接收循环。
    // 重复调用（接收循环已在运行）会被忽略，并记录 warning。
    bool start_receive(ReceiveHandler handler);

    // 定向发送（拷贝语义）：先把 data 复制进新分配的共享缓冲再入队，
    // 调用方无需保活 data。适合 HELLO/ACK 等低频小包；音频广播等高频路径
    // 请用 send_to_shared() 避免逐包拷贝。
    void send_to(const asio::ip::udp::endpoint& target,
        std::span<const std::byte> data);
    // 定向发送（共享缓冲）：队列只持有 shared_ptr，不重复拷贝 payload。
    // 同一份缓冲可安全地发给多个 endpoint（广播场景）。
    void send_to_shared(const asio::ip::udp::endpoint& target,
        std::shared_ptr<const std::vector<std::byte>> data);

    // 便捷发送：发给 set_remote() 指定的默认目标（拷贝语义）。
    // 未设置远端时丢弃并记录 debug（高频音频路径下不应因误用刷 error 日志）。
    void send(std::span<const std::byte> data);
    // 便捷发送（共享缓冲版）：同上，走 send_to_shared 的零拷贝队列路径。
    void send_shared(std::shared_ptr<const std::vector<std::byte>> data);

    // ---- 生命周期 / 状态 ----

    // 停止收发并关闭 socket。幂等：多次调用只生效一次。
    // 停止后不可复用（send/start_receive 会因 stopped 标志静默失败）。
    // 关闭任务被 post 到 strand；异步任务只持有独立 State，不捕获 this，
    // 因此即使析构发生在 io_context handler 执行之前也不会产生 UAF。
    void stop() noexcept;

    // 是否已打开（bind/open 成功且未 stop）。
    [[nodiscard]] bool is_open() const noexcept;

    // 返回 bind 成功后的本地 endpoint 快照，不访问 socket（线程安全、无异常）。
    // 用于 bind 端口=0 后查询 OS 实际分配的端口。
    [[nodiscard]] asio::ip::udp::endpoint socket_local_endpoint() const noexcept;

    // 采集统计快照（字段含义见 UdpTransportStats）。
    [[nodiscard]] UdpTransportStats stats() const noexcept;

private:
    // 发送队列元素：目标 endpoint + 共享 payload（shared_ptr，不拷贝数据本体）。
    struct PendingSend {
        asio::ip::udp::endpoint target;
        std::shared_ptr<const std::vector<std::byte>> payload;
    };

    // transport 的全部可变状态。独立于 UdpTransport 持有（shared_ptr），
    // 供异步 handler 捕获保活；strand 串行化内部访问，普通成员无需 atomic。
    struct State {
        // strand 即本 transport 的"单线程"边界：socket 以 strand 构造，
        // 所有 async 操作经 bind_executor 绑定到 strand。
        using Strand = asio::strand<asio::io_context::executor_type>;

        explicit State(asio::io_context& ioc)
            : strand(asio::make_strand(ioc))
            , socket(strand)
        {
        }

        Strand strand;
        asio::ip::udp::socket socket;
        ReceiveHandler handler; // 收包回调（仅 strand 上访问）

        // 预分配接收缓冲，避免收包路径堆分配；必须覆盖协议允许的最大 UDP
        // datagram。SO_RCVBUF 是内核队列容量，并不等于单个 datagram 的大小。
        static constexpr std::size_t RECV_BUF_SIZE = aqua::config::UDP_RECV_BUFFER_BYTES;
        std::array<std::byte, RECV_BUF_SIZE> recv_buf { };
        asio::ip::udp::endpoint recv_endpoint { }; // 当前收包来源（仅 strand 上访问）
        asio::ip::udp::endpoint local_endpoint { }; // bind 成功时的本地地址快照

        // ---- 发送队列（生产者线程 + strand 共享）----
        // 这是整个 transport 的唯一用户态发送队列，容量由 UDP_MAX_QUEUED_DATAGRAMS
        // 严格限制。生产者入队时只持 tx_queue_mutex；strand 负责实际 async_send_to。
        std::mutex tx_queue_mutex;
        std::deque<PendingSend> send_queue;
        std::optional<PendingSend> in_flight;

        // true 表示已经有一个 strand 发送泵在运行/等待运行。这样连续 send_to_shared()
        // 只会为一批积压数据 post 一个 drain task，而不是每个 datagram 一个 post。
        bool send_pump_scheduled { false };

        // 接收循环是否在运行（仅 strand 上访问；普通 bool 即可，无需 atomic）
        bool receiving { false };

        // ---- 跨线程原子标志 / 统计 ----
        std::atomic<bool> stopped { false }; // stop() 已调用（幂等标志）
        std::atomic<bool> open { false }; // 打开成功快照，供任意线程 is_open() 读取

        std::atomic<std::uint64_t> rx_packets { 0 };
        std::atomic<std::uint64_t> rx_bytes { 0 };
        std::atomic<std::uint64_t> rx_errors { 0 };
        std::atomic<std::uint64_t> tx_packets { 0 };
        std::atomic<std::uint64_t> tx_bytes { 0 };
        std::atomic<std::uint64_t> tx_errors { 0 };
        std::atomic<std::uint64_t> tx_dropped { 0 };
        std::atomic<std::size_t> tx_queue_depth { 0 };
    };

    // 根据 bind_ip 的地址族打开 IPv4/IPv6 socket，配置内核缓冲（SO_RCVBUF/SO_SNDBUF），
    // 并绑定 bind_ip:port。reuse_address 仅在 POSIX 生效（Windows 不设置，见实现注释）。
    // 已 stop 的 transport 拒绝再次打开；失败返回 false，此时 socket 已关闭。
    bool open_and_bind(const std::string& bind_ip, std::uint16_t port, bool reuse_address);

    // 接收循环：投递下一个 async_receive_from（仅 strand 上调用）。
    static void do_receive(const std::shared_ptr<State>& state);
    // 发送泵：由单个 strand task 驱动队列，避免每个 datagram 都向 io_context post 一个 handler。
    static void start_next_send(const std::shared_ptr<State>& state);
    // 关闭 socket、清空队列与回调（stop 流程使用，noexcept）。
    static void close_state(const std::shared_ptr<State>& state) noexcept;

    std::shared_ptr<State> state_;

    // 默认发送目标（client 语义）。可被任意线程读写：set_remote() 在控制线程写，
    // send() 在音频/业务线程读，用互斥保护；与 strand 上的发送队列解耦。
    mutable std::mutex remote_mutex_;
    asio::ip::udp::endpoint remote_ { };
};

} // namespace aqua::net

#endif // AQUA_UDP_TRANSPORT_H
