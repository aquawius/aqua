# Aqua JitterBuffer 设计

> 记录 aqua_core 客户端接收/回放路径上 **JitterBuffer（JB）** 的确定设计，作为实现依据。
> 本文件落实 `audio_design.md` 中留空的"§10 接收端回放管线"，并**取代**其中"JB 输出用 PlayoutSlot"的旧方案（旧方案已废弃，不落地）。
>
> 术语：**AudioFrame** = 网络/采集侧一个音频数据块（= 一个 slot）；**sample frame** = PCM 里"一个采样点 × 全部声道"（见 `audio_frame.h`）。`F` 的单位是 sample frame。

## 1. 范围与硬约束

1. 客户端接收/回放路径**只有一个缓冲**：JB 的环形 slot 存储。不引入 reorder buffer、pre-roll 队列、输出队列、PLC ring 或任何第二级播放缓冲。
2. slot 是 JB 的存储、排序与播放时间线单位；每个 slot 存一个完整 AudioFrame。
3. `F`（每 slot 的 sample frame 数）在一个 session 内**固定**：server 按网络打包配置（约 MTU 级 payload）重新打包成固定大小 AudioFrame，并通过 gRPC 控制面告知 client。
4. JB 需知道 `F` 与 `format`（`frame_bytes`），用于预分配与按 sample frame 读取。
5. `timestamp_ns` 不参与 JB 播放时序；播放节奏由 client 本地回放时钟（callback 节奏）驱动。
6. 水位控制基于 **slot**（`lead_slots`），不使用 bytes / 毫秒 / 独立 frame 水位；bytes 仅用于存储与诊断。
7. 回放后端 callback 的输出粒度由后端决定，与一个 slot 不要求对齐。
8. callback 只消费一个 slot 的一部分时，JB 保存剩余位置，下次从该位置继续。
9. playback timeline 的调整（Fill/Drop）由 consumer/callback 侧完成。
10. callback 路径禁止锁、堆分配、系统调用、阻塞。
11. PLC 本文件不实现；缺帧输出 `F` 个 sample frame 静音。未来 PLC 可在消费侧无侵入替换静音生成，不新增缓冲。

## 2. 核心模型与术语

| 术语 | 含义 |
|------|------|
| `N` | 槽数（环形容量），外部可配，默认 30。 |
| `F` | 每槽 sample frame 数（= AudioFrame::frame_count），session 内固定，来自 server。 |
| `frame_bytes` | 一个 sample frame 的字节数（= `AudioFormat::frame_bytes()`）。 |
| `play_seq` | 读前沿：下一个待消费的 sequence。启动前为哨兵值（见 §8）。 |
| `highest_seq` | producer 已写入的最大 sequence。 |
| `oldest_seq` | producer 已写入的最小 sequence（仅 startup 期间使用）。 |
| `lead_slots` | 播放领先量：`highest_seq − play_seq + 1`（见 §7）。 |
| `used_slots` | 物理占用：当前 `READY` 状态的槽数。 |
| `W` | 水位：`lead_slots / N`。 |

**不变式**：

1. `play_seq ≤ highest_seq + 1`。消费永不越过填充；等号表示"无数据可读"。
2. 每槽定长 `F × frame_bytes` 字节，server 保证、控制面下发，JB 据此预分配。
3. consumer 只读取 `state == READY` 且 `sequence == play_seq` 的槽；producer 只写 `EMPTY` 槽。二者由 slot 状态机保证，杜绝"边写边读"与"覆盖未消费数据"。

## 3. 存储与槽大小

- 槽：`{ state, sequence, data[F × frame_bytes] }`。`data` 为 JB 自有定长缓冲。
- `F` 来源：server 在 gRPC 建连时（`ConnectResponse`）下发 `frames_per_slot`，此后以固定大小发送。client 据此在 JB 构造时一次性预分配 `N × F × frame_bytes`。
- `AudioFrame::data` 是回调内有效的非拥有视图（`std::span`），push 时把 `data.size()` 字节拷贝进槽。
- server 保证帧大小固定；JB 仍做防御校验：`frame.frame_count == F && frame.data.size() == F × frame_bytes`，不符则 `push=false` 并记 `invalid_frame` 统计。

## 4. 环形索引与 sequence 规则

