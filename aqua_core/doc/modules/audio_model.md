# 模块：Audio Model

## 文件

- `aqua_core/include/aqua/audio/audio_format.h`
- `aqua_core/include/aqua/audio/audio_block.h`
- `aqua_core/include/aqua/audio/audio_frame.h`
- `aqua_core/include/aqua/audio/audio_error.h`
- `aqua_core/include/aqua/audio/audio_switch_result.h`

## 设计职责

这一层只定义**音频字节的含义**与**跨层错误词汇**，不关心 UDP、JitterBuffer、设备或线程。

## AudioFormat

`AudioFormat` 只描述 PCM 本身：encoding、channels、sample_rate。合法性由 `is_valid()` 判定：

```text
bytes_per_sample > 0
1 <= channels <= 64          (AUDIO_FORMAT_MAX_CHANNELS)
1 <= sample_rate <= 768000   (AUDIO_FORMAT_MAX_SAMPLE_RATE)
channels × bytes_per_sample 不溢出 uint32
```

位深映射：`PCM_U8=1`、`PCM_S16LE=2`、`PCM_S24LE=3`、`PCM_S32LE=4`、`PCM_F32LE=4`（字节/单声道采样）。

`frame_bytes()` 是所有上下游几何计算的唯一来源：

```text
frame_bytes = channels × bytes_per_sample
slot_bytes  = frame_count × frame_bytes
```

所有乘法都做溢出检查，溢出返回 0。上层创建缓冲区、校验 UDP payload、计算 packetizer pending size，都必须从这里推导，
禁止另写一套"位深 → 字节"的手算逻辑。

`frame_count_for_budget(format, budget)` 由字节预算反推每帧 sample frame 数（向下取整），Server 用它从 MTU 预算推导 F。

## AudioFrame

`AudioFrame` 是**定长**帧视图（`sequence` + `frame_count` + `data` 借用视图），不拥有数据。`data` 在以下时机有效：

- Packetizer sink 回调期间指向 packetizer 的 pending buffer；
- UDP 解码后指向接收缓冲区；
- JitterBuffer `push()` 期间作为输入视图。

因此接收方若需跨回调保存必须复制。JitterBuffer 会把 payload 拷进自己预分配的 slot。

## AudioBlock

`AudioBlock` 是 backend 一次回调的**变长** PCM 视图。它的存在使 backend 的回调粒度不必等于网络 slot 粒度——Packetizer 负责
重切成定长帧。

## AudioError

平台层细节（HRESULT、AAudio result）只记日志，跨层传递用类别化的 `AudioError`：

| 枚举                 | 含义                                       |
|----------------------|--------------------------------------------|
| `None`               | 成功                                       |
| `DeviceNotFound`     | 启动时指定设备不存在                       |
| `DeviceUnavailable`  | 设备存在但当前打不开（被占用 / 暂不可用）  |
| `DeviceDisconnected` | 运行过程中设备消失或失效                   |
| `FormatUnsupported`  | 请求的 AudioFormat 后端不支持              |
| `NotSupported`       | 平台/后端不支持该模式（如 Android loopback）|
| `PermissionDenied`   | 音频权限被拒                               |
| `AlreadyRunning`     | 已运行时再次 `start()`                     |
| `NotRunning`         | 未运行时执行了依赖运行状态的操作           |
| `InvalidArgument`    | 配置非法                                   |
| `BackendFailed`      | 平台层失败（原因见日志）                   |

只有 `DeviceDisconnected`（client 侧还包括 `DeviceUnavailable` / `DeviceNotFound`）会触发设备切换事务；其余错误按终止条件
处理（见 `flow_model.md` §4）。

## 流状态与管理状态

音频域有两级状态，互不混淆：

- **流级** `AudioCaptureState`（`Active` / `Silent` / `Starved`）：描述采集时间轴是否在推进，由 backend 维护。
- **管理级** `CaptureSwitchState` / `PlaybackState`（`Inactive` / `Starting` / `Running` / `Switching` / `Fatal`）：描述切换
  事务，由 `CaptureManager` / `PlaybackManager` 维护。

## SwitchResult

两侧切换事务共用同一套结果词汇（`audio_switch_result.h`）：

```text
None             尚未发生切换
Switched         目标设备一次成功
RolledBack       目标失败，回滚到先前的实际设备
FellBackToSystem 目标与回滚都失败，落到系统默认
Fatal            候选链耗尽（格式不兼容 / 重试超限）
```
