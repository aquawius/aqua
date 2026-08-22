#ifndef AQUA_UDP_SOCKET_BASE_H
#define AQUA_UDP_SOCKET_BASE_H

// UdpServer / UdpClient 共享的 UDP socket 收发骨架。
//
// 生命周期模型：
//   所有异步 handler（收包完成、发送完成、stop 的关闭任务）只捕获
//   std::shared_ptr<State>，从不捕获 UdpSocketBase::this。即使 transport 对象
//   在 io_context 执行已排队 handler 之前被析构，State 与其内 socket 也由
//   shared_ptr 保活到最后一个 handler 结束，不存在 use-after-free。
//
// 并发模型：
//   State 内的 socket 与发送队列只在本类 strand 上访问（socket 以 strand 构造，
//   async 操作均 bind_executor 到 strand），因此 io_context 可以安全地由多个
//   线程 run()，transport 自身无需依赖"单 IO 线程"。
//   stopped / open 与统计计数器用 atomic，供任意线程无锁读取。
//
// 使用约束：
//   停止后的 transport 不可复用（send / start_receive 会因 stopped 静默失败），
//   重连/重启请新建 UdpServer / UdpClient 实例。

#include "aqua/net/udp/udp_config.h"

#include <asio.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace aqua::net {

// 传输层统计快照。
// 由 stats() 采集。计数项均为 atomic 的近似读值（relaxed），多线程并发下
// 各字段之间不保证一致，仅供监控/日志使用，不用于精确计量。
struct UdpTransportStats {
    std::uint64_t rx_packets { 0 };   // 成功收到的 datagram 数
    std::uint64_t rx_bytes { 0 };     // 成功收到的字节总数
    std::uint64_t rx_errors { 0 };    // 接收错误数（stop 流程的 operation_aborted 不计入）
    std::uint64_t tx_packets { 0 };   // 成功发送的 datagram 数
    std::uint64_t tx_bytes { 0 };     // 成功发送的字节总数
    std::uint64_t tx_errors { 0 };    // 发送失败数（如对端关闭触发的 ICMP 错误）
    std::uint64_t tx_dropped { 0 };   // 发送队列超限被丢弃的 datagram 数
    std::size_t tx_queue_depth { 0 }; // 采集时刻的发送队列深度
};

// UDP socket 公共收发骨架。对外使用 UdpServer / UdpClient，本类不对外直接使用。
// 所有 socket 操作、接收回调和发送队列操作均串行化到同一 strand，
// 因此 io_context 可以安全地由多个线程 run()，transport 本身无需再依赖"单 IO 线程"。
class UdpSocketBase {
public:
    // 收包回调。sender 为对端（NAT 映射后的）地址；data 指向内部预分配缓冲，
    // 仅在本次回调内有效（下一次 async_receive_from 会复用同一缓冲），
    // 需要跨回调保活的数据必须自行拷贝。回调在 strand 上触发，禁止阻塞。
    using ReceiveHandler = std::function<void(
        const asio::ip::udp::endpoint& sender,
        std::span<const std::byte> data)>;

    // 创建 transport（仅创建 State：strand + socket），不打开 socket；
    // 打开/绑定由派生类（UdpServer::bind / UdpClient::open）完成。
    explicit UdpSocketBase(asio::io_context& ioc);
    // 析构时自动 stop()：确保 socket 关闭、发送队列清空，防止异步链残留。
    virtual ~UdpSocketBase();

    UdpSocketBase(const UdpSocketBase&) = delete;
    UdpSocketBase& operator=(const UdpSocketBase&) = delete;

    // 启动异步接收循环。必须在 socket 打开（bind/open 成功）后调用，
    // 未打开或已停止时返回 false 且不启动。
    // handler 在 transport strand 上触发，禁止阻塞；handler 抛出的异常会被
    // 捕获并记录，不会终止接收循环。
    // 重复调用（接收循环已在运行）会被忽略，并记录 warning。
    bool start_receive(ReceiveHandler handler);

    // 异步发送数据到目标 endpoint。拷贝语义版本：先把 data 复制进新分配的
    // shared_ptr 缓冲再入队，调用方无需保活 data。适合 HELLO/ACK 等低频小包；
    // 音频广播等高频路径请用 send_shared() 避免逐包拷贝。
    void send_copy(const asio::ip::udp::endpoint& target,
        std::span<const std::byte> data);

