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

## 事件饥饿 fallback（欠账驱动的时间轴补偿）

WASAPI loopback 在最后一个 render client 消失后可能进入 quiescence，audio event 可能长期不触发；
切歌等 render 流重建期间 engine 也可能**反复 signal event 但不产出 packet**（空事件），或零星吐小包
（部分饥饿）。采集时间轴必须与 engine 的 event 行为解耦、恒以 1x 墙钟速率推进——这是 capture 组件
的契约：client 的 jitter buffer 只负责网络抖动，不替采集端的停滞擦屁股。

每轮唤醒（事件或 20ms 超时）统一对账：

```text
expected  = 距上轮结算的墙钟欠账（含小数累积，防漂移）
balance  += expected - 本轮真实交付帧数
balance > 0 -> 欠账：立即合成静音补齐（空事件 / 零星小包 / 完全静默同一公式覆盖）
balance < 0 -> 盈余：engine 暴发，留存抵扣未来欠账（防止迟到真实数据与已补静音重复计时）
```

连续 2 轮补偿（约 40ms 欠账）才把诊断状态标成 `Starved`；第一轮欠账就生成 synthetic silence，
capture 时间轴不停。单轮补偿上限约 150ms（同时是盈余留存上限），防止系统从长时间挂起恢复后
瞬间制造巨大 burst；超出上限的欠账丢弃（不追历史）。

`GetBuffer` 的 `AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY`（engine 官方断流信号，切歌典型场景）
记录 debug 日志；对账模型按墙钟欠账已覆盖该窗口，无需特殊处理。

## 运行期错误

设备拔出、音频服务异常等通过 event_callback 通知 Runtime，使状态转为 Degraded；不在 backend event thread 里直接 `stop()`
，因为那可能 join 自己。
