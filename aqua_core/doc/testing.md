# Testing Strategy

## 1. 测试层级

### Unit

覆盖：

- AudioFormat
- AudioFrameQueue
- JitterBuffer
- NetworkFrame
- address parsing
- packetizer
- Diagnostics/logger

### Integration

覆盖：

- gRPC client/server
- UDP client/server
- runtime config
- WASAPI capture/playback/device manager
- dispatcher

### Regression

每次改变并发/生命周期/Buffer 时优先回归：

- JitterBuffer late write / slot reclaim
- reanchor boundary
- generation wakeup
- UDP stop/start ordering
- Runtime concurrent start/stop
- wildcard UDP advertisement fallback
- device selection and default format resolution

## 2. Runtime lifecycle matrix

必须验证：

```text
start() from Created
start() twice
stop() from Created
stop() twice
stop() while start() is executing
start() after Stopped -> false
```

Client / Server 都必须对称。

## 3. Device tests

Windows test machine 应验证：

- 至少一个 INPUT、一个 OUTPUT；
- default device 被正确标记；
- 指定 device id 可以 resolve；
- direction mismatch 被拒绝；
- loopback 使用 OUTPUT endpoint；
- 无 device id 使用 default endpoint；
- default_format() 返回可表示的 AudioFormat。

## 4. Format tests

验证：

- 显式格式三参数必须成组出现；
- backend default 不依赖硬编码 48 kHz/F32；
- 自动 F 不超过 1443 bytes；
- 显式 F < 16 被拒绝；
- 显式 F 超 payload 被拒绝；
- ConnectResponse 与实际 capture format/F 一致。

## 5. Full validation

Windows baseline 应达到：

```text
configure
build
ctest --output-on-failure
```

当前修改环境若缺少正确 MSVC/vcpkg/CMake 版本，只能报告静态检查与局部 smoke test，不得伪称全量通过。


## 默认配置回归

Core-level regression tests 固化 Server/Client 默认配置与资源边界；ClientRuntime configuration regression 单独加入 CTest，避免出现 tracked 但未编译的孤儿测试文件。必须有一个 Core-level regression test 固化以下 ServerRuntime baseline：`0.0.0.0:50051`、`0.0.0.0:9999`、advertised inherit/wildcard、OUTPUT loopback、无显式 device、无显式 AudioFormat、自动 frame count、默认 network queue。CLI 测试另外验证 Server 零参数解析、Server 仅 `--device-id` 可选择 OUTPUT loopback endpoint，以及 Client 只有 `--server-ip` 与 `--server-rpc` 两个必填连接参数。