    // 共享缓冲发送：队列只持有 shared_ptr，不重复拷贝 payload。
    // 同一份缓冲可安全地发给多个 endpoint（广播场景），避免"每 session
    // 一次堆分配 + 全量拷贝"。与 send_copy() 的线程/生命周期语义相同。
    void send_shared(const asio::ip::udp::endpoint& target,
        std::shared_ptr<const std::vector<std::byte>> data);

    // 停止收发并关闭 socket。幂等：多次调用只生效一次。
    // 停止后不可复用（send/start_receive 会因 stopped 标志静默失败）。
    // 关闭任务被 post 到 strand；异步任务只持有独立 State，不捕获
    // UdpSocketBase::this，因此即使析构发生在 io_context handler 执行之前
    // 也不会产生 UAF。
    void stop() noexcept;

    // 是否已打开（bind/open 成功且未 stop）。
    bool is_open() const noexcept;

    // 返回 bind 成功后的本地 endpoint 快照，不访问 socket（线程安全、无异常）。
    // 用于 bind 端口=0 后查询 OS 实际分配的端口。
    [[nodiscard]] asio::ip::udp::endpoint socket_local_endpoint() const noexcept;

    // 采集统计快照（字段含义见 UdpTransportStats）。
    [[nodiscard]] UdpTransportStats stats() const noexcept;

protected:
    // 根据 bind_ip 的地址族打开 IPv4/IPv6 socket，配置内核缓冲（SO_RCVBUF/SO_SNDBUF），
    // 并绑定 bind_ip:port。IPv6 socket 显式设置 v6_only（IPv4-mapped 不混入），
    // 双栈部署请分别创建 IPv4/IPv6 实例。
    // 本函数在调用线程执行同步 socket 操作是安全的：此刻尚无在途异步操作
    // 与这些调用竞争；成功后 socket 生命周期由 transport 自己管理。
    // 重复调用：同一 endpoint 视为幂等成功，不同 endpoint 拒绝（见实现注释）。
    // reuse_address 仅在 POSIX 生效（Windows 不设置，见实现注释）。
    // 已 stop 的 transport 拒绝再次打开；失败返回 false，此时 socket 已关闭。
    bool open_and_bind(const std::string& bind_ip, std::uint16_t port, bool reuse_address);

private:
    // 发送队列元素：目标 endpoint + 共享 payload（shared_ptr，不拷贝数据本体）。
    struct PendingSend {
        asio::ip::udp::endpoint target;
        std::shared_ptr<const std::vector<std::byte>> payload;
    };

    // transport 的全部可变状态。独立于 UdpSocketBase 持有（shared_ptr），
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

        // ---- 发送队列（仅 strand 上访问）----
        std::deque<PendingSend> send_queue; // 待发送 datagram，容量受 UDP_MAX_QUEUED_DATAGRAMS 限制
        bool send_in_flight { false };      // 当前是否有在途 async_send_to（保证串行发送）

        // 接收循环是否在运行（仅 strand 上访问；普通 bool 即可，无需 atomic）
        bool receiving { false };

        // ---- 跨线程原子标志 / 统计 ----
        std::atomic<bool> stopped { false }; // stop() 已调用（幂等标志）
        std::atomic<bool> open { false };    // 打开成功快照，供任意线程 is_open() 读取

        std::atomic<std::uint64_t> rx_packets { 0 };
        std::atomic<std::uint64_t> rx_bytes { 0 };
        std::atomic<std::uint64_t> rx_errors { 0 };
        std::atomic<std::uint64_t> tx_packets { 0 };
        std::atomic<std::uint64_t> tx_bytes { 0 };
        std::atomic<std::uint64_t> tx_errors { 0 };
        std::atomic<std::uint64_t> tx_dropped { 0 };
        std::atomic<std::size_t> tx_queue_depth { 0 };
    };

    // 接收循环：投递下一个 async_receive_from（仅 strand 上调用）。
    static void do_receive(const std::shared_ptr<State>& state);
    // 发送泵：若队列非空且无在途发送，取出队首发起 async_send_to（仅 strand 上调用）。
    static void start_next_send(const std::shared_ptr<State>& state);
    // 关闭 socket、清空队列与回调（stop 流程使用，noexcept）。
    static void close_state(const std::shared_ptr<State>& state) noexcept;

    std::shared_ptr<State> state_;
};

} // namespace aqua::net

#endif // AQUA_UDP_SOCKET_BASE_H
