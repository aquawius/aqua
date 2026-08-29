# aqua_server

## 参数

```text
--rpc-ip                默认 0.0.0.0
--rpc-port              默认 50051
--udp-ip                默认 0.0.0.0
--udp-port              默认 9999
--advertise-ip          默认跟随 --udp-ip；wildcard 交给 client fallback
--encoding              s16|s24|s32|f32|u8
--channels              声道数
--sample-rate           Hz
--frames-per-slot       0=auto，显式值 >=16
--capture               loopback|input，默认 loopback
--device-id             capture device id
--session-timeout-ms    默认 5000
--reap-interval-ms      默认 1000
--network-queue-slots   默认 4，范围 1..4096
--log-level             trace|debug|info|warn|error|fatal
--list-devices
--help
```

## 格式参数

`--encoding/--channels/--sample-rate` 要么三个全部出现，要么三个都不出现。

不出现：使用 capture backend default format。

出现：必须先通过 `AudioFormat::is_valid()`，然后检查 `F × frame_bytes <= 1443`。

## frames-per-slot

`0`：CLI 不指定 F，Runtime/packetizer 使用 MTU budget 自动推导。

显式 F：

```text
F >= 16
F × frame_bytes <= 1443
```

## 设备语义

`--capture=loopback` 时 `--device-id` 指 OUTPUT endpoint；`--capture=input` 时指 INPUT endpoint。未指定 device-id 使用系统默认对应方向。

## 启动流程

```text
parse
→ create asio::io_context
→ make_shared<ServerRuntime>
→ start
→ install diagnostics/control/signal timers
→ io_context.run
→ stop
```

Server 必须由 `shared_ptr` 持有，因为 reaper 使用 weak self。
