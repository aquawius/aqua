#ifndef AQUA_APP_GUI_UI_FORMAT_H
#define AQUA_APP_GUI_UI_FORMAT_H

// 显示层格式化：字节 / 时长 / 采样率 / 声道 / 毫秒 / 速率。
//
// 放公共层的原因：这些是"同一个值在 client 与 server 上必须看起来一样"的东西。
// unit 规则也只在这里写一次——core 侧一律帧 / 槽 / ns，ms 与人类可读单位在边界
// 派生，UI 不各自定义一套换算。

#include <imgui.h>

#include <cstdint>
#include <string>

namespace aqua::gui {

// 等宽数值字号 = UI 基准字号 × 这个比例（指标 / 日志 / 统计值统一）。
inline constexpr float kMonoRatio = 0.875f;

// 等宽字号（传 UI 基准字号，不掺 DPI：PushFont 收的是未缩放基准值，
// FontScaleDpi 会再统一乘）。
float mono_size_for(int ui_font_size);

// 等宽数值和同行普通标签**共用基线**要往下挪的像素。
// ImGui 的文本是顶对齐的：两种字体 ascent 不同（16px 雅黑标签旁边的 14px
// Consolas 数值会浮高 4px），直接顶对齐看着就是"数字比标签高一点"。
float mono_baseline_dy_for(ImFont* mono, float mono_px);


// 字节数 → "512 B" / "1.5 KB" / "12.34 MB"（裸数字读不出量级）。
std::string format_bytes(std::uint64_t bytes);

// 秒 → "mm:ss"（≥1h 为 "h:mm:ss"）。状态栏与统计页共用。
std::string format_duration(double seconds);

// 采样率 → "48 kHz" / "44100 Hz"。
std::string format_sample_rate(int hz);

// 声道数 → "立体声" / "单声道" / "N 声道"。
std::string format_channels(int channels);

// 毫秒：< 10 ms 给两位小数，否则一位。诊断读数用——"0.0 ms" 这种没有信息量的
// 显示比不显示更糟（回调耗时常态就是 0.02 ms）。
std::string format_ms(double ms);

// 每秒速率：< 1000 给一位小数，否则 "12.4k" 这种紧凑写法。
std::string format_rate(double per_second);

} // namespace aqua::gui

#endif // AQUA_APP_GUI_UI_FORMAT_H
