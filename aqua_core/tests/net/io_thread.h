#ifndef AQUA_TEST_IO_THREAD_H
#define AQUA_TEST_IO_THREAD_H

// 测试共享 helper：让 asio::io_context 在独立线程里持续 run()，析构时停止并 join。
// run() 在暂时没有待处理工作时会直接返回；用 work_guard 保活，直到 stop() 才退出。
// 否则并发场景下 io 短暂空闲后新投递的 handler（如 stop() 的 close_state）会因
// 无人 run 而永远不执行。
//
// std::jthread：析构自动 request_stop + join —— 测试用例里 ASSERT_* 早退时也不会
// 漏 join（漏 join 的 std::thread 析构 = std::terminate，整轮测试直接崩）。
// 但 stop_token 本身不会让 io_context::run() 返回：必须靠 stop_callback 显式
// io.stop()，否则 jthread 析构会永久挂起在 join 上（比崩溃更难排查）。

#include <asio.hpp>

#include <stop_token>
#include <thread>
#include <utility>

namespace aqua::test {

struct IoThread {
    explicit IoThread(asio::io_context& io)
        : io(io)
        , thread([&io](std::stop_token st) {
            std::stop_callback cb(st, [&io] { io.stop(); });
            const auto guard = asio::make_work_guard(io);
            io.run();
        })
    {
    }

    // 持有线程的对象不可拷贝/移动（聚合类型默认可拷贝，误用会双 join）。
    IoThread(const IoThread&) = delete;
    IoThread& operator=(const IoThread&) = delete;

    asio::io_context& io;
    std::jthread thread;
};

} // namespace aqua::test

#endif // AQUA_TEST_IO_THREAD_H
