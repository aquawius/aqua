# Aqua JitterBuffer 设计

> 记录 aqua_core 客户端接收/回放路径上 **JitterBuffer（JB）** 的已确定设计，作为实现的依据。
> 本文件落实 `audio_design.md` 中留空的"§10 接收端回放管线"，并**取代**其中"JB 输出用 PlayoutSlot"的旧方案（旧方案已废弃，不落地）。
>
> 术语说明：本文件里 **AudioFrame** 指网络/采集侧的一个音频数据块（= 一个 slot）；**sample frame** 指 PCM 里"一个采样点 × 全部声道"（见 `audio_frame.h`）。`F` 的单位是 sample frame。

## 1. 范围与硬约束

- 客户端接收/回放路径上**只有这一个缓冲**：JB 的环形缓冲。硬要求，不得再引入任何其他缓冲（无 reorder buffer、无 pre-roll 队列、无输出队列、无 PLC ring）。
- JB 内部的"读游标"（`read_offset`）是**偏移量，不是缓冲**。
- server 侧"采集 → 攒成固定大小 AudioFrame → 交给网络层发送"里的打包缓冲属于 server 侧职责，不在本文件范围。
- PLC（丢包隐藏）本文件**不引入**；缺帧一律输出 `F` 帧静音（见 §8.3）。JB 的 `pull` 返回结果保留静音统计，未来 PLC 可作为消费侧的后处理**无侵入**接入，不新增缓冲。

## 2. 核心模型与术语

| 术语 | 含义 |
|------|------|
| 槽（slot） | JB 环形缓冲的一个单元，恰好存放一个 AudioFrame 的完整 PCM。 |
| 序列（sequence） | `AudioFrame::sequence`，音频层单调序号。JB 唯一排序依据。 |
| `timestamp_ns` | **保留字段，JB 不使用**。播放节奏由回放后端回调时钟驱动。 |
| `play_seq` | 读前沿：下一个待消费的 sequence。 |
| `highest_seq` | 写前沿：已写入的最大 sequence。 |
| `N` | 槽数（环形容量），外部可配，默认 30。 |
| `F` | 每槽的 sample frame 数（= 一个 AudioFrame 的 `frame_count`）。**固定值**，由 server 在 gRPC 建连时下发。 |
| 水位 `W` | `(highest_seq − play_seq) / N`，见 §6。 |

**不变式**：

1. `play_seq ≤ highest_seq + 1`。消费永不越过填充；`play_seq = highest_seq + 1` 表示"当前无数据可读"。
2. 每槽大小固定为 `F × frame_bytes` 字节，由 server 保证（固定帧大小发送）并经控制面下发；JB 按此预分配。
3. 每个槽在写入后、被消费前只被同一 sequence 占据一次（环形一周）。

## 3. 存储与槽大小

- 槽存储：`{ sequence, valid, data }`，其中 `data` 是 JB 自有定长缓冲，大小 `F × frame_bytes`。
- `F` 的来源：**server 在 gRPC 建连时（`ConnectResponse`）下发 `frames_per_slot`**；server 此后以固定大小发送 AudioFrame。client 据此在 JB 构造时一次性预分配 `N × F × frame_bytes`。
- `AudioFrame::data` 是回调内有效的非拥有视图（`std::span`），push 时把 `data.size()` 字节**拷贝**进槽。
- server 保证帧大小固定，JB **信任**该值，不做跨帧大小校验；可保留 debug 断言 `frame.frame_count == F`。

## 4. 环形索引与 sequence 锚定

- 槽索引：`idx = sequence % N`。
- **锚定**：首个有效帧的 sequence 记为 `S0`，同时令 `play_seq = highest_seq = S0`。此后规则都相对 `play_seq` 展开，sequence 不必从 0 开始。
- 每槽记录自己的 `sequence`，用于重复检测与回绕正确性（防止读到上一圈的陈旧槽）。

## 5. 填充侧（producer / 网络线程）：`push`

`bool push(const AudioFrame& frame) noexcept`，返回是否接受（false = 丢弃）。

对 sequence `s` 的接纳规则：

| 条件 | 判定 | 处理 |
|------|------|------|
| 首帧 | 无锚定 | 锚定 `play_seq=highest_seq=s`，拷贝入槽 |
| `s < play_seq` | 迟到（已消费/已越过） | 丢弃 |
| `s ≥ play_seq + N` | 越界（环形满，会覆盖未消费槽） | 丢弃（罕见，主动调整应已避免） |
| 槽 `idx = s % N` 已 `valid` 且 `sequence == s` | 重复 | 丢弃 |
| 其余 | 窗内（含轻度乱序） | 拷贝入槽，置 `valid`，`highest_seq = max(highest_seq, s)` |