- 槽索引：`idx = sequence % N`。
- 每槽保存自己的 `sequence`，用于重复检测与陈旧数据防护。
- sequence 在单个 session 内**单调递增**；本设计**不要求 JB 支持 uint64 回绕**，session 生命周期内不得依赖回绕语义。
- "迟到/越界/重复"的判断见 §5；比较基于 uint64 单调不回绕这一前提。

## 5. Slot 状态机与生命周期

每个 slot 的 `state`：

```text
EMPTY → WRITING → READY → EMPTY
```

- **producer**：CAS `EMPTY→WRITING`（claim），写 `sequence` + `data`，再 `READY`（release）。
- **consumer**：acquire 读到 `READY` 后才读 `sequence` + `data`；完整消费后 `READY→EMPTY`（release）。
- `sequence`/`data` 是普通字段，其可见性由 `state` 的 acquire/release 建立；只要遵守状态机，无需各自原子。

**状态转换权责**：

| 转换 | 由谁 | 时机 |
|------|------|------|
| `EMPTY→WRITING` | producer | 写入前 claim |
| `WRITING→READY` | producer | 写完 sequence+data |
| `READY→EMPTY` | consumer | 完整消费（含缺帧静音结束后） |
| `READY→EMPTY` | producer | 迟到自回收（见 §6） |

## 6. Producer：`push`

`bool push(const AudioFrame& frame) noexcept`，`false` = 丢弃。

令 `s = frame.sequence`。

### 6.1 接纳条件

| 条件 | 判定 | 处理 |
|------|------|------|
| 帧大小不符（`frame_count ≠ F` 或字节数不符） | — | 丢弃 + 统计 |
| 启动后 `s < play_seq` | 迟到 | 丢弃 |
| 启动后 `s ≥ play_seq + N` | 越界（会覆盖未消费槽） | 丢弃 |
| 槽 `idx = s % N` 非 `EMPTY`（含 WRITING/READY） | 重复/冲突 | 丢弃 |
| 其余 | 合法 | 写入 |

- 启动前（`play_seq` 仍为哨兵）不执行"迟到/越界"检查：producer 只负责填充，靠"槽非 EMPTY 则拒"限制物理占用。
- "迟到"按 `s < play_seq`、重复按槽内 sequence 判断，不依赖 `highest_seq`，从而容忍窗内乱序。

### 6.2 写入顺序

```text
claim: CAS slot.state EMPTY → WRITING
write: slot.sequence = s; 拷贝 data
publish: slot.state = READY (release)
late-recheck: 若已启动且 s < play_seq → 自回收 slot READY→EMPTY (release)，返回 false
commit: highest_seq = max(highest_seq, s) (release)
        oldest_seq  = min(oldest_seq, s)  (release)
        used_slots += 1
return true
```

要点：

- `READY` 必须在 `highest_seq` 更新**之前**发布，保证 consumer 看到 `highest_seq ≥ s` 时，槽 `s` 已 READY（或确属缺失）。
- `late-recheck` 处理"写入期间 consumer 已越过该 sequence"的竞态窗口：帧已变迟，自行回收，不污染 `highest_seq`/`used_slots`。

## 7. 水位与分区

水位是**播放领先量**（sequence 距离），不是有效槽数——丢失的 sequence 仍占时间线一个槽位，故领先量只由序列距离决定。

```text
lead_slots = highest_seq − play_seq + 1
```

- 空态：`play_seq == highest_seq + 1` → `lead_slots = 0`。
- 满态：`highest_seq == play_seq + N − 1` → `lead_slots = N`（100%）。

```text
W = lead_slots / N
```

恢复目标：

```text
T = 0.60
T_slots = round(T × N)
```

分区（`lead_slots` 的整数阈值，构造时预计算；`N=30` 时约值见括号）：

| 区间 | 阈值（槽） | 水位 | 含义 |
|------|---:|---:|---|
| deadline low | `lead < WL`（≈9） | `[0.00, 0.30)` | 严重欠载 |
| warning low | `WL ≤ lead < NL`（≈14） | `[0.30, 0.45)` | 欠载 |
| normal | `NL ≤ lead ≤ NH`（≈23） | `[0.45, 0.75]` | 正常，不动作 |
| warning high | `NH < lead ≤ WH`（≈27） | `(0.75, 0.90]` | 过载 |
| deadline high | `lead > WH` | `(0.90, 1.00]` | 严重过载 |

