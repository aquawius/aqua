# 音频设计

## 1. 音频单位

Aqua 代码中有三种不同对象：

- `AudioBlock`：采集 backend 一次 callback 提供的**变长 PCM 字节块**。
- `AudioFrame`：Packetizer 将 block 重切成的**定长网络/缓冲 slot**，包含 `sequence + frame_count + data`。
- `sample frame`：某一时间点所有声道的一组采样。`frame_bytes = bytes_per_sample × channels`。

不要把 `sample frame`、`AudioFrame slot`、UDP packet 混用。

## 2. AudioFormat

支持：

| encoding | bytes/sample |
|---|---:|
| PCM_S16LE | 2 |
| PCM_S24LE | 3 |
| PCM_S32LE | 4 |
| PCM_F32LE | 4 |
| PCM_U8 | 1 |

合法范围：`channels` 1..64，`sample_rate` 1..768000。实际产品配置通常远小于上限，但 Core 类型本身保留更宽范围。

`AudioFormat` 只描述 PCM 数据，不描述 latency、packet size、buffer size、设备或协议。

## 3. Server 格式决策

Server 启动时：

1. 若 CLI 指定 `--encoding/--channels/--sample-rate` 三项，则构造显式格式。
2. 三项必须成组出现，不能只指定其中一部分。
3. 未指定时，使用 capture backend 的 shared-mode/default format。
4. 格式确定后计算 `frame_count`。
5. `frame_count` 也成为 ConnectResponse 的 session-wide 固定参数。

Server 不做运行期格式切换；设备/格式修改需要停机重新启动。

## 4. Client 格式决策

Client 不自行决定播放格式。Connect 成功后使用 Server 返回的格式作为 playback 配置：

```text
server format
    ↓
JitterBuffer format
    ↓
AudioPlaybackConfig.format
```

Playback backend 若无法原生支持该格式，启动失败，错误应为 `FormatUnsupported` 或对应平台错误；Core 不隐式插入格式转换器。

## 5. MTU 与 F

UDP Audio packet 的安全 payload budget 为 1443 字节：

```text
1500 - 40 IPv6 header - 8 UDP header - 9 Aqua audio header = 1443
```

显式 `F` 必须满足：

```text
F >= 16
F × frame_bytes <= 1443
```

`F=0` 表示自动按 payload budget 向下取整。

这意味着 `F` 不是“延迟毫秒”配置，而是一次 AudioFrame 包含多少 sample frame。实际网络发送周期由 `F / sample_rate` 决定。

## 6. Capture / Playback 对立模型

Capture 是 push：OS 调用 callback，应用拿到数据。

Playback 是 pull：OS 需要数据时调用 callback，应用填充输出。

这一对抽象使 WASAPI 和未来 AAudio 可以共享同一 Runtime 数据路径，而平台线程只负责适配 OS callback。

## 7. 音频错误

`AudioError` 只表达上层可处理类别：设备不存在、不可用、断开、格式不支持、权限、参数非法、backend 失败等。HRESULT、AAudio result 等平台细节写入日志，不泄漏进跨平台业务接口。
