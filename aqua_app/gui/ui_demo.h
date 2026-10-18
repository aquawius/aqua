#ifndef AQUA_APP_GUI_UI_DEMO_H
#define AQUA_APP_GUI_UI_DEMO_H

// 演示数据（AQUA_GUI_DEMO=1）：全部是拟真常量/函数，不发起任何真实连接。
// 只给"没有真实数据可用"的预览场景用；界面本身对真实会话与演示模式走同一套
// 渲染代码（值来源在 ui_metrics.cpp 的 metric_text 里二选一）。刻意不依赖任何
// core 头——它是纯前端数据。

namespace aqua::gui {

// 往全局日志缓冲写几条演示日志（只在演示模式首次启动时调一次）。
void seed_demo_logs();

// 缓冲侧 6 个指标的演示值：
//   { water_level, lead_ms, target_ms, jitter_ms, gap_ratio, underrun_ratio }
// 与 ui_metrics.cpp 的 MetricId 前 6 项一一对应。
void fill_demo_tiles(double (&v)[6]);

// 同一套公式在任意时刻的取值：遥测面板要把"过去 60 秒"补出来（否则第一屏的
// 趋势图是空的，演示模式下看着像坏了）。
void fill_demo_tiles_at(double t, double (&v)[6]);

// 链路侧指标的演示值：{ 占用槽, 容量槽, 到达包速, ACK 延迟 ms, 乱序, 迟到,
// 重锚, 掩盖, 目标下限槽, 目标上限槽 }。
constexpr int kDemoLinkCount = 10;
void fill_demo_link(double (&v)[kDemoLinkCount]);

// 演示模式的音频格式：遥测面板第一行与设置→统计的「音频格式」分项共用同一份，
// 免得两处各写一个常量、改一处忘一处。取值与真实会话同量级（48 kHz / 立体声 /
// PCM F32LE / 1536 kbps / 480 帧 ≈ 10 ms）。
struct DemoFormat {
    const char* summary; // 一行摘要（遥测面板直接显示）
    const char* encoding; // "PCM F32LE"
    int sample_rate;      // Hz
    int channels;
    int bits_per_sample;
    int bitrate_kbps;
    int frame_count;  // 每包帧数 F
    double packet_ms; // 包时长
};
DemoFormat demo_format();

} // namespace aqua::gui

#endif // AQUA_APP_GUI_UI_DEMO_H