- `WL/NL/NH/WH` = `round(0.30/0.45/0.75/0.90 × N)`；`T_slots = round(0.60 × N)`。
- 端点约定：`0.30`/`0.90` 归 warning，`0.45`/`0.75` 归 normal。取整差一槽不影响行为。

## 8. Startup 与 anchor

JB 创建后 `play_seq = UINT64_MAX`（哨兵 = 未启动）。startup 只看**播放领先量**，目标与运行时一致：

```text
lead = (oldest_seq ≤ highest_seq) ? highest_seq − oldest_seq + 1 : 0
```

- 未达到 `lead ≥ T_slots`（60%）时：producer 继续 push；consumer 每次 callback 输出全静音，不建立 anchor、不推进 `play_seq`。
- 首次满足 `lead ≥ T_slots` 时，consumer 在 callback 线程建立 anchor：

```text
play_seq = oldest_seq       // 最小已存在 sequence，而非"首个到达的 sequence"
read_offset = 0
current_slot_state = snapshot(play_seq)   // READY 或 MISSING
```

此后进入运行时水位控制，startup 规则不再生效。`play_seq` 由哨兵变为真实值这一写入即是对 producer 的"已启动"信号。

> 说明：anchor 取 `oldest_seq`（最小 sequence）而非"首帧 sequence"，避免首帧因轻微乱序不是最老时，把更早到达的帧误判为迟到丢弃。

## 9. 调整控制器

### 9.1 两个方向（必须区分）

- **高水位 Drop**：`play_seq += step`（跳过 `step` 个完整 slot），直接降 `W`，即时、确定。
- **低水位 Fill**：输出静音、`play_seq`/`read_offset` 不动，让 `highest_seq` 随网络继续增长而抬升 `W`，速度由网络决定。
- **缺帧静音**（§10.3）方向相反：缺帧时 `play_seq` 仍前进（时间流逝），会让 `W` 下降；Fill 是"停住序列、时间继续走"让 `W` 上升。二者不得混淆。

### 9.2 分区动作

- **deadline**：立即硬校正到 `T`。
  - deadline low：持续 Fill（静音、`play_seq` 不动）直到 `lead ≥ T_slots`。
  - deadline high：`SKIP(lead − T_slots)` 一步跳到 60%。
- **warning**：以有上限、递增的步长向 `T` 调整（§9.3）。
- **normal**：不动作。
- **恢复停止条件**：一旦进入恢复 episode，**持续到 `lead` 到达/越过 `T_slots`（60%）**，不是刚回到 normal 边界（45%/75%）。到达后回稳态。normal 宽区间只用于"是否从稳态开始调整"，故不会在边界来回震荡。

### 9.3 warning 递增步长（可插拔）

步长单位为 **slot**，两方向对称：

```cpp
struct WarningStepParams {
    std::uint32_t min_step = 1;   // 起始步长（槽）
    std::uint32_t max_step = 0;   // 0 = 自动：max(2, round(0.10 × N))
    double       growth   = 2.0;  // 每连续评估一次的倍率
};

// 返回本次步长（槽数）。Lower（高水位）= 跳过 step 槽；Raise（低水位）= 补 step×F 帧静音。
using WarningStepFn = std::uint32_t (*)(const WarningStepParams&, std::uint32_t k) noexcept;

// 默认：step = min(cap, base × growth^(k−1))
std::uint32_t default_warning_step(const WarningStepParams&, std::uint32_t k) noexcept;
```

- `N=30` 时 `max_step = 3`。
- `k` = 连续处于 warning 的评估次数（≥1）；进入 normal/deadline 或 episode 结束即归零。
- 重评估时机：每次 `pull` 开头一次。

### 9.4 控制器状态与 decide 伪代码

状态（consumer 线程独占，实时路径内只做无锁读写）：

```text
episode_dir         ∈ { None, Up, Down }
consecutive_warning : uint32 = 0
hold_remaining      : uint32 = 0   // 剩余静音 sample 帧数（warning 低的有界 Fill）
hold_until_target   : bool = false // deadline 低的持续 Fill
```

