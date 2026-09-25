#include "aqua/audio/devices/device_set_poller.h"

#include "aqua/logger/logger.h"

#include <algorithm>
#include <iterator>

namespace aqua::audio {

std::string format_device_ids(const std::vector<AudioDeviceId>& ids)
{
    std::string out = "[";
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += ids[i].value();
    }
    out += ']';
    return out;
}

std::optional<std::vector<AudioDeviceId>> DeviceSetPoller::enumerate_ids() const noexcept
{
    try {
        const auto devices = devices_->enumerate(direction_);
        bool has_empty = false;
        std::vector<AudioDeviceId> ids;
        ids.reserve(devices.size());
        for (const auto& device : devices) {
            // 空 id 是"跟随系统"的合成条目而非设备（Android 恒有一条）。
            // WASAPI 从不返回空 id，故“过滤后为空”须看原始项：含空项=合成
            // （无信息），原始空列表=真零设备（有效空集）。
            if (device.id.empty()) {
                has_empty = true;
                continue;
            }
            ids.push_back(device.id);
        }
        if (ids.empty() && has_empty) {
            return std::nullopt;
        }
        // 枚举顺序不保证稳定：排序后比较，否则顺序抖动会被当成设备插拔。
        std::ranges::sort(ids);
        return ids;
    } catch (const std::exception& e) {
        log_debug_fmt("DeviceSetPoller: enumeration threw, treating as no information: {}",
            format_exception_message(e));
        return std::nullopt;
    } catch (...) {
        log_debug("DeviceSetPoller: enumeration threw, treating as no information");
        return std::nullopt;
    }
}

std::optional<DeviceSetChange> DeviceSetPoller::poll() noexcept
{
    auto sampled_opt = enumerate_ids();
    if (!sampled_opt) {
        // 无信息：Android 合成条目 / 枚举抛异常。不得冲掉已有基线与去抖——
        // 否则 Android 上每个 tick 都会重置确认计数，永远确认不了变化。
        return std::nullopt;
    }
    auto sampled = std::move(*sampled_opt);

    if (sampled != sampled_) {
        sampled_ = std::move(sampled);
        sample_hits_ = 1; // 首次见到该集合：开始去抖，本次不上报
        return std::nullopt;
    }
    if (sample_hits_ < kConfirmations) {
        ++sample_hits_;
        if (sample_hits_ < kConfirmations) {
            return std::nullopt;
        }
    }
    if (has_reported_ && reported_ == sampled_) {
        return std::nullopt; // 连续一致但与上次上报相同：无变化
    }

    DeviceSetChange change;
    change.present = sampled_;
    if (has_reported_) {
        // 两侧都已排序（enumerate_ids 保证），可直接用集合差。
        std::ranges::set_difference(sampled_, reported_, std::back_inserter(change.added));
        std::ranges::set_difference(reported_, sampled_, std::back_inserter(change.removed));
    }
    reported_ = sampled_;
    has_reported_ = true;
    return change;
}

void DeviceSetPoller::reset() noexcept
{
    sampled_.clear();
    sample_hits_ = 0;
    reported_.clear();
    has_reported_ = false;
}

} // namespace aqua::audio
