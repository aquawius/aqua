// 并发与跨线程安全测试
//
// 验证模块头文件中声称的线程安全契约：
//   - JitterBuffer: push/pop 同线程，诊断 getter 跨线程读
//   - DiagnosticsManager: io_context 线程 record_*，主线程 collect_and_log/snapshot
//   - UdpTransport: 多线程并发 send 安全（asio::post 串行化）
//   - SessionManager: for_each_connected + 并发 remove/create
//   - SpscRingBuffer: 单生产者单消费者高负载

#include "core/audio/ringbuffer/spsc_ringbuffer.h"
#include "core/diagnostics/diagnostics_manager.h"
#include "core/jitter_buffer/jitter_buffer.h"
#include "core/net/packet/packet.h"
#include "core/net/transport/udp_transport.h"
#include "core/public/audio_format.h"
#include "core/public/config.h"
#include "core/session/session_manager.h"

#include <gtest/gtest.h>

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {

aqua::AudioFormat make_test_format() {
    aqua::AudioFormat fmt;
    fmt.encoding = aqua::AudioEncoding::PcmF32LE;
    fmt.channels = 2;
    fmt.sample_rate = 48000;
    return fmt;
}

constexpr std::uint32_t FRAMES_PER_PACKET = 144;
constexpr std::size_t PAYLOAD_SIZE = 144 * 2 * 4;  // 1152

std::vector<std::byte> make_payload(std::uint32_t sequence) {
    return std::vector<std::byte>(PAYLOAD_SIZE, static_cast<std::byte>((sequence & 0xFF) + 1));
}

} // namespace

// ==== 1. JitterBuffer: push/pop 主线程 + 诊断 getter 跨线程读 ====
// 头文件契约：slots_mutex_ 保护 slots_，允许诊断 getter 跨线程读

TEST(ConcurrencyTest, JitterBufferDiagnosticsGetterConcurrentWithPush) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 4, 16);

    std::atomic<bool> stop{false};
    std::atomic<int> getter_calls{0};
    std::atomic<bool> reader_started{false};

    // 后台线程：持续读诊断 getter
    std::thread reader([&] {
        reader_started.store(true, std::memory_order_relaxed);
        while (!stop.load(std::memory_order_relaxed)) {
            (void)jb.buffer_fill_packets();
            (void)jb.packets_received();
            (void)jb.packets_lost();
            (void)jb.duplicates();
            (void)jb.late_packets();
            (void)jb.next_sequence();
            (void)jb.capacity_packets();
            getter_calls.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // 等 reader 线程确认启动后再开始 push，避免主线程在 reader 被调度前就跑完。
    while (!reader_started.load(std::memory_order_relaxed)) {
        std::this_thread::yield();
    }

    // 主线程：push + pop
    constexpr int N = 500;
    for (int i = 0; i < N; ++i) {
        auto p = make_payload(static_cast<std::uint32_t>(i));
        jb.push(static_cast<std::uint32_t>(i), p);

        // 每 10 个包 pop 一些
        if (i % 10 == 9) {
            std::vector<std::byte> out(PAYLOAD_SIZE);
            int pops = 0;
            while (jb.buffer_fill_packets() > 0 && pops < 5) {
                (void)jb.pop_next(out);
                ++pops;
            }
        }
        // Release 模式下 push 循环极快，定期 yield 让 reader 线程运行。
        if (i % 20 == 0) {
            std::this_thread::yield();
        }
    }

    stop.store(true, std::memory_order_relaxed);
    reader.join();

    // getter 应被调用多次（证明并发运行，未死锁未崩溃）
    EXPECT_GT(getter_calls.load(), 100);
    EXPECT_EQ(jb.packets_received(), static_cast<std::uint64_t>(N));
}

// ==== 2. JitterBuffer: 高并发 getter + reset ====
// reset 也修改 slots_，验证 reset 与 getter 不死锁

TEST(ConcurrencyTest, JitterBufferResetConcurrentWithGetters) {
    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 4, 16);

    std::atomic<bool> stop{false};
    std::atomic<int> resets{0};

    // 2 个 reader 线程
    std::thread r1([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            (void)jb.buffer_fill_packets();
            (void)jb.next_sequence();
        }
    });
    std::thread r2([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            (void)jb.packets_received();
            (void)jb.capacity_packets();
        }
    });

    // 主线程：push + 偶尔 reset
    for (int i = 0; i < 200; ++i) {
        auto p = make_payload(static_cast<std::uint32_t>(i));
        jb.push(static_cast<std::uint32_t>(i), p);
        if (i % 50 == 49) {
            jb.reset();
            resets.fetch_add(1, std::memory_order_relaxed);
        }
    }

    stop.store(true, std::memory_order_relaxed);
    r1.join();
    r2.join();

    EXPECT_EQ(resets.load(), 4);
}