要点：

- JB **不要求**网络侧保证到达顺序：窗内乱序帧会落到各自正确的 `idx`，在 `play_seq` 走到之前按序播放。
- "迟到"与"重复"按 `s < play_seq` 与槽内 sequence 判断，不依赖 `highest_seq`（避免轻微乱序被误判为迟到）。
- 首个 AudioFrame 到达前，消费侧按 §7/§8 输出静音（天然 pre-roll），与 push 无竞态（槽的写入都在 producer 侧完成，且以 release 发布 `highest_seq` 后才对 consumer 可见）。

## 6. 水位与分区

水位是**序列距离**（时间领先量），不是"有效槽数"——丢失的 sequence 在时间线上仍占一个槽位（播放时输出静音），因此领先量只由 `highest_seq − play_seq` 决定，与中间是否有洞无关。

- `d = highest_seq − play_seq`（槽数；空缓冲时按 0 处理）。
- `W = d / N`，取值 `[0, 1]`。
- 恢复目标 `T = 0.60`（正常区中心）。

分区（半开区间，`W` 计）：

| 区间 | 范围 | 含义 |
|------|------|------|
| deadline 低 | `[0.00, 0.30)` | 严重欠载 |
| warning 低 | `[0.30, 0.45)` | 欠载 |
| normal | `[0.45, 0.75]` | 正常（不动作） |
| warning 高 | `(0.75, 0.90]` | 过载 |
| deadline 高 | `(0.90, 1.00]` | 严重过载 |

- 端点约定：`0.30` 归 warning 低、`0.90` 归 warning 高；`0.45` 与 `0.75` 归 normal。
- 实现可直接用 `d` 与 `N` 的浮点比值 `d/N` 比较，或预计算整数阈值；两者等价，边界差一个整槽不影响行为。

## 7. 调整控制器（核心）

### 7.1 两个方向的调整语义（必须区分）

- **高水位（跳过）**：`play_seq += step`（丢弃 `step` 个槽），**直接**降低 `W`，即时、确定性。
- **低水位（补空白）**：输出静音且 `play_seq` **不动**，让网络继续填充、`highest_seq` 追上来，**间接**抬高 `W`。抬高速度由网络速率决定，非本地可控。
- **缺帧静音**（§8.3）与"低水位补空白"方向相反：缺帧时 `play_seq` 仍 `+1`（时间流逝、跳过丢失槽），会让 `W` 下降；而补空白是"停住序列、时间继续走"，让 `W` 上升。两者不得混淆。

### 7.2 分区动作

- **deadline**：立即、一次性把 `W` 硬校正到 `T`。
  - deadline 低：持续输出静音（`play_seq` 不动）直到 `W ≥ 60%`。
  - deadline 高：`skip` 步长 `d − T` 槽（一步跳到 60%）。
- **warning**：以**有上限、递增**的步长向 `T` 调整（见 §7.3）。
- **normal**：不动作。
- **恢复停止条件**：一旦开始调整（进入恢复 episode），**持续调整直到 `W` 到达/越过 `T`（60%）**，而不是只回到 normal 边界（45%/75%）。到达 `T` 后回到稳态。normal 的宽区间 `[45%, 75%]` 只用于"是否从稳态**开始**调整"的判断（吸收小抖动、避免频繁触发）；恢复的终点固定是 60%，因此不会在 45%/75% 边界来回震荡。

### 7.3 warning 递增步长（可插拔）

步长单位按方向区分：**跳过（高水位）= 槽；补空白（低水位）= sample 帧**。这避免把"补多少静音"绑死在每槽帧数上。

```cpp
enum class AdjustDirection { Raise, Lower };  // Raise = 低水位(补空白)；Lower = 高水位(跳过)

struct WarningStepParams {
    std::uint32_t skip_min = 1;    // 高水位起始步长（槽）
    std::uint32_t skip_max = 0;    // 0 = 自动：max(2, round(0.10 * N))
    std::uint32_t hold_min = 160;  // 低水位起始步长（sample 帧；≈3.3ms @48k）
    std::uint32_t hold_max = 0;    // 0 = 自动：8 × hold_min
    double       growth   = 2.0;   // 每连续评估一次的倍率
};

// 返回本次步长：dir == Lower → 槽数；dir == Raise → sample 帧数。
// k = 连续处于 warning 的评估次数（≥1）。
using WarningStepFn = std::uint32_t (*)(const WarningStepParams&, AdjustDirection dir, std::uint32_t k) noexcept;

// 默认实现：step = min(cap, base × growth^(k−1))；cap/base 按方向取 skip_*/hold_*。
std::uint32_t default_warning_step(const WarningStepParams&, AdjustDirection dir, std::uint32_t k) noexcept;
```

