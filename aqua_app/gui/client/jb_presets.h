#ifndef AQUA_APP_GUI_JB_PRESETS_H
#define AQUA_APP_GUI_JB_PRESETS_H

// 网络预设（JitterBuffer 档位）与参数规格的单一数据源。
// 数值与 Android 的 JbPreset / AdvancedScreen 逐项对齐；本模块同时是
// UiState → ClientRuntimeConfig jb_* 字段的映射落点。
//
// 字段语义与 core 一一对应（见 aqua_core/audio/buffer/buffer_config.h）：
//   capacity        → JB_DEFAULT_CAPACITY_SLOTS（target 不超过它的 2/3）
//   min_target      → JB_ADAPTIVE_DEFAULT_MIN_TARGET_SLOTS（target 硬下限）
//   stall_peak_cap  → JB_ADAPTIVE_STALL_PEAK_CAP_SLOTS（一次大 stall 的 target 上限）
//   stall_decay     → JB_ESTIMATOR_STALL_PEAK_DECAY_MS_PER_SEC（高位回落速率 ms/s）
//   stall_threshold → JB_ESTIMATOR_DEFAULT_STALL_THRESHOLD_PACKETS（包周期）
//   underrun_penalty→ JB_ADAPTIVE_UNDERRUN_PENALTY_SLOTS（每次欠载抬升的槽数）

#include <cstdint>
#include <string>

namespace aqua::gui {

// core 默认值（预设表 / 自定义值里字段为 0 = 不显式设置、取这些默认）。
// 与 aqua_core 的 JitterBuffer 默认参数保持一致，改动时两边同步。
constexpr std::uint32_t kJbDefaultCapacity = 30;       // 容量（槽）
constexpr std::uint32_t kJbDefaultMinTarget = 3;       // 目标下限（槽）
constexpr double kJbDefaultStallPeakCap = 12.0;        // stall 峰值上限（槽）
constexpr double kJbDefaultStallDecay = 5.0;           // stall 高位回落（ms/s）
constexpr double kJbDefaultStallThreshold = 5.0;       // stall 阈值（包周期）
constexpr double kJbDefaultUnderrunPenalty = 1.0;      // 欠载惩罚（槽/次）

// 档位总数（inline constexpr + cpp 内 static_assert 与表长对齐）。
inline constexpr int kJbPresetCount = 11;

// 一次连接实际使用的 6 个 JB 参数（已解析：0 字段已回落 core 默认）。
// 会话层映射 ClientRuntimeConfig 时用这个，不用自己再解析一遍。
// 「自定义配置」模式下 UiState::jb_custom 存的是**原始值**（0 = core 默认，
// 与 Android 滑条同语义），组装连接配置时过一遍 jb_effective() 再交给会话层。
struct JbParams {
    std::uint32_t capacity = kJbDefaultCapacity;
    std::uint32_t min_target = kJbDefaultMinTarget;
    double stall_peak_cap = kJbDefaultStallPeakCap;
    double stall_decay = kJbDefaultStallDecay;
    double stall_threshold = kJbDefaultStallThreshold;
    double underrun_penalty = kJbDefaultUnderrunPenalty;
};

// 参数规格：编辑（设置→高级的滑条）与展示（预设 tooltip / 只读值）共用一份，
// 保证滑条范围、默认值、文案三处永远讲同一套话。标签与 Android 的
// AdvancedScreen 逐项一致（跨端可对照）。
struct JbParamSpec {
    const char* name;     // 人话标签
    const char* unit;     // 单位（"槽" / "ms/s" / "包周期" / "槽/次"）
    const char* desc;     // 口径说明（悬浮提示；可含换行）
    double default_value; // core 默认；编辑值 0 = 取默认（与 Android 同语义）
    double min;           // 编辑下限（恒 0 = 默认档）
    double max;           // 编辑上限（对齐 Android 滑条范围）
    bool integer;         // 整数参数（用 SliderInt）
};

inline constexpr int kJbParamCount = 6;

// 6 个参数的规格，顺序固定：容量 / 最低水位 / 断流应对上限 / 断流记忆衰减 /
// 断流判定门槛 / 欠载加固步长（与 JbParams 字段一一对应）。
const JbParamSpec (&jb_param_specs())[kJbParamCount];

// 原始值 → 有效值：0 字段回落 core 默认。预设解析与自定义配置共用。
JbParams jb_effective(JbParams raw);

// 按下标（与 jb_param_specs() 同一顺序）读写 JbParams 的单个字段。
// 设置→高级的 6 条滑条靠它统一驱动，省掉 6 段重复的 widget 代码；
// 越界下标安全（get 返回 0 = core 默认，set 忽略）。
double jb_param_get(const JbParams& p, int index);
void jb_param_set(JbParams& p, int index, double value);

// 档位显示名（越界下标回落第 0 档）。
const char* jb_preset_label(int index);

// 解析档位 → 完整参数（0 字段取 core 默认）。
JbParams jb_resolve(int index);

// 一个参数的可展示行：滑条 tooltip 与设置→高级 tab 共用，保证两处口径一致。
struct JbParamRow {
    const char* name;   // 参数名
    const char* unit;   // 单位
    const char* desc;   // 一句话口径说明
    double value;       // 解析后的有效值
    bool explicit_set;  // false = 取 core 默认
};

// 解析档位 → 6 个可展示行。
void jb_param_rows(int index, JbParamRow (&out)[kJbParamCount]);

// 悬浮提示全文：档位名 + 6 个参数（含"（core 默认）"标注）+ 生效时机。
std::string jb_preset_tooltip(int index);

} // namespace aqua::gui

#endif // AQUA_APP_GUI_JB_PRESETS_H
