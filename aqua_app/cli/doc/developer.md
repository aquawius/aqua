# CLI 开发约定

1. parser 只生成 `RuntimeConfig`，不创建 runtime。
2. main 负责：logger init、parse、runtime create/start、diagnostics、signal、stop。
3. 不在 CLI main 实现 UDP/gRPC/audio 业务。
4. 新配置先加入 Core config，再在 CLI parser 映射；不要在 CLI 偷加第二套语义。
5. 任何新参数都必须说明单位、默认值、范围以及是否会改变协议/RT 语义。
