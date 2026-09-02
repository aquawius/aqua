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

## 7. 平台测试

WASAPI 测试要与 domain test 分开看：domain tests 验证纯 Core 语义，WASAPI tests 验证 COM、event、buffer/padding、设备错误和真实
callback 生命周期。

新增 AAudio 时，优先复用 domain test 集，不要把协议/缓冲测试复制成 Android 专用版本；Android 专用测试只覆盖 AAudio adapter
和 JNI ABI。
