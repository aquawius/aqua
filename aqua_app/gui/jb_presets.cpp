#include "jb_presets.h"

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

// 各参数的展示元数据（名字 / 单位 / 口径说明），顺序与 JbParams 字段一致。
// 说明文字对齐 core 注释，改语义时两边一起改。
constexpr JbParamRow kParamMeta[kJbParamCount] = {
    { "缓冲容量", "槽", "JitterBuffer 槽位总数（target 不超过它的 2/3）", 0.0, true },
    { "目标下限", "槽", "target 硬下限（几何地板还可能再抬高）", 0.0, true },
    { "Stall 峰值", "槽", "一次大 stall 最多把 target 推多高", 0.0, true },
    { "Stall 衰减", "ms/s", "stall 高位每秒回落多少毫秒", 0.0, true },
    { "Stall 阈值", "包周期", "到达间隔超过 N 个包周期即判 stall", 0.0, true },
    { "欠载惩罚", "槽/次", "每次欠载把 target 下限抬升的槽数", 0.0, true },
};

} // namespace

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
    JbParams p;
    p.capacity = row.capacity != 0 ? row.capacity : kJbDefaultCapacity;
    p.min_target = row.min_target != 0 ? row.min_target : kJbDefaultMinTarget;
    p.stall_peak_cap = row.stall_peak_cap != 0.0 ? row.stall_peak_cap : kJbDefaultStallPeakCap;
    p.stall_decay = row.stall_decay != 0.0 ? row.stall_decay : kJbDefaultStallDecay;
    p.stall_threshold = row.stall_threshold != 0.0 ? row.stall_threshold : kJbDefaultStallThreshold;
    p.underrun_penalty = row.underrun_penalty != 0.0 ? row.underrun_penalty : kJbDefaultUnderrunPenalty;
    return p;
}

void jb_param_rows(int index, JbParamRow (&out)[kJbParamCount])
{
    const JbPresetRow& row = preset_row(index);
    const JbParams p = jb_resolve(index);

    struct Source {
        double value;   // 表内原值（0 = 未显式设置）
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
        out[i] = kParamMeta[i];
        out[i].value = sources[i].resolved;
        out[i].explicit_set = (sources[i].value != 0.0);
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