- `N=30` 时 `skip_max = 3`。
- 重评估时机：**每次 `pull` 开头一次**（见 §8）。

### 7.4 控制器状态与伪代码

状态（全部位于 consumer 线程，实时线程内只做无锁读写）：

```
episode_dir       ∈ { None, Up, Down }   // 当前恢复方向；None = 稳态
consecutive_warning : uint32 = 0         // 连续 warning 评估次数
hold_remaining    : uint32 = 0           // 剩余待输出静音帧数（warning 低的有界 hold）
hold_until_target : bool = false         // deadline 低的持续 hold（到 60% 为止）
```

```
decide(d):                       // d = highest_seq − play_seq，空时按 0
  if episode_dir == Up:
      if d >= T:                 // 已到 60%：结束恢复
          episode_dir = None; consecutive_warning = 0; hold_remaining = 0; hold_until_target = false
          return NONE
      if hold_until_target:      // deadline 低：持续静音直到 60%
          return HOLD
      if hold_remaining == 0:    // warning 低：上一段 hold 耗尽仍未到目标 → 递增续 hold
          consecutive_warning += 1
          hold_remaining = step_fn(Raise, consecutive_warning)   // 帧
      return HOLD

  if episode_dir == Down:
      if d <= T:
          episode_dir = None; consecutive_warning = 0
          return NONE
      consecutive_warning += 1
      return SKIP(step_fn(Lower, consecutive_warning))           // 槽

  // —— 稳态：按分区决定是否开始 ——
  if d < WL:                     // deadline 低
      episode_dir = Up; consecutive_warning = 0; hold_until_target = true
      return HOLD
  if d < NL:                     // warning 低
      episode_dir = Up; consecutive_warning = 1
      hold_remaining = step_fn(Raise, 1)
      return HOLD
  if d <= NH:                    // normal
      consecutive_warning = 0
      return NONE
  if d <= WH:                    // warning 高
      episode_dir = Down; consecutive_warning = 1
      return SKIP(step_fn(Lower, 1))
  // deadline 高
  episode_dir = Down; consecutive_warning = 0
  return SKIP(d − T)
```

其中 `WL/NL/NH/WH` 为 `0.30/0.45/0.75/0.90` 换算到槽数的整数阈值。

## 8. 消费侧（consumer / 实时音频线程）：`pull`

### 8.1 与回放后端粒度的关系（读游标）

- 回放后端的回调粒度由后端决定（`frames_per_buffer` 只是请求值，**不假定是 480**，也不假定后端严格按它回调）。
- **server 后端 buffer 容量与 client 后端 buffer 容量可能不同**，因此一个 AudioFrame（槽）不会恰好对齐 client 的一次回调。
- "整槽"只作为**时间线 / 调整的单位**（跳过、补空白都按整槽）；真正往 `output` 填数据时，**后端要多少给多少**。
- 槽内用一个**读游标 `read_offset`**（`[0, F)`，sample 帧，表示当前槽已输出的帧数）做部分消费：槽没消费完就**保留剩余量，下次回调接着用**；消费完则 `play_seq+1`、`read_offset=0`。`read_offset` 是偏移量，不是缓冲。

### 8.2 pull 伪代码

`JitterBufferPullResult pull(std::span<std::byte> output) noexcept`：

