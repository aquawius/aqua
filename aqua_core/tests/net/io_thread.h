#ifndef AQUA_TEST_IO_THREAD_H
#define AQUA_TEST_IO_THREAD_H

// 测试共享 helper：让 asio::io_context 在独立线程里持续 run()，析构时 stop() 并 join。
// run() 在暂时没有待处理工作时会直接返回；用 work_guard 保活，直到 stop() 才退出。
// 否则并发场景下 io 短暂空闲后新投递的 handler（如 stop() 的 close_state）会因
// 无人 run 而永远不执行。

#include <asio.hpp>

#include <thread>

namespace aqua::test {

struct IoThread {
    explicit IoThread(asio::io_context& io)
        : io(io)
        , thread([&io] {
            asio::executor_work_guard<asio::io_context::executor_type> guard(io.get_executor());
            io.run();
        })
    {
    }

    ~IoThread()
    {
        io.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    asio::io_context& io;
    std::thread thread;
};

} // namespace aqua::test

#endif // AQUA_TEST_IO_THREAD_H
