# Aqua Architecture

> Current implementation baseline for `aqua_core` + `aqua_app`. This document describes implemented contracts; future ideas are explicitly marked as future work.

## 1. System shape

```text
                           CONTROL PLANE
Client  ───────────── gRPC Connect/Disconnect ─────────────> Server
Client  <──── session + UDP endpoint + format + F ───────── Server

                            DATA PLANE
Server Capture
    │
    ▼
AudioBlock
    │
    ▼
AudioPacketizer ── fixed AudioFrame ──> AudioFrameQueue
                                             │
                                             ▼
                                   AudioNetworkDispatcher
                                             │
                                             ▼
                                         UdpServer
                                             │
                                             ▼
                                            UDP
                                             │
                                             ▼
                                         UdpClient
                                             │
                                             ▼
                                      JitterBuffer
                                             │
                                             ▼
                                        AudioPlayback
```

## 2. Hard module boundaries

`audio` owns PCM types and audio algorithms. `net` owns wire encoding, sockets, session-aware UDP dispatch and gRPC. `runtime` composes modules. `app/cli` only parses user input and presents diagnostics.

A network component never takes `AudioFrame` as its public API. The only bridge is `AudioNetworkDispatcher`, which converts the audio-domain object to `NetworkFrame` before entering `UdpServer`.

## 3. Server composition

ServerRuntime owns the server graph and its one-shot lifecycle. During construction it resolves the selected capture endpoint and its effective audio format early enough to preallocate the realtime handoff path. The product default is OUTPUT loopback on the platform default render endpoint; an explicit device-id alone therefore changes only the selected OUTPUT endpoint unless `--capture=input` is also specified. During start it creates/starts the capture backend, UDP, dispatcher, gRPC and session reaper.

The effective server format is:

```text
explicit ServerRuntimeConfig.format
        OR
capture backend/device default shared format
```

When the format is omitted, `AudioDeviceManager::default_format()` queries the selected capture endpoint without starting the stream. After capture startup, `AudioCapture::info().format` must equal the effective format. A mismatch aborts startup rather than allowing the network path to use the wrong geometry.

## 3.1 Server zero-argument defaults

`ServerRuntimeConfig{}` is intentionally usable as the server baseline: gRPC `0.0.0.0:50051`, UDP `0.0.0.0:9999`, OUTPUT loopback on the system default render endpoint, backend-derived default capture format, and automatic F from the UDP payload budget. CLI and Core Runtime must preserve these same defaults.

## 4. Client composition

ClientRuntime obtains its authoritative format/F from ConnectResponse, then constructs JitterBuffer and starts playback using that exact format. The playback endpoint is independently selectable and defaults to the platform default output device.

## 4.1 Application defaults

The CLI contract intentionally keeps the normal path small:

```text
server: no arguments required
client: --server-ip <IP> --server-rpc <PORT>
```

All other application settings have local defaults. Device selection and diagnostics are opt-in.

## 5. Device model

```text
INPUT  -> microphone / capture endpoint
OUTPUT -> render endpoint
```

Loopback is not a third device category. It is a capture mode using an OUTPUT endpoint:

```text
AudioCaptureSource::OUTPUT_LOOPBACK
        + AudioDeviceDirection::OUTPUT
```

`--list-devices` discovers active endpoints. On Server, `--device-id` alone selects an OUTPUT endpoint for loopback; `--capture=input --device-id <ID>` selects an INPUT endpoint. On Client, `--device-id` selects an OUTPUT playback endpoint. `nullopt` means platform default.

## 6. Address model

Server has three independent address concepts:

```text
rpc_bind_ip          local gRPC listener address
udp_bind_ip          local UDP listener address
advertised_udp_address  endpoint sent to clients
```

`rpc_bind_ip` and `udp_bind_ip` may be wildcard (`0.0.0.0` / `::`). `advertised_udp_address` may also be wildcard as a sentinel meaning “do not override the route discovered through the control plane”. Client then uses the IP supplied to `connect_to_server()`.

If `advertised_udp_address` is not explicitly supplied, Runtime derives it from `udp_bind_ip`. An explicit advertised address always wins. This supports multi-NIC hosts, NAT/front-end deployment and special routing.

## 7. Audio data lifecycle

### Server

`AudioCapture` produces borrowed `AudioBlock` spans. `AudioPacketizer` performs a bounded copy into preallocated storage and emits fixed-size `AudioFrame`s. `AudioFrameQueue` is the only RT→worker handoff. The dispatcher encodes and calls `UdpServer::broadcast()` from its worker thread.

### Client

`UdpClient` validates source and wire shape, then copies/accepts frame payload into JitterBuffer. JitterBuffer is the only application-layer playback buffer. It owns the sequence timeline, startup pre-roll, missing-frame silence and Fill/Drop correction.

## 8. Runtime lifecycle

Both runtimes are one-shot:

```text
Created -> Starting -> Running / Degraded -> Stopping -> Stopped
```

`start()` and `stop()` are lifecycle-serialized with `lifecycle_mutex_`. Concurrent `stop()` calls are safe. A `stop()` racing a `start()` waits for startup's lifecycle critical section and then performs teardown. No restart is supported after `Stopped`.

## 9. Realtime boundary

Capture callback and playback callback are hard realtime-facing application boundaries. They must not perform:

- locks;
- allocation;
- synchronous logging/I/O;
- blocking waits;
- executor submission.

JitterBuffer diagnostics are therefore normally counters only. The compile-time `AQUA_JITTER_BUFFER_RT_DEBUG_LOG` option intentionally violates this rule for offline diagnosis and must stay off in performance/release builds.

## 10. Concurrency map

```text
Capture RT thread
    └── AudioPacketizer.push
          └── AudioFrameQueue.push

Dispatcher worker
    └── AudioFrameQueue.consume
          └── NetworkFrame.encode
          └── UdpServer.broadcast

UDP / gRPC async handlers
    └── strand / gRPC runtime ownership

Playback RT thread
    └── JitterBuffer.pull

Runtime control thread
    └── startup / shutdown / diagnostics / CLI orchestration
```

## 11. Failure domains

| Failure | Immediate response | Owner |
|---|---|---|
| invalid CLI/config | reject before run | CLI/core |
| capture start failure | startup abort | ServerRuntime |
| playback start failure | startup abort | ClientRuntime |
| malformed UDP | drop + counter | UdpClient/UdpServer |
| lost frame | playback silence | JitterBuffer |
| HELLO timeout | Client `Degraded` | ClientRuntime |
| backend runtime error | Runtime `Degraded` | Runtime control path |
| gRPC startup failure | startup abort | ServerRuntime |

## 12. Current non-goals

No authentication token, TLS, public-internet trust model, automatic resampling, automatic device following or complete non-Windows audio backend is part of the current implementation baseline.