```
pull(output):
  K = format.frames_from_bytes(output.size())   // 后端本次要的 sample 帧数
  frame_bytes = format.frame_bytes()
  result = { frames_filled=0, silence_frames=0, skipped_slots=0 }

  if !anchored:                       // 尚无任何帧：全静音（启动 pre-roll）
      zero(output); return { K, K, 0 }

  d = highest_seq − play_seq
  action = decide(d)

  if action == HOLD:                  // 低水位：全静音，play_seq/read_offset 不动
      zero(output)
      if !hold_until_target: hold_remaining = max(0, hold_remaining − K)
      return { K, K, 0 }

  if action == SKIP(step):            // 高水位：前移 play_seq
      play_seq = min(play_seq + step, highest_seq + 1)
      read_offset = 0
      result.skipped_slots += step

  // —— 正常消费（sample 帧游标）——
  filled = 0; silence = 0
  while filled < K:
      if play_seq > highest_seq:      // 耗尽：剩余填静音，play_seq 不动
          zero(output[filled*frame_bytes .. K*frame_bytes]); silence += K − filled; filled = K; break
      idx = play_seq % N
      slot = slots[idx]
      if slot.valid && slot.sequence == play_seq:
          n = min(F − read_offset, K − filled)
          copy slot.data[read_offset .. read_offset+n] (×frame_bytes) → output[filled .. filled+n]
          filled += n; read_offset += n
          if read_offset == F: play_seq += 1; read_offset = 0
      else:                           // 缺帧：输出 F 帧静音，跳过该槽
          n = min(F, K − filled)
          zero n 帧; filled += n; silence += n
          play_seq += 1; read_offset = 0

  result.frames_filled = filled; result.silence_frames = silence
  return result
```

### 8.3 缺帧（丢包）处理

- 丢包只会丢整个 AudioFrame；槽 `valid=false` 即缺帧。
- 缺帧输出 **`F` 帧静音**（`F` 已知，故静音时长精确），并 `play_seq+1`（时间流逝，跳过该槽）。
- 若 `F` 帧静音跨越 pull 边界（本 pull 剩余不足 `F`），只输出剩余部分、不足的静音自然并入下一 pull 的"耗尽补零"，误差被水位反馈吸收，可忽略。

要点：

- `pull` 始终把 `output` 填满（真实数据 + 静音），返回 `frames_filled == K`；不阻塞、不加锁、不堆分配。
- 缺帧与耗尽都输出静音，但**只有缺帧会推进 `play_seq`**；耗尽不推进（留给下次 `decide` 进入低水位恢复）。
- `HOLD` 期间 `hold_remaining` 按 `K` 扣减；由于 `decide` 每次 `pull` 重评估 `d`，到达 `T` 即停，最多多输出一拍静音（≈ 一次回调时长），可忽略。

## 9. 并发与内存序（SPSC）

- 生产者：网络线程，唯一调用 `push`。
- 消费者：回放后端实时音频线程，唯一调用 `pull`。
- 单生产者单消费者（SPSC）：
  - `play_seq`：consumer 写、producer 读（用于越界判断），`std::atomic<std::uint64_t>`。
  - `highest_seq`：producer 写、consumer 读，`std::atomic<std::uint64_t>`。
  - 槽数据：producer 写、consumer 读。
- 可见性约定：
  - producer 先写满槽数据，再以 **release** 更新 `highest_seq`（首帧还需先置 `anchored` 再发布）。
  - consumer 先 **acquire** 读 `highest_seq`（及 `anchored`），再读槽数据；只读 `sequence ≤ highest_seq` 的槽。
  - 由此，槽数据写入对 consumer 的可见性由 `highest_seq` 的 release/acquire 建立，槽数据本身无需原子。
- 实时线程约束：`pull` 路径**禁止锁、堆分配、系统调用**；预分配、日志等一律在 producer 侧完成。

## 10. 配置项汇总

| 配置 | 默认 | 说明 |
|------|------|------|
| `slot_count` | `30` | 环形槽数，外部控制容量（≈ 容量时长）。 |
| `format` | （必填） | 权威 `AudioFormat`，来自 server 下发。 |
| `frames_per_slot` | （必填，来自 server） | 每槽 sample frame 数 `F`，gRPC 建连时下发；预分配依据。 |
| `target` | `0.60` | 恢复目标 / 稳态中心。 |
| `normal_low / normal_high` | `0.45 / 0.75` | normal 区。 |
| `warning_low / warning_high` | `0.30 / 0.90` | warning 区边界。 |
| `step.skip_min / skip_max` | `1 / 0` | 高水位步长（槽）；`skip_max=0` = `max(2, round(0.10×N))`。 |
| `step.hold_min / hold_max` | `160 / 0` | 低水位步长（帧）；`hold_max=0` = `8 × hold_min`。 |
| `step.growth` | `2.0` | 每连续评估一次的倍率。 |
| `step_fn` | 默认指数函数 | 可插拔，见 §7.3。 |

容量 / 延迟换算：`总时长 = N × F / sample_rate`，稳态时长 = `总时长 × 60%`。

