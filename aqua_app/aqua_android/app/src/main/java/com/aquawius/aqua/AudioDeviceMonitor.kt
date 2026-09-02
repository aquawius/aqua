package com.aquawius.aqua

import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Handler
import android.os.Looper

/**
 * 播放设备监视器：设备列表 + 变化通知（playback_switching_design.md §9）。
 *
 * AquaService 持有（后台播放期间存活，不放 Activity——后台时设备事件
 * 也要能驱动自动切换）。AudioDeviceCallback 固定派发到主线程（显式
 * Handler），回调体只做快照与 diff，合并去抖与切换决策在 Controller。
 *
 * - 设备列表：用户可感知的输出设备（主页设备弹层的数据源）；
 * - 新设备接入：蓝牙 / USB / 有线耳机等可切换设备首次出现时通知一次
 *   （扬声器等常驻设备不触发；注册时的初始列表也不触发）。
 */
class AudioDeviceMonitor(private val audioManager: AudioManager) {

    /** 输出设备列表变化（主线程回调；参数为当前快照）。 */
    var onDevicesChanged: ((List<AudioDeviceInfo>) -> Unit)? = null

    /** 可切换的新输出设备接入（主线程回调；合并去抖在上层）。 */
    var onSelectableOutputAdded: (() -> Unit)? = null

    /** 已知可切换设备 id（diff 基准）。回调固定在主线程（显式 Handler），
     *  与 start() 的初始基准同线程，无跨线程可见性问题。 */
    private var knownSelectableIds: Set<Int> = emptySet()

    private val callback = object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(added: Array<out AudioDeviceInfo>) {
            refresh(added)
        }

        override fun onAudioDevicesRemoved(removed: Array<out AudioDeviceInfo>) {
            refresh(removed)
        }
    }

    fun start() {
        // 注册前先建 diff 基准：registerAudioDeviceCallback 会立即以现有
        // 设备回调 onAudioDevicesAdded，不能误报为"新设备接入"。
        refresh(changed = emptyArray())
        audioManager.registerAudioDeviceCallback(callback, Handler(Looper.getMainLooper()))
    }

    fun stop() {
        audioManager.unregisterAudioDeviceCallback(callback)
    }

    private fun refresh(changed: Array<out AudioDeviceInfo>) {
        val outputs = audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
            .filter { isSelectableOutput(it) }
        onDevicesChanged?.invoke(outputs)

        val selectableIds = outputs.map { it.id }.toSet()
        val newIds = selectableIds - knownSelectableIds
        if (changed.any { isSelectableOutput(it) } && newIds.isNotEmpty()) {
            onSelectableOutputAdded?.invoke()
        }
        knownSelectableIds = selectableIds
    }

    companion object {
        /** 用户可感知的输出设备类型（弹层与自动切换的范围；排除听筒/通话等）。 */
        fun isSelectableOutput(device: AudioDeviceInfo): Boolean {
            if (!device.isSink) return false
            return when (device.type) {
                AudioDeviceInfo.TYPE_BUILTIN_SPEAKER,
                AudioDeviceInfo.TYPE_WIRED_HEADSET,
                AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
                AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
                AudioDeviceInfo.TYPE_BLE_HEADSET,
                AudioDeviceInfo.TYPE_USB_HEADSET,
                AudioDeviceInfo.TYPE_USB_DEVICE,
                AudioDeviceInfo.TYPE_USB_ACCESSORY,
                -> true
                else -> false
            }
        }

        /** 设备类型文案（弹层 supporting 文本）。 */
        fun typeLabel(type: Int): String = when (type) {
            AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> "扬声器"
            AudioDeviceInfo.TYPE_WIRED_HEADSET -> "有线耳机"
            AudioDeviceInfo.TYPE_WIRED_HEADPHONES -> "有线耳机"
            AudioDeviceInfo.TYPE_BLUETOOTH_A2DP -> "蓝牙"
            AudioDeviceInfo.TYPE_BLE_HEADSET -> "蓝牙 LE"
            AudioDeviceInfo.TYPE_USB_HEADSET -> "USB 耳机"
            AudioDeviceInfo.TYPE_USB_DEVICE -> "USB 音频"
            AudioDeviceInfo.TYPE_USB_ACCESSORY -> "USB 配件"
            else -> "音频设备"
        }
    }
}