```text
decide(lead):                          # lead = highest_seq − play_seq + 1
  if episode_dir == Up:
      if lead >= T_slots:              # 已到 60%：结束
          episode_dir = None; consecutive_warning = 0; hold_remaining = 0; hold_until_target = false
          return NONE
      if hold_until_target:            # deadline 低：持续 Fill
          return HOLD
      if hold_remaining == 0:          # warning 低：上一段耗尽 → 递增续 Fill
          consecutive_warning += 1
          hold_remaining = step_fn(consecutive_warning) × F
      return HOLD

  if episode_dir == Down:
      if lead <= T_slots:
          episode_dir = None; consecutive_warning = 0
          return NONE
      consecutive_warning += 1
      return SKIP(step_fn(consecutive_warning))

  # —— 稳态 ——
  if lead < WL:                        # deadline 低
      episode_dir = Up; consecutive_warning = 0; hold_until_target = true
      return HOLD
  if lead < NL:                        # warning 低
      episode_dir = Up; consecutive_warning = 1
      hold_remaining = step_fn(1) × F
      return HOLD
  if lead <= NH:                       # normal
      consecutive_warning = 0
      return NONE
  if lead <= WH:                       # warning 高
      episode_dir = Down; consecutive_warning = 1
      return SKIP(step_fn(1))
  # deadline 高
  episode_dir = Down; consecutive_warning = 0
  return SKIP(lead − T_slots)
```

## 10. Consumer：`pull`

`JitterBufferPullResult pull(std::span<std::byte> output) noexcept`。

定义：

```text
K = format.frames_from_bytes(output.size())   // 后端本次要的 sample 帧数
```

consumer 维护 `read_offset ∈ [0, F]`（当前 slot 已向 callback 输出的 sample 帧数）与 `current_slot_state ∈ { READY, MISSING }`（进入该 slot 时对 `state` 的一次性快照，避免中途切换语义）。

### 10.1 读游标（部分消费）

一次 callback 可能只消费一个 slot 的前半段、也可能跨多个 slot。`read_offset` 保存剩余位置，**这不是第二个缓冲，只是槽内偏移**。server 与 client 后端 buffer 容量不同时，正是靠它适应 `K ≠ F`、`K` 非 `F` 整数倍的情况。

### 10.2 pull 伪代码

```text
snapshot(seq):
  idx = seq % N
  current_slot_state =
      (slots[idx].state == READY && slots[idx].sequence == seq) ? READY : MISSING

advance_slot():
  idx = play_seq % N
  if slots[idx].state == READY:        # 消费完（含"迟到写入"的 READY），回收并弃其数据
      slots[idx].state = EMPTY (release)
      used_slots -= 1
  play_seq += 1
  read_offset = 0
  snapshot(play_seq)

pull(output):
  K = frames_from_bytes(output.size()); frame_bytes = format.frame_bytes()
  result = { frames_filled=0, silence_frames=0, skipped_slots=0 }

  # 1) startup
  if play_seq == UINT64_MAX:                    # 未启动
      lead = (oldest_seq <= highest_seq) ? highest_seq - oldest_seq + 1 : 0
      if lead < T_slots:
          zero(output); return { K, K, 0 }
      play_seq = oldest_seq; read_offset = 0
      snapshot(play_seq)
      # fall through

  # 2) 控制决策
  lead = highest_seq - play_seq + 1
  action = decide(lead)

  # 3) Fill（低水位）
  if action == HOLD:
      zero(output)
      if !hold_until_target: hold_remaining = max(0, hold_remaining - K)
      return { K, K, 0 }

  # 4) Drop（高水位）
  if action == SKIP(step):
      repeat step 次: advance_slot()            # 被跳过的 READY 槽一并回收
      result.skipped_slots += step

  # 5) 正常消费
  filled = 0; silence = 0
  while filled < K:
      if play_seq > highest_seq:                # 耗尽：剩余静音，play_seq 不动
          zero(output[filled*frame_bytes .. K*frame_bytes])
          silence += K - filled; filled = K; break
      if current_slot_state == MISSING:
          n = min(F - read_offset, K - filled)
          zero n 帧; filled += n; silence += n; read_offset += n
      else:                                     # READY
          slot = slots[play_seq % N]
          n = min(F - read_offset, K - filled)
          copy slot.data[read_offset .. read_offset+n] → output[filled .. filled+n]
          filled += n; read_offset += n
      if read_offset == F:
          advance_slot()

  result.frames_filled = filled; result.silence_frames = silence
  return result
```