## 11. 公开接口草案（`include/aqua/audio/buffer/jitter_buffer.h`）

```cpp
namespace aqua::audio {

enum class AdjustDirection { Raise, Lower };

struct WarningStepParams {
    std::uint32_t skip_min = 1;
    std::uint32_t skip_max = 0;   // 0 = max(2, round(0.10*N))
    std::uint32_t hold_min = 160; // sample 帧
    std::uint32_t hold_max = 0;   // 0 = 8 * hold_min
    double       growth   = 2.0;
};

using WarningStepFn = std::uint32_t (*)(const WarningStepParams&, AdjustDirection, std::uint32_t k) noexcept;
std::uint32_t default_warning_step(const WarningStepParams&, AdjustDirection, std::uint32_t k) noexcept;

struct JitterBufferConfig {
    std::uint32_t slot_count = 30;
    AudioFormat   format;
    std::uint32_t frames_per_slot = 0;   // F，必填（来自 server）

    double target = 0.60;
    double normal_low = 0.45, normal_high = 0.75;
    double warning_low = 0.30, warning_high = 0.90;

    WarningStepParams step;
    WarningStepFn step_fn = &default_warning_step;
};

struct JitterBufferPullResult {
    std::uint32_t frames_filled = 0;   // 本次实际填充帧数（通常 == 请求帧数）
    std::uint32_t silence_frames = 0;  // 其中静音帧数（缺帧 + 低水位 hold）
    std::uint32_t skipped_slots = 0;   // 本次跳过的槽数（高水位）
};

class JitterBuffer {
public:
    static std::expected<std::unique_ptr<JitterBuffer>, AudioError>
    create(const JitterBufferConfig& config);   // 校验 config；非法 → InvalidArgument

    bool push(const AudioFrame& frame) noexcept;                  // producer
    JitterBufferPullResult pull(std::span<std::byte> output) noexcept; // consumer（实时）
    double water_level() const noexcept;                         // 监控/测试用
    void reset() noexcept;                                       // 停止回放后复用
};

} // namespace aqua::audio
```

回放回调粘合层（示意）：

```cpp
std::uint32_t on_playback(void* ud, std::span<std::byte> output) noexcept {
    return static_cast<JitterBuffer*>(ud)->pull(output).frames_filled;
}
```

## 12. 测试矩阵（确定性，不依赖真实网络/设备）

用合成的 push/pull 序列驱动，逐项断言：

1. **顺序输出**：有序 push → 有序 pull 出完整 PCM。
2. **锚定**：首帧 `sequence` 非 0（如 `12345`）→ 正确锚定，后续正常。
3. **乱序**：push `5,6` 再 push `4` → `4` 落入正确槽，输出 `4,5,6`。
4. **缺帧静音**：push `10,12`（缺 `11`）→ `11` 位置输出恰好 `F` 帧静音，`12` 正确，`play_seq` 正常推进。
5. **迟到/重复丢弃**：`s < play_seq` 与同槽同 sequence 重复 → 拒收，不影响后续。
6. **越界丢弃**：`s ≥ play_seq + N` → 拒收。
7. **水位与分区边界**：构造 `d` 遍历 `0..N`，断言 `deadline/warning/normal` 分类与 `30/45/75/90/60` 边界一致。
8. **deadline 低 / pre-roll**：空缓冲 → 持续静音；填充到 `W ≥ 60%` 后开始出真实数据。
9. **deadline 高**：填到 `W > 90%` → 一次 `pull` 内跳到 `60%`。
10. **warning 递增**：持续 warning → 步长 `1,2,3,…` 封顶；到 `60%` 后停止并复位计数器。
11. **回绕**：跨 `N` 的整数倍 sequence → 索引正确、无陈旧槽误读。
12. **部分消费（读游标）**：`pull` 请求大小 ≠ 槽大小（更大/更小、非整数倍）→ 读游标正确，跨槽/跨回调边界不丢不重，剩余量正确留存。

## 13. 未决 / 后续

- **控制面字段**：本设计依赖 `ConnectResponse` 新增每 AudioFrame 的 `frames_per_slot`（`F`）字段，且 server 以该固定大小发送。该字段需在实现 JB 前落进 `aqua_service.proto` 并透传为 `JitterBufferConfig::frames_per_slot`。
- **PLC 插入点**：缺帧当前输出 `F` 帧静音；未来 PLC 作为 `pull` 的后处理接入（据 `silence_frames` 触发），不新增缓冲。
