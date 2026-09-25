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

std::vector<AudioDeviceId> DeviceSetPoller::enumerate_ids() const noexcept
{
    try {
        const auto devices = devices_->enumerate(direction_);
        std::vector<AudioDeviceId> ids;
        ids.reserve(devices.size());
        for (const auto& device : devices) {
            // 空 id 是"跟随系统"的合成条目而非设备（Android 只返回这一条）。
            // 剔除后为空即"无信息"，由 poll() 统一处理。
            if (!device.id.empty()) {
                ids.push_back(device.id);
            }
        }
        // 枚举顺序不保证稳定：排序后比较，否则顺序抖动会被当成设备插拔。
        std::ranges::sort(ids);
        return ids;
    } catch (const std::exception& e) {
        log_debug_fmt("DeviceSetPoller: enumeration threw, treating as no information: {}",
            format_exception_message(e));
        return { };
    } catch (...) {
        log_debug("DeviceSetPoller: enumeration threw, treating as no information");
        return { };
    }
}

std::optional<DeviceSetChange> DeviceSetPoller::poll() noexcept
{
    auto sampled = enumerate_ids();
    if (sampled.empty()) {
        // 无信息：平台不可枚举（Android 合成条目）/ 枚举抛异常 / 真的一个设备
        // 都没有。三者都不足以支撑决策，且都不得冲掉已有基线——否则 Android
        // 上每个 tick 都会把去抖状态重置，永远确认不了任何变化。
        return std::nullopt;
    }

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
