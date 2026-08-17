#include <gtest/gtest.h>

#include "core/audio/ringbuffer/spsc_ringbuffer.h"

#include <thread>

using aqua::audio::SpscRingBuffer;

TEST(SpscRingBufferTest, CapacityRoundsUpToAlignment)
{
    // 向上取整为 1KiB (1024) 的倍数
    SpscRingBuffer rb(10000);
    EXPECT_EQ(rb.capacity(), 10240u);

    SpscRingBuffer rb2(8000);
    EXPECT_EQ(rb2.capacity(), 8192u);

    SpscRingBuffer rb3(8192);
    EXPECT_EQ(rb3.capacity(), 8192u); // 已是倍数，不变

    SpscRingBuffer rb4(1);
    EXPECT_EQ(rb4.capacity(), 1024u); // 最小 1KiB
}

TEST(SpscRingBufferTest, WriteThenRead)
{
    SpscRingBuffer rb(256);
    std::vector<std::byte> data(64);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = std::byte { static_cast<uint8_t>(i) };

    EXPECT_EQ(rb.write(data), 64u);
    EXPECT_EQ(rb.available_read(), 64u);

    std::vector<std::byte> out(64);
    EXPECT_EQ(rb.read(out), 64u);
    EXPECT_EQ(out, data);
    EXPECT_EQ(rb.available_read(), 0u);
}

TEST(SpscRingBufferTest, WriteFullReturnsPartial)
{
    SpscRingBuffer rb(1024);
    std::vector<std::byte> data(2048, std::byte { 0xAA });

    // 容量 1024，首次写 1024
    EXPECT_EQ(rb.write(data), 1024u);
    // 再写应该返回 0
    EXPECT_EQ(rb.write(data), 0u);
    EXPECT_EQ(rb.available_write(), 0u);
}

TEST(SpscRingBufferTest, EmptyReadReturnsZero)
{
    SpscRingBuffer rb(128);
    std::vector<std::byte> out(64);
    EXPECT_EQ(rb.read(out), 0u);
}

TEST(SpscRingBufferTest, Wraparound)
{
    SpscRingBuffer rb(1024);

    // 写 600，读 600，再写 600 — 第二次写会跨越尾部（600 + 600 > 1024）
    std::vector<std::byte> w1(600, std::byte { 0x11 });
    EXPECT_EQ(rb.write(w1), 600u);
    std::vector<std::byte> r1(600);
    EXPECT_EQ(rb.read(r1), 600u);
    EXPECT_EQ(r1, w1);

    std::vector<std::byte> w2(600, std::byte { 0x22 });
    EXPECT_EQ(rb.write(w2), 600u);
    std::vector<std::byte> r2(600);
    EXPECT_EQ(rb.read(r2), 600u);
    EXPECT_EQ(r2, w2);
}

TEST(SpscRingBufferTest, PartialReadThenContinue)
{
    SpscRingBuffer rb(256);
    std::vector<std::byte> data(128);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = std::byte { static_cast<uint8_t>(i) };

    rb.write(data);

    std::vector<std::byte> out1(50);
    EXPECT_EQ(rb.read(out1), 50u);
    EXPECT_EQ(out1, std::vector<std::byte>(data.begin(), data.begin() + 50));

    std::vector<std::byte> out2(78);
    EXPECT_EQ(rb.read(out2), 78u);
    EXPECT_EQ(out2, std::vector<std::byte>(data.begin() + 50, data.end()));
}

TEST(SpscRingBufferTest, Clear)
{
    SpscRingBuffer rb(1024);
    std::vector<std::byte> data(64, std::byte { 0x01 });
    rb.write(data);
    EXPECT_EQ(rb.available_read(), 64u);

    rb.clear();
    EXPECT_EQ(rb.available_read(), 0u);
    EXPECT_EQ(rb.available_write(), 1024u);
}

TEST(SpscRingBufferTest, ConcurrentProducerConsumer)
{
    SpscRingBuffer rb(4096);
    constexpr size_t TOTAL = 100000;
    std::vector<std::byte> produced(TOTAL);
    for (size_t i = 0; i < TOTAL; ++i)
        produced[i] = std::byte { static_cast<uint8_t>(i & 0xFF) };

    std::vector<std::byte> consumed(TOTAL);

    std::thread producer([&] {
        size_t offset = 0;
        while (offset < TOTAL) {
            size_t chunk = std::min(size_t(500), TOTAL - offset);
            size_t written = rb.write(std::span<const std::byte> { produced.data() + offset, chunk });
            offset += written;
        }
    });

    std::thread consumer([&] {
        size_t offset = 0;
        while (offset < TOTAL) {
            size_t chunk = std::min(size_t(500), TOTAL - offset);
            size_t got = rb.read(std::span<std::byte> { consumed.data() + offset, chunk });
            offset += got;
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(consumed, produced);
}
