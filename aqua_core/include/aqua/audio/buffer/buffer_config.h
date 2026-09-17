#ifndef AQUA_AUDIO_BUFFER_BUFFER_CONFIG_H
#define AQUA_AUDIO_BUFFER_BUFFER_CONFIG_H

// Buffer 组件（JitterBuffer / JitterEstimator / TargetController）的常量与默认取值集中地。
//
// 所有常量位于 aqua::config 命名空间，与 udp_config.h / grpc_config.h /
// runtime_config.h 共用同一命名空间，避免多个 config 命名空间互相遮蔽。
//
// 分层约定（与 udp_config.h 一致）：
//   - 本文件 = Buffer 组件自己的默认取值，组件单独使用时也成立；
//   - runtime_config.h = runtime / CLI 层默认值与边界，引用本文件；
//   - 三个组件头文件只保留类型与结构，默认数值一律从这里取；
//   - CLI 的 --jb-* 默认值一律从这里取，避免同一旋钮两处定义。
//
// 调参提示：本文件的默认值是实测与仿真的结论，不是安全边界。多数常量可以
// 直接改动观察效果；真正"改了会坏"的只有少数几个（已逐个标注）——不要把
// 实验结论误当硬约束，想试极值就试，听感不对再调回来。

#include <cstdint>

