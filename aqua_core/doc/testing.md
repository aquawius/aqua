# 测试与回归

## 1. 测试原则

测试目标不是只验证“能播”，而是验证边界契约：序号、容量、并发发布、停止顺序、格式拒绝、网络异常、回调生命周期。

## 2. Audio model

覆盖：

- `AudioFormat` 合法性与 overflow
- frame/byte 对齐
- `AudioFrame` well-formed

## 3. Packetizer / Queue

必须验证：

- 变长 block 跨多个 slot；
- pending 半帧保留到下一次 push；
- unaligned block 被拒绝；
- sequence 连续；
- queue 满时 drop newest；
- consumer callback 返回后才 release slot；
- wake hint 不是队列状态本身。

## 4. JitterBuffer

重点回归：

- pre-roll 恰好 startup_level 边界；
- 未启动远跳与 reanchor；
- 运行中远跳；
- late frame；
- slot collision；
- duplicate sequence；
- 缺帧静音；
- output 跨 slot；
- Fill warning 慢放校正（重播 READY slot）与 target 终止；
- Drop step 增长；
- deadline-high；
- reanchor 后陈旧 Ready 清理；
- `advance_slot()` 的先推进 play_seq 再回收 slot；
- reanchor hold-stuck 5-pull fallback；
- sanity jump rejection。

## 5. UDP / Session

覆盖 malformed datagram、wrong type、payload size mismatch、wrong session、unexpected sender、HELLO establish/refresh、timeout
reap、disconnect idempotence。

## 6. Runtime

覆盖：

- 非法配置拒绝；
- backend 缺失；
- Connect 无效 response；
- stream geometry overflow；
- payload 超 MTU；
- UDP 启动失败；
- HELLO 启动失败；
- playback start 失败；
- stop 幂等；
- async callback 晚到时 CallbackGate 不 use-after-free；
- Server reaper stop 不留下 timer work。

## 7. 设备切换测试

切换事务用 mock 后端 + mock 设备管理器覆盖（`tests/audio/*_manager_test.cpp`），不依赖真实音频设备：

- 候选链：一次成功（`Switched`）、回滚（`RolledBack`）、落系统默认（`FellBackToSystem`）、链耗尽（`Fatal`）；
- `Fatal` 是终态：后续 restart 被拒且不触碰后端；
- 重试预算：窗口内前 3 次成功，第 4 次直接 `Fatal`（`start_attempts` 不增长）；
- 格式钉死：候选收到的请求格式等于会话格式；backend 违约（info 不符）时候选按 `FormatUnsupported` 处理；
- 回调活跃期 restart：无死锁、无双重生产/消费（`max_concurrent_callbacks == 1`）；
- 共享预算：错误驱动与默认跟随的 restart 合并计数。

时间线连续性（restart 前后 seq 单调、session 不重建）由实现结构保证：切换事务只调用管理器自身方法，packetizer / network /
session 不在其可达范围——属于代码评审项而非断言项。

## 8. 平台测试

WASAPI 测试要与 domain test 分开看：domain tests 验证纯 Core 语义，WASAPI tests 验证 COM、event、buffer/padding、设备错误和真实
callback 生命周期。

新增 AAudio 时，优先复用 domain test 集，不要把协议/缓冲测试复制成 Android 专用版本；Android 专用测试只覆盖 AAudio adapter
和 JNI ABI。

## 9. 测试目标与运行

测试不是单一可执行，而是按模块拆成的多个 gtest 目标：

| 目标                             | 内容                                  | 平台       |
|----------------------------------|---------------------------------------|------------|
| `aqua_tests`                     | logger                                | 全         |
| `aqua_diagnostics_tests`         | diagnostics                           | 全         |
| `aqua_net_tests`                 | gRPC / session / UDP / 格式转换        | 全         |
| `aqua_audio_tests`               | AudioFormat / AudioFrameQueue          | 全         |
| `aqua_audio_packetizer_tests`    | packetizer                            | 全         |
| `aqua_jitter_buffer_tests`       | JitterBuffer（含边界与回归）           | 全         |
| `aqua_playback_manager_tests`    | PlaybackManager 切换事务               | 全         |
| `aqua_capture_manager_tests`     | CaptureManager 切换事务               | 全         |
| `aqua_capi_test`                 | C API（需 `AQUA_BUILD_C_API=ON`）      | 全         |
| `aqua_wasapi_device_manager_tests`| WASAPI 设备解析                       | 仅 Windows |
| `aqua_wasapi_capture_tests`      | WASAPI 采集                            | 仅 Windows |
| `aqua_wasapi_playback_tests`     | WASAPI 回放                            | 仅 Windows |

全部经 `gtest_discover_tests` 注册，因此按用例粒度跑：

```bash
ctest --preset windows-x64-debug          # 全量
ctest --preset windows-x64-debug -R CaptureManager   # 按名字过滤
ctest --test-dir cmake_build/windows-x64-debug -C Debug --output-on-failure
```

test preset 只有 Windows / Linux / macOS 六份，Android 没有（Android 只构建 `aqua_capi`，测试跑在主机侧）。
完整构建与 preset 说明见 `build_and_release.md` 与仓库根 `BUILD.md`。