// ==== 3. DiagnosticsManager: io_context 线程 record_* + 主线程 collect_and_log/snapshot ====
// 头文件契约：事件回调在 io_context 线程，周期采样在主线程

TEST(ConcurrencyTest, DiagnosticsManagerCrossThreadAccess) {
    std::size_t rb_fill = 0;
    std::atomic<std::uint64_t> played{0};

    aqua::diag::DiagnosticsManager dm(
        48000, 8, PAYLOAD_SIZE,
        [&rb_fill]() { return rb_fill; },
        PAYLOAD_SIZE * 16,
        [&played]() { return played.load(std::memory_order_relaxed); });

    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 4, 16);

    std::atomic<bool> stop{false};
    std::atomic<int> record_calls{0};

    // 模拟 io_context 线程：高频 record_packet_arrival + record_audio_bytes + record_hello_*
    std::thread io_thread([&] {
        for (int i = 0; i < 1000; ++i) {
            dm.record_packet_arrival(static_cast<std::uint32_t>(i),
                                     static_cast<std::uint32_t>(i) * FRAMES_PER_PACKET);
            dm.record_audio_bytes(PAYLOAD_SIZE);
            if (i % 50 == 0) {
                dm.record_hello_sent();
                dm.record_hello_ack_received();
                dm.record_hello_ack();
            }
            if (i % 10 == 0) {
                dm.record_underrun();
                dm.record_deadline_miss();
            }
            record_calls.fetch_add(1, std::memory_order_relaxed);
        }
        stop.store(true, std::memory_order_relaxed);
    });

    // 主线程：record_rb_occupancy + collect_and_log + snapshot
    int log_calls = 0;
    while (!stop.load(std::memory_order_relaxed) || record_calls.load() < 1000) {
        rb_fill = (rb_fill + 100) % (PAYLOAD_SIZE * 8);
        played.fetch_add(48, std::memory_order_relaxed);
        dm.record_rb_occupancy();

        if (log_calls % 5 == 0) {
            dm.collect_and_log(jb);
            auto snap = dm.snapshot();
            // 不严格断言值，只验证不崩溃 + 字段非负
            EXPECT_GE(snap.recv_audio_bytes, 0u);
            EXPECT_GE(snap.underruns, 0u);
        }
        ++log_calls;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    io_thread.join();

    // 最终 snapshot 应反映 io_thread 的调用
    dm.collect_and_log(jb);
    auto snap = dm.snapshot();
    EXPECT_GT(snap.recv_audio_bytes, 0u);
    EXPECT_GT(snap.recv_hello_acks, 0u);
    EXPECT_GT(snap.underruns, 0u);
    EXPECT_GT(snap.deadline_misses, 0u);
}

// ==== 4. UdpTransport: 多线程并发 send 安全 ====
// 头文件契约：send 通过 asio::post 串行化到 io_context 线程

TEST(ConcurrencyTest, UdpTransportConcurrentSendersSafe) {
    asio::io_context ioc;
    aqua::net::UdpTransport sender(ioc);
    aqua::net::UdpTransport receiver(ioc);

    ASSERT_TRUE(sender.bind("127.0.0.1", 0));
    ASSERT_TRUE(receiver.bind("127.0.0.1", 0));

    auto recv_ep = receiver.socket_local_endpoint();
    auto sender_ep = sender.socket_local_endpoint();

    std::atomic<int> received{0};
    receiver.start_receive([&](const auto& /*sender*/, auto /*data*/) {
        received.fetch_add(1, std::memory_order_relaxed);
    });

    std::thread ioc_thread([&] { ioc.run(); });

    // 4 个线程并发 send
    constexpr int PER_THREAD = 50;
    constexpr int NUM_THREADS = 4;
    std::vector<std::thread> threads;
    std::vector<std::byte> payload(PAYLOAD_SIZE, std::byte{0xAB});

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < PER_THREAD; ++i) {
                sender.send(recv_ep, payload);
            }
        });
    }

    for (auto& th : threads) th.join();

    // 等待接收完成
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (received.load() < NUM_THREADS * PER_THREAD
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sender.stop();
    receiver.stop();
    ioc.stop();
    ioc_thread.join();

    // UDP loopback 在高并发下可能丢包（socket buffer 溢出），
    // 重点验证不崩溃 + 收到部分包（>= 25%）
    EXPECT_GE(received.load(), static_cast<int>(NUM_THREADS * PER_THREAD * 0.25));
}