namespace aqua::config {

// ==================== JitterBuffer：容量 ====================

// 环形槽数下限。低于 4 时五个水位带（warning_low < normal_low < target <
// normal_high < warning_high）无法保持严格序，JitterBuffer::create 会直接
// 拒绝。这是**结构性**下限，不是经验值。注意严格序是浮点口径：capacity=4
// 时整数化后是 1,1,2,3,4（warning_low == normal_low，阶梯填充带消失，
// decide 退化为更积极的 hold-fill，行为安全）；capacity≥5 才是整数严格序。
inline constexpr std::uint32_t JB_MIN_CAPACITY_SLOTS = 4;

// 环形槽数上限。纯护栏，防误配置（30 槽 @1400B ≈ 42KB；512 槽 ≈ 0.72MB /
// 1.87s @3.646ms（F=175/48kHz），已远超任何真实抖动需求）。同时是 RT 护栏：reanchor 在
// RT 线程上是 O(capacity) 扫描（低频异常路径，有界），512 把单次扫描压到
// 亚毫秒级——4096 槽（15.4s 缓冲）对 LAN 实时音频本就是病态配置。
inline constexpr std::uint32_t JB_MAX_CAPACITY_SLOTS = 512;

// 默认环形槽数（= 30 × 3.646ms ≈ 109ms @F=175/48kHz；F 随格式变化，
// 每槽毫秒数 = F / sample_rate，不要死记 3.646）。
// 这是"延迟 ↔ 抗抖动"的主刻度：越大越抗抖动、稳态播放延迟越高。
// 自适应模式下 target 另有 2/3 的结构上限（见 JB_ADAPTIVE_TARGET_CAPACITY_RATIO），
// 所以容量买的是"抖动吸收余量"，不全是延迟。
inline constexpr std::uint32_t JB_DEFAULT_CAPACITY_SLOTS = 30;

// ==================== JitterBuffer：水位带（固定模式的 capacity 分数）====================

// 稳态中心 / 恢复目标。lead 稳定在 target 附近即为健康。
inline constexpr double JB_TARGET_RATIO = 0.60;

// normal 带下界：低于它开始 FILL（慢放重复当前槽以追平）。
inline constexpr double JB_NORMAL_LOW_RATIO = 0.35;

// normal 带上界：高于它开始 DROP（跳槽以追上）。
inline constexpr double JB_NORMAL_HIGH_RATIO = 0.80;

// warning/deadline 下分界：低于它进入强制静音 Hold（时间轴修正）。
inline constexpr double JB_WARNING_LOW_RATIO = 0.20;

// warning/deadline 上分界：高于它进入 deadline-high DROP（跳步最大）。
inline constexpr double JB_WARNING_HIGH_RATIO = 0.90;

// 启动 pre-roll 锚定水位（独立于上面的稳态阈值序，取值 (0,1]）。
// 0.50：早于 target 锚定，给涌入中的帧留 headroom（等 target 会在通知间隙
// 被网络推入打满低容量 JB → deadline-high Drop 抽搐），又高于 normal_low
// 提供抗抖动垫层。锚定后 lead 位于 normal 区，稳态自然向 target 漂移。
inline constexpr double JB_STARTUP_LEVEL_RATIO = 0.50;

// ==================== JitterBuffer：warning 区步长 ====================

// 起始步长（槽）：刚进 warning 区时每次评估跳几槽。
inline constexpr std::uint32_t JB_WARNING_STEP_MIN_SLOTS = 1;

// max_step=0 自动推导时的下限：max(本值, round(JB_WARNING_STEP_AUTO_FRACTION × N))。
// 保证小容量 JB 也有 ≥2 槽的修正能力，不至于一格一格磨。
inline constexpr std::uint32_t JB_WARNING_STEP_AUTO_MIN_SLOTS = 2;

// max_step 自动推导的容量比例。10% 意味着 30 槽 JB 单次最多跳 3 槽——
// 修正要够快（否则追不上持续高水位），又不能一次跳太多导致听感突兀。
inline constexpr double JB_WARNING_STEP_AUTO_FRACTION = 0.10;

// 步长增长倍率：持续处于 warning 时，步长按此倍率逐级放大。
inline constexpr double JB_WARNING_STEP_GROWTH = 2.0;

// 连续多少次 warning 评估才增长一级。4 次 ≈ 温和起步、久拖才变激进，
// 避免偶发抖动被当成持续偏移来猛修。
inline constexpr std::uint32_t JB_WARNING_GROWTH_INTERVAL = 4;

// ==================== JitterBuffer：reanchor（时间轴重锚定）====================

// reanchor 请求允许的最大序列跨度（帧）。超过即判定为荒谬请求并拒绝（sanity）。
// 100'000 帧 @48kHz ≈ 2.1 秒的时间线跳变——真断流也不会这么跳，基本只可能
// 来自解析错误或恶意流。放宽会让一次坏包把播放头弹飞。
inline constexpr std::uint64_t JB_MAX_REANCHOR_JUMP_FRAMES = 100'000;

// reanchor 后水位卡死的兜底：连续该次数 pull 内水位无进展则强制放弃 Hold。
// 防止"等 target 等不到"变成永久静音。
inline constexpr std::uint32_t JB_REANCHOR_HOLD_STUCK_PULLS = 5;

// 远超前 reanchor 的最小缺口（包）。s 与 highest 的间隔小于该值视为顺序溢出
// （ring 满后 producer 短暂无法落盘，highest 冻结），应由 deadline-high DROP
// 兜底而非 reanchor；只有缺口明显更大（时间线断裂）才 reanchor。
// 值太小会把正常满窗误判为断裂（reanchor 风暴），太大则漏掉真正的中断跳变。
inline constexpr std::uint32_t JB_REANCHOR_MIN_GAP_SLOTS = 4;

// ==================== JitterBuffer：PCM concealment ====================

// 连续掩盖上限（包）。第 i 个被掩盖的包增益 = (max - i) / max（线性淡出），
// 第 max+1 个起转静音。0 = 关闭 concealment（退化成 v1 硬静音）。
// 3 包 ≈ 10.9ms @F=175/48kHz：再长就是"重复音"而不是"掩盖"，听感比静音更糟。
inline constexpr std::uint32_t JB_CONCEALMENT_DEFAULT_MAX_SLOTS = 3;

// ==================== JitterEstimator（纯网络观测）====================

// 滑动观测窗大小（包）：[max_ext-63, max_ext] 内见过的 seq 标记位。
// 窗内已见 = duplicate；窗内未见但落后 max = reordered；窗外 = late。
// 64 = u64 位图一位一包，零成本；再大要换结构。
inline constexpr std::uint64_t JB_ESTIMATOR_REORDER_WINDOW_PACKETS = 64;

// stall 判定阈值（**包周期**的倍数）：到达间隔超过它即判为 stall（时间断流），
// 该间隔**不计入** RFC 3550 的 J，只记 stall_events。
// 为什么必须剔除：一次 100ms 的瞬断会把均值型 J 顶高好几毫秒，之后十几秒
// 的 target 都被这次历史事故绑架。
// 5.0：burst 发包的正常串间间隔约 2.7 个包周期，留近一倍余量；35ms 级及以上
// 的真 stall（双机实测 35~170ms）稳稳落在线外。0 = 关检测（每个间隔都进 J，
// 回到裸 RFC 3550）。
inline constexpr double JB_ESTIMATOR_DEFAULT_STALL_THRESHOLD_PACKETS = 5.0;

// stall 峰值的衰减速度（ms/s）。estimator 跟踪"近期最坏到达间隙"的衰减最大
// 值（NetEq DelayManager 的 peak detection 思路：每次 stall 刷新为
// max(峰值, 本次间隙)，无 stall 时按本速率线性回落），TargetController
// 用它把 target 抬到"能挺过近期最坏间隙"的水位——J 是均值型观测量，
// 被 stall 门剔除的尾部全靠这个峰值项补回来。
// 衰减速度的取舍：周期性中小 stall（下载拥塞实测 ~2.7 次/s、间隔 ~370ms）
// 之间只衰减 ~3.7ms，峰值紧贴近期最坏值，target 对拥塞保持反应；单次大
// stall 则要限制"钉高位"时长——10ms/s 下 170ms 事故从峰值衰减到 k×J 交叉
// 点（~26ms）约 14s（与旧 stall 门防的"顶到 21 挂 14s"同级，但有界、可
// 解释），50ms stall 约 2.4s。调小 = 峰值更持久（稀疏 stall 也记得住，
// 但大事故挂更久）；调大 = 更快忘记（延迟回落快，稀疏 stall 之间可能
// 掉得太低再次欠载）。
inline constexpr double JB_ESTIMATOR_STALL_PEAK_DECAY_MS_PER_SEC = 10.0;

// ==================== JitterBuffer：修正拼接 crossfade ====================

// 修正拼接点的线性 crossfade 长度（采样帧）。DROP 跳槽着陆、FILL 重播
// 开头、conceal 启动都是整包硬拼接（3.646ms @F=175/48kHz），音乐里即咔哒/断音；
// 在拼接后的前 N 帧从"最后一个已播采样"线性淡出即消除台阶。64 帧 ≈ 1.33ms：
// 覆盖一次跳变，涂抹可忽略。钳制到 [2, frame_count]（1 帧无意义，超一包无必要）。
// 只在 SpliceConfig.enabled 时分配/生效，组件默认关。注意：这只改输出样本值，
// 不动时间轴、不动 target、不动 episode——控制零风险。
inline constexpr std::uint32_t JB_SPLICE_DEFAULT_XFADE_FRAMES = 64;

// ==================== JitterEstimator 尾部直方图（默认抖动项输入）======================

// 单包绝对偏差 |到达间隔 - 发送间隔| 的滑动窗口长度（包）。2048 包 @274 包/s
// ≈ 7.5s：覆盖 Wi-Fi 省电周期与下载拥塞波动，又不至于记住上古事故。
// 刻意用差分而不用 transit - base：累积最小 base 在漂移/阶跃下永不更新，
// 相对值钉高位；差分天然漂移不变。窗口滑出即遗忘——涨快（新极端立刻进 P99）
// 跌慢（旧极端滑出才跌）天然不对称，不需要 dwell/限速第二套机制。
// 影子阶段只观测不驱动控制；转正时 margin 策略枚举加档，ScaledJitter 原样保留。
inline constexpr std::uint32_t JB_TAIL_WINDOW_PACKETS = 2048;

// 直方图桶数：1ms/桶，[0,64)ms + ≥64ms 溢出桶。64 桶线性扫求 P99，每包 O(64)
// 可忽略；桶量化 1ms ≈ 0.27 槽，ceil 吸收。
inline constexpr std::uint32_t JB_TAIL_HISTOGRAM_BUCKETS = 64;

// 尾部分位数：P99。均值噪声碰不到它，只有尾部运动才搬得动 desired。
inline constexpr double JB_TAIL_QUANTILE = 0.99;

// 尾部生效的最少样本数。窗口未满时 P99 ≈ 最大值（n<100 时单个 spike 直接
// 就是 P99），冷启动一次断流能把 target 一步顶到顶——启动期水位本来就薄，
// 必须回退 k×J（J 对单 spike 按 1/16 衰减，0.2 秒洗掉）。128 包 ≈ 0.47s。
// 未满时 estimator 发布 -1（诊断 p99 列显示 -1.0 = 暂无尾部数据），
// controller 按 tail<0 回退 k×J。
inline constexpr std::uint32_t JB_TAIL_MIN_SAMPLES = 128;

// ==================== TargetController：风暴端稳 ====================

// 风暴进入阈值（stall 事件/秒）。stall 事件（到达间隔超阈值的次数）频繁 =
// "断流频发期"：逐个追 spike 只会让 target 上蹿下跳（WiFi 实测 116 次/300s
// 伴随 drop 168 次）。tumbling 窗口计数：窗口内频率 ≥ 本阈值进入风暴；
// 连续一个完整窗口零事件才退出（进快出慢）。风暴期冻结一切下跌（涨仍即时）。
// 阈值 1.0/s：成串 stall（2~5 次/2s）进，零散不进；窗口 2s：进入延迟 ≤2s，
// 退出 ≤4s。常年恶劣链路会一直端着——那正是它需要的 buffer，不是债务。
inline constexpr double JB_STORM_ENTER_EVENTS_PER_SEC = 1.0;
inline constexpr std::uint32_t JB_STORM_WINDOW_MS = 2000;

// ==================== TargetController（自适应 target）====================

// 自适应 target 的结构上限 = capacity × 本比例（默认 2/3）。
//
// 这是**硬性的结构约束，不是调优值**，改动前务必读懂原因：
// 水位带随 target 等比缩放（warning_high ≈ 1.5×target）。target 一旦顶到
// capacity，整条高水位带就落到 ring 之外，DROP 机制彻底失效；consumer 随后
// 把 lead 顶满 ring，新到的包全部撞上未消费槽位被 slot-busy 拒收——人为造洞，
// consumer 再在洞上欠载。极端 gain 实测：busy≈25% rx、underrun≈30%，而 UDP
// 零丢包、声音全破。
// 上 1/3 留给抖动吸收，与固定模式的 0.60 target / 0.90 ceiling 是同一结构。
inline constexpr double JB_ADAPTIVE_TARGET_CAPACITY_RATIO = 2.0 / 3.0;

// target 硬下限的默认取值（槽，= `--jb-min-target` 的默认值）。
//
// **有效下限 = max(本值, 几何地板 + 1)**：几何地板（= 一次 playback callback
// 消耗的包数 + 1，见 geometric_floor_slots）无条件托底，本旋钮只能把下限往上
// 抬，压不下去——target 低于几何地板意味着"每个 callback 必然把 JB 抽空"，
// 那是结构性的，与抖动无关，不是可选项。
//
// 为什么默认 3：实测 2 slots 在 F=175（3.646ms）链路上正好落在欠载悬崖之下
// （16.6% 欠载 + 25% 丢帧），3 slots 归零；再往上抬只是换延迟。
// 说明：WiFi 实测曾短暂把默认抬到 6（≈22ms 盖住 20~50ms 常态断流），后回退——
// 没有一组默认值能同时适用于 LAN/WiFi/2.4G，改由用户按需调 --jb-min-target
// （WiFi 建议 6~7，见 jitter_buffer_control_design.md 选档方法），出厂默认
// 保持实验室中性值。
inline constexpr std::uint32_t JB_ADAPTIVE_DEFAULT_MIN_TARGET_SLOTS = 3;

// 起步 target（槽）。仅用于建流后第一个包之前的窗口；J 在约 16 个包
// （≈60ms）内收敛，target 随即被自适应拉到稳态值。会被夹到 [floor, max]。
inline constexpr std::uint32_t JB_ADAPTIVE_DEFAULT_INITIAL_TARGET_SLOTS = 4;

// 起步 pre-roll 水位的绝对下限（槽）。自适应模式下 startup_level =
// max(本值, 起步 target)：起步水位若低于 target，锚定后立刻落进 normal 区
// 以下触发 FILL（静音等待），等于把启动延迟换成静音——不如直接按 target 起步。
inline constexpr std::uint32_t JB_ADAPTIVE_STARTUP_MIN_SLOTS = 3;

// 组件单独使用（调用方未提供）时的默认包时长（ms）。真实链路总由
// ClientRuntime 按 F/sample_rate 计算后传入。
inline constexpr double JB_ADAPTIVE_DEFAULT_PACKET_MS = 10.0;

// k：margin = k × J（包单位）。**自适应模式的主力旋钮**，也是 CLI 唯一保留的
// 连续型调参项。
//
// 为什么不是教科书的 2~3：J（RFC 3550 A.8）是 |到达间隔偏差| 的**均值**，
// 而 target 必须覆盖**峰值**。Aqua 的 server 以 capture 周期成串发包
// （480 帧/10ms 抓一次，175 帧/包 → 每 10ms 一串 2~3 个包，串内间隔≈0；
// 发送端已按 packet 周期 pacing 摊平，这里说的是**未摊平**时的形态），
// 这种确定性 burst 下 J≈4.6ms 而实际峰峰值 8.75ms ≈ 2 倍均值；再叠加
// callback 周期（10.667ms）与发包周期（10ms）的拍频，拖到最坏相位时
// k=2 给出的 3 slots 会周期性排空（双机实测 6.5% 欠载 + 12% 丢帧）。
// 离线仿真（同几何、扫 16 个相位取最坏）中 k≥5 才把欠载压到 0。
// 干净 / 匀速链路上 J→0，margin→0，target 落到地板，不会过度缓冲。
// 调大不再无限涨延迟：超出的部分被 2/3 结构上限接住（见上）。
inline constexpr double JB_ADAPTIVE_DEFAULT_JITTER_GAIN = 5.0;

// 恢复限速（槽/秒）：网络变好后 target 每秒最多降这么多。
// 只锁**下跌**——上涨永远即时（恶化必须立刻跟进）。这是"涨快跌慢"的峰值保持，
// 防止抖动项在槽边界上下摆动时 target 来回抽动。0 或负 = 取默认值（构造器归一）——
// **没有"不限速"这个极值**，想要瞬时回落就直接填一个很大的值（如 1000）。
// 默认 0.2（2026-09 实测结论）：收发速率视为相同（晶振差百 ppm 级，37 秒才漂
// 1 包，相对 1.6 秒一次的修正可忽略），水位运动几乎全是抖动/突发——跌慢只换
// 延迟不换安全，episode 频率肉眼可见下降。干净网验证：跌速与欠载无关
// （欠载由 penalty 地板 + stall 项兜底），0.2/s 下 5 秒才跌 1 格，突发间隔
// 内 target 基本不动。
inline constexpr double JB_ADAPTIVE_FALL_RATE_SLOTS_PER_SEC = 0.2;

// 远距快速跌落的距离门限（槽）与倍率：current - desired ≥ 本门限时跌速 × 本倍率。
// 慢跌速的代价是风暴过后的 overhang（19 跌回 4 要 75 秒，期间水位偏高 FILL
// 不断）；近平衡区（<4 槽）的 chatter 仍按 0.2/s 磨。远距 4×0.2 = 0.8/s，
// 15 格 overhang 约 19 秒回家。风暴冻结/dwell 优先级不变（它们在别的分支）。
inline constexpr std::uint32_t JB_FALL_FAR_DISTANCE_SLOTS = 4;
inline constexpr double JB_FALL_FAR_RATE_MULTIPLE = 4.0;

// 上涨后的峰值保持窗口（ms）：窗口内禁止下跌。
//
// 为什么需要：J 随发包几何在 ceil 边界上下摆动（Wi-Fi 省电时发包变成
// ~20ms 一大串，J 在 4.7↔5.4ms 间跳，margin 6.3↔7.2 → ceil 7↔8），跌侧
// 1 槽/秒的限速又不断制造下跌腿，于是 target 反复横跨 7↔8。单看翻转无所谓
// （lead 在 normal 带内就不触发 Fill/Drop），但 target 一旦落到偏低的 7，
// lead 就会撞上 normal_high 触发 Drop（实测 drop_duty 1.4%）。
// 涨后 dwell 内锁跌 = 峰值保持。0 = 关（退回纯限速行为）。
// 默认 5000（2026-09 实测结论）：配合 0.2/s 跌速，一次涨上去多端一会儿；
// 收发速率相同假设下，端着不跌只换延迟（见 FALL_RATE 注释），episode 频率下降。
inline constexpr double JB_ADAPTIVE_RISE_DWELL_MS = 5000.0;

// 每次**可闻**欠载抬升 target **下限**的槽数。"可闻"口径见
// select_penalty_events（conceal 开时只有掩盖封顶溢出才计，孤立短缺口不计）。
// 欠载反馈是"安全网"，不是主力：预测项覆盖不了随机尾部与丢包，反馈项补这个洞。
// 抬的是下限而不是加到 margin 上——这样抖动项已经很高时不会重复叠加。
// 被盖住的缺口不触发，干净链路上恒为 0，不增延迟。0 = 关闭整条反馈闭环。
inline constexpr double JB_ADAPTIVE_UNDERRUN_PENALTY_SLOTS = 1.0;

// 欠载反馈累计上限（槽）：防止病态链路把 target 一路顶到结构上限。
inline constexpr std::uint32_t JB_ADAPTIVE_UNDERRUN_PENALTY_MAX_SLOTS = 6;

// 无新欠载时反馈项的回落速率（槽/秒）。比 FALL_RATE 慢，意味着"坏过一次就
// 多安全一会儿"；0 = 不衰减（一旦抬起来就不下来，等效于永久提高下限）。
inline constexpr double JB_ADAPTIVE_UNDERRUN_PENALTY_DECAY_SLOTS_PER_SEC = 0.5;

// stall 峰值项的安全余量（包）。margin 的 stall 峰值项 =
// 近期最坏到达间隙 / 包周期 + 本值，与 k×J 取 max（谁大听谁：两者都是
// "要多少水"的估计，相加会重复计——stall 的亚阈值残余本来就在 J 里）。
// +1 包 = 挺过最坏间隙后水位不归零（留一包垫到达相位），与几何地板 +1
// 的"一个 callback 口粮 + 一包余量"是同一思想。0 = 峰值刚好贴边（间隙
// 结束时水位归零，下一次 stall 稍有拖长就欠载）。
inline constexpr double JB_ADAPTIVE_STALL_PEAK_EXTRA_PACKETS = 1.0;

// stall 峰值项的上限（槽）。stall 是"已经发生的恢复风险信号"，不是新的
// steady-state 延迟要求——一次 60ms+ 的孤立 stall 不该买入十几槽的延迟债务
// （双机实测 59.6ms stall：target 7→17，随后 17 次 DROP / 49 skip_slots 的
// 还债风暴）。加上限后：stall_margin = min(峰值/包周期+余量, 本值)，线性增长
// 再饱和——这本身就是 soft/hard 两区制，不需要第二个显式阈值。
// 8 槽 ≈ 29ms（@3.646ms 包）：实测异常分三档——19~25ms（下载常态）、30~34ms
// （调度/切歌档）、50ms+（真事故），本值完整覆盖前两档；第三档交给欠载
// penalty（抬下限）+ concealment（兜 3 包）+ reanchor（断流兜底），各管一段。
// 与衰减的关系：JB_ESTIMATOR_STALL_PEAK_DECAY_MS_PER_SEC 决定"封顶后的高位
// 挂多久"，本值决定"最多推多高"。调大 = 孤立大 stall 的欠载更少但延迟债务
// 更重；调小 = 债务更轻但 30ms 档开始漏欠载。
inline constexpr double JB_ADAPTIVE_STALL_PEAK_CAP_SLOTS = 8.0;

// 死区（槽）：期望与当前差值在该范围内不动。**默认 0** —— 死区与"跌侧不限
// 死区 grind 到底"叠加会产生永久偏移：跌到 desired 后，desired 回升 ≤deadband
// 被吞掉，target 永远停在 desired−1（实测 target 卡 2 而 desired=3，对应
// 16.6% 欠载 + 25% 丢帧）。阻尼由跌侧限速提供，不要在这里加死区。
inline constexpr std::uint32_t JB_ADAPTIVE_DEADBAND_SLOTS = 0;

// ==================== 控制面诊断日志 ====================

// 决策层稳态摘要的节流周期（ms）。TargetController 每次 update 都结算一份
// "本拍为什么涨 / 跌 / 不动"的完整状态，但只有在 target 变化（事件驱动）或
// 距上次摘要超过本周期时才打一行——否则 push strand 会按包频率刷屏。
// 这是**日志节奏，不是控制参数**：改它只影响日志密度，不影响 target。
inline constexpr double JB_CONTROL_LOG_SUMMARY_INTERVAL_MS = 5000.0;

// push 拒绝汇总日志的节流周期（ms）。首次拒绝立即打，之后最多每本周期一行；
// 行内给出与上次打印之间的增量（累计计数器的差分）。同样是**日志节奏**。
inline constexpr double JB_CONTROL_LOG_REJECT_INTERVAL_MS = 1000.0;

// reanchor 探测日志（"近端远超前"分支）的节流分母：第 1 次 + 每 N 次一行。
// 环形满溢（未达断裂缺口，交由 deadline-high DROP 兜底）会按包频率命中本分支，
// 不节流就会在一次长断流里刷出成百上千行。同样是**日志节奏**。
inline constexpr std::uint32_t JB_CONTROL_LOG_REANCHOR_PROBE_EVERY = 128;

} // namespace aqua::config

#endif // AQUA_AUDIO_BUFFER_BUFFER_CONFIG_H
