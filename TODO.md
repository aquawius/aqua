# TODO

待办事项（按优先级）。已决策不做/已明确延后的也记录在此，避免重复讨论。

---

## 1. 服务端发送路径：消除每帧/每包堆分配与锁

**状态**：待办（本次不做，记录下来后续实现） **性质**：实时延迟尾巴 / 分配抖动，非正确性问题 **主题**：producer 侧（server）与 RT
无关但会加重 GC/分配器压力，进而抬高端到端抖动

### 现状（问题点）

| 位置                                                    | 现状                                                                                                                                                                         | 频率                                                       |
|---------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|------------------------------------------------------------|
| `aqua_core/src/runtime/audio_network_dispatcher.cpp:96` | `make_shared<const vector<byte>>(NetworkFrame::audio(...).encode())` —— 每帧 **2 次**堆分配（vector 缓冲 + shared_ptr 控制块），且编码结果先落临时 `vector` 再转 shared 持有 | 每帧一次，F=480@48k 约 **100 次/秒**（3ms 帧约 330 次/秒） |
| `aqua_core/src/net/udp/udp_transport.cpp:280`           | `send_to()` 拷贝语义：再 `make_shared<vector<byte>>` 复制一份 → 每包又一次分配 + 拷贝                                                                                        | 每帧 × 接收端数量                                          |
| `aqua_core/src/net/udp/udp_transport.cpp:307`           | `tx_queue_mutex` 每包加解锁一次（strand 已串行化生产者，实际无竞争，但非 RT 原语）                                                                                           | 每包一次                                                   |

结果是「一帧音频 → 3~4 次堆分配 + 2 次拷贝 + 1 次加锁」，全部落在发送热路径上。

### 建议方案

1. **编码目标缓冲池化**：`AudioNetworkDispatcher::drain()` 里复用一个预分配的 scratch buffer（payload 上界见
   `udp_config.h` 的 1440B / `UDP_MAX_PAYLOAD`），`NetworkFrame::audio(...).encode_to(span)` 直接写进去，避免中间 `vector`。
2. **发送缓冲池化**：`UdpTransport` 增加「buffer pool + 回收」路径——`send_to(span)` 从池里取一个 `shared_ptr`（自定义
   deleter 归还池），编码结果直接写入后交给 `send_to_shared()`；in-flight 期间由 shared_ptr 持有，async_send_to 完成后自动归还。目标是
   datagram 稳态零分配。
    - 变体（更简单）：先只做「`send_to` 不再二次拷贝」——让 dispatcher 直接产 `shared_ptr` 交给 `broadcast()`，跳过
      `send_to(span)` 这一跳，可先消掉一半分配。
3. **TX 队列无锁化**：`send_queue` 改为定长 SPSC/MPSC 环形队列（strand 已串行化消费者），去掉 `tx_queue_mutex`
   ；队列满仍保持现有「丢最旧待发项」语义与 `tx_dropped` 计数。
4. **顺带**（同一工作流，控制面/低频，优先级低）：
    - `udp_client.cpp:149` 每包取 `learned_mutex` —— 单 asio strand 内实际无竞争，可改 atomic 快照或 strand 内直读。
    - `udp_server.cpp:79` heartbeat ACK 每 1Hz 一分配 —— 可忽略，除非顺手。

### 验收标准

- 稳态（无丢包、队列不满）下 server 发送路径堆分配次数为 **0**（可用分配计数 hook 或 VS/ASan 的 allocation counter 验证）。
- `tx_dropped` / `encode_failures` / `dispatch_failures` 在同等负载下不劣化。
- 端到端延迟 P99 不劣化；理想情况抖动（jitter_ms）下降。
- 全部既有单测通过；新增一条「连续 N 帧发送后池无泄漏/容量不增长」的测试。

### 风险 / 注意

- 池化后 buffer 生命周期由 shared_ptr 跨 strand 持有， **必须**保证 async_send_to 完成前不被复用（否则静默数据损坏）——建议用自定义
  deleter 归还，不要手写引用计数。
- 池容量要有上界并计入诊断，避免 under pressure 时无限增长掩盖背压问题。

