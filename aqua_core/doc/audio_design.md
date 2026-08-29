# Audio Design

## 1. Domain types

### AudioFormat

```text
encoding + channels + sample_rate
```

It describes PCM representation only. Supported encodings are S16LE, S24LE, S32LE, F32LE and U8.

### AudioBlock

Borrowed, variable-length PCM produced by capture. It contains no sequence and no ownership.

### AudioFrame

Fixed transport unit:

```text
sequence
frame_count = F
data.size() = F × frame_bytes
```

### NetworkFrame

Wire representation containing the UDP protocol header plus payload. Network serialization is explicit little-endian.

## 2. Capture

`AudioCapture` is push-based. The backend owns the realtime thread and invokes:

```cpp
AudioCaptureCallback(AudioBlock)
```

`AudioCaptureConfig` includes:

- `source`: INPUT_DEVICE or OUTPUT_LOOPBACK;
- optional device id;
- optional format;
- requested buffer size.

When format is absent the backend selects the platform shared/default stream format and reports it through `AudioCaptureInfo::format`.

## 3. Playback

`AudioPlayback` is pull-based. The backend asks the application for the next output buffer:

```cpp
AudioPlaybackCallback(span<byte>) -> frames_filled
```

Client passes the format received from Server unchanged. Playback device selection is independent from network format.

## 4. Device selection

`AudioDeviceManager` provides:

```text
enumerate(direction)
default_device(direction)
resolve(direction, optional<id>)
default_format(direction, optional<id>)
```

The last operation is a backend capability query used when Server format is unspecified.

## 5. Server default-format flow

```text
CLI omitted encoding/channels/rate
          │
          ▼
ServerRuntimeConfig.format = nullopt
          │
          ▼
resolve selected capture endpoint
          │
          ▼
AudioDeviceManager::default_format()
          │
          ▼
backend shared-mode/default stream format
          │
          ▼
effective AudioFormat
          │
          ├──> packetizer / queue geometry
          ├──> gRPC ConnectResponse
          └──> capture startup expectation
```

The server therefore no longer assumes F32/48 kHz as an implementation default.

## 6. Explicit-format flow

The user must provide all three options together:

```text
--encoding
--channels
--sample-rate
```

Partial overrides are rejected. This prevents ambiguous configurations such as “keep backend sample rate but force F32”. If explicit format is unsupported by the backend, startup fails with `FormatUnsupported`.

## 7. Frame size

For automatic F:

```text
F = floor(1443 / frame_bytes)
```

For explicit F:

```text
F >= 16
F × frame_bytes <= 1443
```

`1443` is the IPv6-safe payload budget derived from a 1500-byte MTU.

## 8. No hidden conversion

Current implementation deliberately does not perform automatic resampling or channel mixing. A format mismatch is an explicit failure. This keeps sequence timing, payload size and playback interpretation deterministic.

## 9. Backend rules

Windows/WASAPI currently provides:

- INPUT device capture;
- OUTPUT loopback capture;
- OUTPUT playback;
- active endpoint enumeration;
- endpoint default/shared format query.

Future backends must preserve the domain semantics even when a platform lacks a capability. Unsupported features should return `NotSupported` rather than inventing a fake device or format.
