# 模块：AudioCapture / WASAPI Capture

## 抽象

`AudioCapture` 是 output push interface：

```text
OS -> AudioBlock callback
```

callback 必须 realtime-safe；backend 自己保证线程退出后 stop 才返回。

## WASAPI loopback

Server 默认 `OUTPUT_LOOPBACK`。Loopback endpoint 的数据来自系统 output engine，事件粒度不是 Aqua packetizer 需要的固定
F，因此 Capture -> Packetizer 必须允许变长 block。

## 事件饥饿 fallback

WASAPI loopback 在最后一个 render client 消失后可能进入 quiescence，audio event 可能长期不触发。当前 backend 不使用无限等待：

```text
20ms timeout
   ├─ GetNextPacketSize 有数据 -> 正常 drain
   └─ 无数据 -> 根据墙钟时长生成合成静音
```

连续 2 次 timeout（约 40ms）才把诊断状态标成 `Starved`；第一次 timeout 就可能生成 synthetic silence，避免 capture 时间轴停止。

单次合成静音上限约 150ms，防止系统从长时间挂起恢复后瞬间制造巨大 burst。

## 运行期错误

设备拔出、音频服务异常等通过 event_callback 通知 Runtime，使状态转为 Degraded；不在 backend event thread 里直接 `stop()`
，因为那可能 join 自己。
