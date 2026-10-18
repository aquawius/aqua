#include "jb_presets.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace aqua::gui {
namespace {

// 与 Android 的 JbPreset 逐档对齐（容量 / 下限 / stall 峰值 / 衰减 / 欠载惩罚）。
// 字段为 0 = 不显式设置、取 core 默认。
// 与 Android 的一处有意分歧：Android 的 stallThresholdPackets 缺省传 0（= 关检测），
// 桌面 GUI 与 CLI 一致——不传即 core 默认 5.0（检测开启）。
struct JbPresetRow {
    const char* label;
    const char* short_hint;
    std::uint32_t capacity = 0;
    std::uint32_t min_target = 0;
    double stall_peak_cap = 0.0;
    double stall_decay = 0.0;
    double stall_threshold = 0.0;
    double underrun_penalty = 0.0;
};

constexpr JbPresetRow kJbPresets[] = {
    { "极低延迟", "容量 16 槽", 16 },
    { "LAN", "下限 5 槽", 0, 5, 8.0, 10.0 },
    { "Wi-Fi", "下限 7 槽", 0, 7, 10.0, 10.0 },
    { "Wi-Fi 稳定", "下限 9 槽", 0, 9, 12.0, 10.0 },
    { "Wi-Fi 拥挤", "下限 12 槽", 0, 12, 14.0, 10.0 },
    { "公网", "容量 60 槽", 60, 12, 16.0, 5.0 },
    { "50 ms", "下限 14 槽", 60, 14, 16.0, 5.0 },
    { "100 ms", "下限 27 槽", 60, 27, 16.0, 5.0 },
    { "弱网", "容量 80 槽", 80, 32, 16.0, 5.0, 0.0, 2.0 },
    { "200 ms", "下限 55 槽", 120, 55, 20.0, 5.0, 0.0, 2.0 },
    { "恶劣", "容量 160 槽", 160, 80, 24.0, 3.0, 0.0, 3.0 },
};

static_assert(static_cast<int>(sizeof(kJbPresets) / sizeof(kJbPresets[0])) == kJbPresetCount,
    "kJbPresetCount 与预设表长度不一致");

const JbPresetRow& preset_row(int index)
{
    if (index < 0 || index >= kJbPresetCount) {
        index = 0; // 越界回落第 0 档（UI 下标由 UiState 夹住，这里双保险）
    }
    return kJbPresets[index];
}

// 参数规格表（定义见 jb_presets.h 的 JbParamSpec）。
// 标签 / 范围 / 默认值文案与 Android 的 AdvancedScreen 逐项一致；说明文字
// 对齐 core 注释与 Android 的 tip，改语义时两边一起改。
constexpr JbParamSpec kParamSpecs[kJbParamCount] = {
    { "缓冲容量", "槽",
      "环形缓冲的「容量」（吸收抖动与断流的本钱），不等于实际延迟；\n"
      "target 最多用到它的 2/3。\n"
      "0 = 默认 30 槽。",
      30.0, 0.0, 400.0, true },
    { "最低水位", "槽",
      "自动水位的最小值，决定延迟下限。\n"
      "非低延迟模式建议不小于 40 槽。\n"
      "0 = 默认 3 槽。",
      3.0, 0.0, 100.0, true },
    { "断流应对上限", "槽",
      "断流（到达间隔突增）期间允许追加的水位上限。\n"
      "0 = 默认 12.0 槽。",
      12.0, 0.0, 40.0, false },
    { "断流记忆衰减", "ms/s",
      "断流峰值「被遗忘」的速度：每过 1 秒没有新断流，\n记住的峰值就减掉这么多。\n"
      "0 = 默认 5.0 ms/s。",
      5.0, 0.0, 30.0, false },
    { "断流判定门槛", "包周期",
      "判定为断流的到达间隔阈值（单位 = 包的时长）；\n断流与普通抖动走不同的处理路径。\n"
      "0 = 默认 5.0 个包。",
      5.0, 0.0, 12.0, false },
    { "欠载加固步长", "槽/次",
      "每次欠载后抬升水位下限的步长（闭环补偿项，累计上限 6 槽，\n无欠载时逐步回落；链路正常时恒为 0，不引入额外延迟）。\n"
      "0 = 默认 1.0 槽。",
      1.0, 0.0, 6.0, false },
};

} // namespace

