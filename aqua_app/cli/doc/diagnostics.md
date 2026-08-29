# CLI 诊断

## Server 每 1 秒 Debug snapshot

核心 source：

```text
state
 audio
 queue
 sessions
 udp
```

counter 覆盖 capture、packetizer、queue、dispatcher、UDP、session。

## Client 每 1 秒 Debug snapshot

核心 source/counter 覆盖：

```text
state
 grpc result
 udp stats + hello liveness
 jitter water/used/reanchor
 jitter push/pull/fill/drop
 playback pull
```

## 控制轮询

Server/Client 还有 500ms control poll。它不是音频控制器，只负责观察 runtime 是否进入 `Degraded` 等 terminal condition。

因此“500ms poll”不能解释成“500ms 一次播放调整”。JitterBuffer 完全由 playback callback 驱动。