// ==== 5. UdpTransport: send 与 stop 并发安全 ====

TEST(ConcurrencyTest, UdpTransportSendConcurrentWithStop) {
    asio::io_context ioc;
    aqua::net::UdpTransport t1(ioc);
    aqua::net::UdpTransport t2(ioc);

    ASSERT_TRUE(t1.bind("127.0.0.1", 0));
    ASSERT_TRUE(t2.bind("127.0.0.1", 0));
    auto ep2 = t2.socket_local_endpoint();

    std::thread ioc_thread([&] { ioc.run(); });

    std::vector<std::byte> payload(100, std::byte{0x01});

    // 一个线程持续 send
    std::atomic<bool> sending{true};
    std::thread sender([&] {
        while (sending.load(std::memory_order_relaxed)) {
            t1.send(ep2, payload);
        }
    });

    // 让 send 跑一会儿
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 另一线程 stop（与 send 并发）
    t1.stop();
    sending.store(false, std::memory_order_relaxed);
    sender.join();

    t2.stop();
    ioc.stop();
    ioc_thread.join();

    SUCCEED();  // 不崩溃即通过
}

// ==== 6. SessionManager: for_each_connected + 并发 remove ====
// 头文件契约：for_each_connected 快照模式，回调内可安全调 SessionManager 方法

TEST(ConcurrencyTest, SessionManagerForEachConcurrentWithRemove) {
    aqua::SessionManager sm;

    // 创建 100 个 session 并 establish
    std::vector<aqua::SessionManager::session_id_t> ids;
    for (int i = 0; i < 100; ++i) {
        auto id = sm.create_session();
        ASSERT_TRUE(id.has_value());
        ids.push_back(*id);
        asio::ip::udp::endpoint ep(asio::ip::make_address("127.0.0.1"),
                                   static_cast<std::uint16_t>(10000 + i));
        sm.establish_udp(*id, ep);
    }

    std::atomic<bool> stop{false};
    std::atomic<int> for_each_iters{0};

    // 线程 A：持续 for_each_connected
    std::thread for_each_thread([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            sm.for_each_connected([&](auto id, const auto& /*ep*/) {
                (void)id;
                return true;  // 遍历全部
            });
            for_each_iters.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // 线程 B：并发 remove
    std::thread remove_thread([&] {
        for (int i = 0; i < 50; ++i) {
            sm.remove_session(ids[i]);
        }
    });

    remove_thread.join();
    stop.store(true, std::memory_order_relaxed);
    for_each_thread.join();

    // 验证最终状态一致
    EXPECT_EQ(sm.session_count(), 50u);
    EXPECT_GT(for_each_iters.load(), 0);
}

// ==== 7. SessionManager: 并发 create + establish + touch ====

TEST(ConcurrencyTest, SessionManagerConcurrentCreateEstablishTouch) {
    aqua::SessionManager sm;
    std::atomic<int> created{0};
    std::atomic<int> established{0};

    constexpr int NUM_THREADS = 8;
    constexpr int PER_THREAD = 50;
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < PER_THREAD; ++i) {
                auto id = sm.create_session();
                if (!id.has_value()) continue;
                created.fetch_add(1, std::memory_order_relaxed);

                asio::ip::udp::endpoint ep(asio::ip::make_address("127.0.0.1"),
                                          static_cast<std::uint16_t>(10000 + (i % 1000)));
                if (sm.establish_udp(*id, ep)) {
                    established.fetch_add(1, std::memory_order_relaxed);
                }
                sm.touch_session(*id);
            }
        });
    }

    for (auto& th : threads) th.join();

    EXPECT_EQ(created.load(), NUM_THREADS * PER_THREAD);
    EXPECT_EQ(established.load(), NUM_THREADS * PER_THREAD);
    EXPECT_EQ(sm.session_count(), static_cast<size_t>(NUM_THREADS * PER_THREAD));
}