### 10.3 缺帧（丢包）

- 丢包只会丢整个 AudioFrame；槽非 `READY` 且 `play_seq ≤ highest_seq` 即缺帧。
- 缺帧输出**完整 `F` 个 sample frame 静音**，用 `read_offset` 跨 callback 保存进度：`read_offset` 从 0 走到 `F` 才 `advance_slot()`。因此一个缺失帧无论跨多少 callback，都严格占满一个 slot 的播放时间，不会提前进入下一 sequence。
- 与"耗尽"（`play_seq > highest_seq`，输出静音但**不**推进 `play_seq`）相区分。

### 10.4 要点

- `pull` 始终填满 `output`（真实数据 + 静音），返回 `frames_filled == K`；不阻塞、不加锁、不分配。
- 缺帧推进 `play_seq`，耗尽不推进（留给下次 `decide` 进低水位）。
- `HOLD` 期间 `hold_remaining` 按 `K` 扣减；`decide` 每次 `pull` 重评估 `lead`，到 `T_slots` 即停，最多多输出一拍静音（≤ 一次回调时长），可忽略。

## 11. 并发与内存序（SPSC）

- 生产者 = 网络线程（唯一 `push`）；消费者 = 回放实时线程（唯一 `pull`）。
- 原子字段：

```text
play_seq     atomic<uint64_t>    // consumer 写、producer 读（迟到/越界判断；哨兵=未启动）
highest_seq  atomic<uint64_t>    // producer 写、consumer 读
oldest_seq   atomic<uint64_t>    // producer 写、consumer 读（startup）
used_slots   atomic<uint32_t>    // producer ++ / consumer --（诊断）
slot.state   atomic（EMPTY/WRITING/READY）
```

- 发布/读取约定：
  - producer：写 `sequence`+`data` → `state=READY`(release) → `highest_seq/oldest_seq`(release) → `used_slots++`。
  - consumer：acquire `state`；仅当 READY 才读 `sequence`+`data`；acquire 读 `highest_seq`。
  - 槽数据可见性由各自 `state` 的 acquire/release 建立，**不依赖 `highest_seq`**（因为 producer 允许乱序写入）。
- 实时线程约束：`pull` 路径禁止锁、堆分配、系统调用；预分配、统计、日志一律在 producer/控制侧完成。

## 12. 配置

| 配置 | 默认 | 说明 |
|------|---:|---|
| `capacity_slots` | `30` | 环形槽数 `N`。 |
| `format` | 必填 | session 权威 `AudioFormat`。 |
| `frames_per_slot` | 必填（来自 server） | 每槽 sample frame 数 `F`。 |
| `target` | `0.60` | 恢复目标 / 稳态中心。 |
| `normal_low / normal_high` | `0.45 / 0.75` | normal 区。 |
| `warning_low / warning_high` | `0.30 / 0.90` | warning/deadline 分界。 |
| `step.min_step / max_step` | `1 / 0` | warning 步长（槽）；`max_step=0` = `max(2, round(0.10×N))`。 |
| `step.growth` | `2.0` | 连续 warning 评估的步长倍率。 |
| `step_fn` | 默认指数函数 | 可插拔，见 §9.3。 |

容量/延迟换算：`总时长 = N × F / sample_rate`，稳态时长 ≈ `总时长 × 60%`。

## 13. 公开接口草案（`include/aqua/audio/buffer/jitter_buffer.h`）

