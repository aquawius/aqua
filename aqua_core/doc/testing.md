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

### 4.1 控制律离线回放（`jitter_control_replay_test.cpp`）

**改 target 控制参数前先跑它**，不要直接上现场。它把合成链路喂进**真实的**
`JitterEstimator` + `TargetController` + `JitterBuffer`（不是重实现：公式、限速、风暴、
带几何全部来自生产代码，带几何经 `apply_adaptive_bands()` 与 ClientRuntime 同源），
虚拟时钟 + 固定 seed + 扫 16 个相位取最坏 → 确定性、无 sleep。

看场景表：

```bash
aqua_jitter_buffer_tests --gtest_filter=*ScenarioTable*
```

覆盖：干净链路 / 1ms / 3ms 抖动 / 1% 丢包 / **1% 成串丢包**（Gilbert-Elliott，平均连丢 6 包）/
35% 断流风暴 / **hold 风暴**（250ms 黑洞 + 回补 burst，delay-spike regime）/
**2s 完全断流**。每行给出 target 的 min/max/mean、静音占比、最长连续断流、
FILL/DROP episode、P99，以及决策层的路径占比（rise/fall/dwell/storm）与切换次数。

已钉住的不变量：

- target 永远落在 [几何地板, 2/3 容量]；
- 干净链路 target 贴地板（零欠载下限验证）；
- 丢包走 concealment 而非裸静音，**成串丢包**同样守预算；
- **splice 只改输出样本**：开/关 splice 时逐包 target 与路径序列必须逐位相同，
  输出只在 ≤2× 淡变长度的短窗内不同（实测 56 段 / 最大 63 帧 / 占 0.61% 样本）；
- 风暴期进 storm hold；
- **断流后残余被 stall 封顶挡住、且不锁死**（见下）。

### 4.2 断流后的残余水位（已知取舍，实测数据）

2s 完全断流的实测（最坏相位）：

```text
ref end:  target=7  tail_margin=2.92  stall_margin=0.00  eff_min=4
run end:  target=8  tail_margin=2.92  stall_margin=8.00  penalty=0.00
          stall_peak=1770ms -> 回落到封顶以下约需 174s
```

即：残余**来自 stall 峰值项**（不是欠载罚分——罚分上限 6 槽、0.5 槽/s，12s 内必清零），
幅度被 `JB_ADAPTIVE_STALL_PEAK_CAP_SLOTS` 挡住（8 槽），但**持续时间与事故时长成正比**：
峰值按 `JB_ESTIMATOR_STALL_PEAK_DECAY_MS_PER_SEC`（10ms/s）线性衰减，2s 的峰值约 174s
才回落到封顶以下。相对链路正常稳态的增量是 **+1 槽（3.6ms）**。

这是设计取舍（`buffer_config.h` 该常量注释即写"大事故挂更久"），不是缺陷；
`OutageResidueIsCappedNotLocked` 把"封顶生效 + 不锁死"钉成断言。若将来要把
"事故后回落时间"纳入契约，需要单独决策（例如给 stall 峰值加时间上限，或对
超过阈值的峰值改用别的衰减律）。

### 4.3 同文件其他套件（一句话索引）

- `StormCapacitySweep`：容量 30/60/120 × 风暴四档。结论：纯丢失型风暴加容量逐字无效；
  hold 风暴下 cap120 让 churn 归零且 target 不涨（地板驱动）。
- `PresetLadderSweep`：min-target 3/6/9/12 × 四档链路。结论：min 6 以 +7ms 杀掉干净链路的
  DROP 抖动；丢包链上抬 min 有害（走 penalty 路线）；hold 风暴看容量不看 min。
- `HoldBurstPreservesOrder` / `HoldStormReproducesBurstPathology`：hold 模型保序回归
  （串行化释放）与病理签名（busy/reanchor 非零，区别于 loss 模型）。

### 4.4 真 trace 回放（`jitter_trace_replay_test.cpp`）

`AQUA_JB_TRACE=/path/to/client.log` 指向 `--jb-trace` 语料（JBT 行新旧格式都认），
同一份到达节奏跑 16 相位：`ReplayIsDeterministic`（同输入同输出）与
`SameTraceBoundsHold`（target 永不出 [floor, max] + 逐相位表）。未设置则跳过，
CI 常绿。拿现场问题做回归时，先把 trace 留下来，再谈复现。

## 5. UDP / Session

覆盖 malformed datagram、wrong type、payload size mismatch、wrong session、unexpected sender、heartbeat
establish/refresh、timeout reap、disconnect idempotence。

## 6. Runtime

覆盖：

- 非法配置拒绝；
- backend 缺失；
- Connect 无效 response；
- stream geometry overflow；
- payload 超 MTU；
- UDP 启动失败；
- heartbeat 启动失败；
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

| 目标                               | 内容                              | 平台       |
|------------------------------------|-----------------------------------|------------|
| `aqua_tests`                       | logger                            | 全         |
| `aqua_diagnostics_tests`           | diagnostics                       | 全         |
| `aqua_net_tests`                   | gRPC / session / UDP / 格式转换   | 全         |
| `aqua_audio_tests`                 | AudioFormat / AudioFrameQueue     | 全         |
| `aqua_audio_packetizer_tests`      | packetizer                        | 全         |
| `aqua_jitter_buffer_tests`         | JitterBuffer（含边界与回归）      | 全         |
| `aqua_playback_manager_tests`      | PlaybackManager 切换事务          | 全         |
| `aqua_capture_manager_tests`       | CaptureManager 切换事务           | 全         |
| `aqua_capi_test`                   | C API（需 `AQUA_BUILD_C_API=ON`） | 全         |
| `aqua_wasapi_device_manager_tests` | WASAPI 设备解析                   | 仅 Windows |
| `aqua_wasapi_capture_tests`        | WASAPI 采集                       | 仅 Windows |
| `aqua_wasapi_playback_tests`       | WASAPI 回放                       | 仅 Windows |

全部经 `gtest_discover_tests` 注册，因此按用例粒度跑：

```bash
ctest --preset windows-x64-debug          # 全量
ctest --preset windows-x64-debug -R CaptureManager   # 按名字过滤
ctest --test-dir cmake_build/windows-x64-debug -C Debug --output-on-failure
```

test preset 只有 Windows / Linux / macOS 六份，Android 没有（Android 只构建 `aqua_capi`，测试跑在主机侧）。 完整构建与 preset
说明见 `build_and_release.md` 与仓库根 `BUILD.md`。