// ==== 8. SpscRingBuffer: 高负载 SPSC 数据完整性 ====
// 加大压力：100 万字节，小分块

TEST(ConcurrencyTest, SpscRingBufferHighLoadIntegrity) {
    aqua::audio::SpscRingBuffer rb(64 * 1024);

    constexpr std::size_t TOTAL = 1024 * 1024;  // 1MB
    constexpr std::size_t CHUNK = 37;  // 故意不整除，制造回绕

    std::vector<std::byte> send_data(TOTAL);
    for (std::size_t i = 0; i < TOTAL; ++i) {
        send_data[i] = static_cast<std::byte>(i & 0xFF);
    }

    std::vector<std::byte> recv_data(TOTAL);
    std::atomic<bool> producer_done{false};

    std::thread producer([&] {
        std::size_t sent = 0;
        while (sent < TOTAL) {
            std::size_t to_send = std::min(CHUNK, TOTAL - sent);
            std::size_t w = rb.write(std::span<const std::byte>{send_data.data() + sent, to_send});
            sent += w;
        }
        producer_done.store(true, std::memory_order_relaxed);
    });

    std::thread consumer([&] {
        std::size_t received = 0;
        while (received < TOTAL) {
            std::size_t to_recv = std::min(CHUNK, TOTAL - received);
            std::size_t r = rb.read(std::span<std::byte>{recv_data.data() + received, to_recv});
            received += r;
        }
    });

    producer.join();
    consumer.join();

    // 字节级校验
    EXPECT_EQ(send_data, recv_data);
}

// ==== 9. SpscRingBuffer: available_read/available_write 并发下不越界 ====

TEST(ConcurrencyTest, SpscRingBufferAvailableBoundsUnderConcurrency) {
    aqua::audio::SpscRingBuffer rb(1024);
    std::atomic<bool> stop{false};

    std::thread producer([&] {
        std::vector<std::byte> buf(64, std::byte{0xAA});
        while (!stop.load(std::memory_order_relaxed)) {
            rb.write(buf);
        }
    });

    std::thread consumer([&] {
        std::vector<std::byte> buf(64);
        while (!stop.load(std::memory_order_relaxed)) {
            rb.read(buf);
        }
    });

    // 主线程持续检查真正的不变量：available_read / available_write 各自落在 [0, capacity]。
    // 注意：ar + aw == capacity 只在"同一次快照"下成立；两个 getter 各自独立读
    // write_pos/read_pos（两次快照），并发下 w/r 可能在两次调用间变化，等式不恒成立，
    // 不能作为断言（否则 flaky）。
    for (int i = 0; i < 1000; ++i) {
        auto ar = rb.available_read();
        auto aw = rb.available_write();
        EXPECT_LE(ar, rb.capacity());
        EXPECT_LE(aw, rb.capacity());
    }

    stop.store(true, std::memory_order_relaxed);
    producer.join();
    consumer.join();
}

// ==== 10. DiagnosticsManager: 高并发 record_audio_bytes 计数精确 ====

TEST(ConcurrencyTest, DiagnosticsManagerConcurrentCounterIncrement) {
    std::size_t rb_fill = 0;
    aqua::diag::DiagnosticsManager dm(
        48000, 8, PAYLOAD_SIZE, [&rb_fill]() { return rb_fill; }, PAYLOAD_SIZE * 16);

    constexpr int NUM_THREADS = 8;
    constexpr int PER_THREAD = 1000;
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < PER_THREAD; ++i) {
                dm.record_audio_bytes(100);
                dm.record_hello_ack();
                dm.record_underrun();
                dm.record_deadline_miss();
            }
        });
    }

    for (auto& th : threads) th.join();

    aqua::jitter::JitterBuffer jb(make_test_format(), FRAMES_PER_PACKET, 4, 16);
    dm.collect_and_log(jb);
    auto snap = dm.snapshot();

    // atomic 计数应精确
    EXPECT_EQ(snap.recv_audio_bytes, static_cast<std::uint64_t>(NUM_THREADS * PER_THREAD * 100));
    EXPECT_EQ(snap.recv_hello_acks, static_cast<std::uint64_t>(NUM_THREADS * PER_THREAD));
    EXPECT_EQ(snap.underruns, static_cast<std::uint64_t>(NUM_THREADS * PER_THREAD));
    EXPECT_EQ(snap.deadline_misses, static_cast<std::uint64_t>(NUM_THREADS * PER_THREAD));
}