```cpp
namespace aqua::audio {

struct WarningStepParams {
    std::uint32_t min_step = 1;
    std::uint32_t max_step = 0;   // 0 = max(2, round(0.10×N))
    double       growth   = 2.0;
};

using WarningStepFn = std::uint32_t (*)(const WarningStepParams&, std::uint32_t k) noexcept;
std::uint32_t default_warning_step(const WarningStepParams&, std::uint32_t k) noexcept;

struct JitterBufferConfig {
    std::uint32_t capacity_slots = 30;
    AudioFormat   format;
    std::uint32_t frames_per_slot = 0;   // F，必填（来自 server）

    double target = 0.60;
    double normal_low = 0.45, normal_high = 0.75;
    double warning_low = 0.30, warning_high = 0.90;

    WarningStepParams step;
    WarningStepFn step_fn = &default_warning_step;
};

struct JitterBufferPullResult {
    std::uint32_t frames_filled = 0;   // 本次填充帧数（== 请求帧数）
    std::uint32_t silence_frames = 0;  // 其中静音帧数（缺帧 + Fill）
    std::uint32_t skipped_slots = 0;   // 本次跳过的槽数（Drop）
};

class JitterBuffer {
public:
    static std::expected<std::unique_ptr<JitterBuffer>, AudioError>
    create(const JitterBufferConfig& config);   // 校验；非法 → InvalidArgument

    bool push(const AudioFrame& frame) noexcept;                   // producer
    JitterBufferPullResult pull(std::span<std::byte> output) noexcept; // consumer（实时）

    std::uint32_t capacity_slots() const noexcept;
    std::uint32_t used_slots() const noexcept;
    std::size_t   capacity_bytes() const noexcept;   // == N × F × frame_bytes
    std::size_t   used_bytes() const noexcept;       // == used_slots × F × frame_bytes
    double        water_level() const noexcept;      // == lead_slots / N（诊断/测试）

    void reset() noexcept;   // 需两侧线程静止：回未启动态、全槽 EMPTY、复位指针与 episode
};

} // namespace aqua::audio
```

回放回调粘合层（示意）：

```cpp
std::uint32_t on_playback(void* ud, std::span<std::byte> output) noexcept {
    return static_cast<JitterBuffer*>(ud)->pull(output).frames_filled;
}
```

## 14. 测试矩阵（确定性，不依赖真实网络/设备）

基础存储：

1. 有序 push → 有序 pull 完整 PCM。
2. 首帧 `sequence` 非 0 → 正确锚定（anchor = 最小 sequence）。
3. 乱序 push `5,6` 后到 `4` → 输出 `4,5,6`。
4. 重复 sequence → 丢弃。
5. `s ≥ play_seq + N` 越界 → 丢弃。
6. 环形复用 → 不读到陈旧 sequence。
7. 非法 `frame_count ≠ F` → `push=false`。
8. `used_slots/capacity_slots/used_bytes/capacity_bytes/water_level` 始终正确；部分消费未完成前当前槽仍计 `used_slots`。

Startup：

9. 未到 60% → 全静音、不推进、不建立 anchor。
10. 到达 60% → 建立 anchor（取最小 sequence）并开始播放。
11. startup 期间网络持续 push 不受 consumer 影响。

水位：

12. 遍历整数 `lead_slots` 验证 `30/45/60/75/90%` 边界。
13. normal 不动作。
14. warning low 进入 Fill，最终回 60%。
15. warning high 进入 Drop，最终回 60%。
16. deadline low 持续 Fill 到 60%。
17. deadline high 一次 Drop 到 60% 附近。
18. 恢复不在 45/75% 边界震荡。

Callback 与 slot 边界：

19. `output` < slot → 下次从 `read_offset` 续读。
20. `output` > slot → 跨多槽不丢不重。
21. `output` 恰为多个 slot → 无丢失、无重复。
22. 部分消费后普通 callback → 剩余正确保留。
23. server/client callback 粒度不同 → 连续正确。
24. 缺帧跨 callback → 恰好 `F` 帧静音后才进入下一 sequence。
25. 耗尽 → 静音且不推进 `play_seq`。

并发：

26. 乱序写入时 consumer 不读 WRITING 槽。
27. `READY` publish / `EMPTY` recycle 的 acquire/release 正确。
28. producer 不覆盖 consumer 正在读的槽。
29. 诊断读取不引入 callback 锁。

## 15. 与 Server / 控制面的契约

- server 在 session 建立时通过 `ConnectResponse` 下发 `frames_per_slot = F`，来源是 server 音频打包配置；`server AudioFrame 大小 == client JB slot 大小`，session 内固定。
- server 与 client 各自的 capture/playback backend buffer 容量无需相同；它们只决定一次 callback 的 `K`，JB 用 `read_offset` 处理 `K ≠ F`。
- 该 `frames_per_slot` 字段需在实现 JB 前落进 `aqua_service.proto` 并透传为 `JitterBufferConfig::frames_per_slot`。