const JbParamSpec (&jb_param_specs())[kJbParamCount]
{
    return kParamSpecs;
}

JbParams jb_effective(JbParams raw)
{
    if (raw.capacity == 0) {
        raw.capacity = kJbDefaultCapacity;
    }
    if (raw.min_target == 0) {
        raw.min_target = kJbDefaultMinTarget;
    }
    if (raw.stall_peak_cap == 0.0) {
        raw.stall_peak_cap = kJbDefaultStallPeakCap;
    }
    if (raw.stall_decay == 0.0) {
        raw.stall_decay = kJbDefaultStallDecay;
    }
    if (raw.stall_threshold == 0.0) {
        raw.stall_threshold = kJbDefaultStallThreshold;
    }
    if (raw.underrun_penalty == 0.0) {
        raw.underrun_penalty = kJbDefaultUnderrunPenalty;
    }
    return raw;
}

// 下标 ↔ 字段的唯一映射（顺序 = kParamSpecs = JbParams 字段声明顺序）。
// 加参数时：先改 kParamSpecs，再在这里补 case，两处顺序必须一致。
double jb_param_get(const JbParams& p, int index)
{
    switch (index) {
    case 0: return static_cast<double>(p.capacity);
    case 1: return static_cast<double>(p.min_target);
    case 2: return p.stall_peak_cap;
    case 3: return p.stall_decay;
    case 4: return p.stall_threshold;
    case 5: return p.underrun_penalty;
    default: return 0.0;
    }
}

void jb_param_set(JbParams& p, int index, double value)
{
    switch (index) {
    case 0: p.capacity = static_cast<std::uint32_t>(std::llround(value)); break;
    case 1: p.min_target = static_cast<std::uint32_t>(std::llround(value)); break;
    case 2: p.stall_peak_cap = value; break;
    case 3: p.stall_decay = value; break;
    case 4: p.stall_threshold = value; break;
    case 5: p.underrun_penalty = value; break;
    default: break;
    }
}

const char* jb_preset_label(int index)
{
    return preset_row(index).label;
}

const char* jb_preset_hint(int index)
{
    return preset_row(index).short_hint;
}

JbParams jb_resolve(int index)
{
    const JbPresetRow& row = preset_row(index);
    JbParams raw;
    raw.capacity = row.capacity;
    raw.min_target = row.min_target;
    raw.stall_peak_cap = row.stall_peak_cap;
    raw.stall_decay = row.stall_decay;
    raw.stall_threshold = row.stall_threshold;
    raw.underrun_penalty = row.underrun_penalty;
    return jb_effective(raw);
}

void jb_param_rows(int index, JbParamRow (&out)[kJbParamCount])
{
    const JbPresetRow& row = preset_row(index);
    const JbParams p = jb_resolve(index);

    struct Source {
        double value; // 表内原值（0 = 未显式设置）
        double resolved;
    };
    const Source sources[kJbParamCount] = {
        { static_cast<double>(row.capacity), static_cast<double>(p.capacity) },
        { static_cast<double>(row.min_target), static_cast<double>(p.min_target) },
        { row.stall_peak_cap, p.stall_peak_cap },
        { row.stall_decay, p.stall_decay },
        { row.stall_threshold, p.stall_threshold },
        { row.underrun_penalty, p.underrun_penalty },
    };

    for (int i = 0; i < kJbParamCount; ++i) {
        const JbParamSpec& spec = kParamSpecs[i];
        out[i] = JbParamRow { spec.name, spec.unit, spec.desc,
            sources[i].resolved, sources[i].value != 0.0 };
    }
}

std::string jb_preset_tooltip(int index)
{
    const JbPresetRow& row = preset_row(index);
    JbParamRow rows[kJbParamCount];
    jb_param_rows(index, rows);

    std::string out;
    out.reserve(400);
    out += row.label;
    out += "（";
    out += row.short_hint;
    out += "）\n";
    for (const JbParamRow& r : rows) {
        char line[128];
        std::snprintf(line, sizeof(line), "%s：%g %s%s\n",
            r.name, r.value, r.unit, r.explicit_set ? "" : "（core 默认）");
        out += line;
    }
    out += "\n改动于下次连接生效。";
    return out;
}

} // namespace aqua::gui
