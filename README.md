# Quest Pro Touch Plus

Magisk module for reading the Touch Pro thumb-rest X, Y and force fields from both controllers and sending haptic commands. It reads `trackingservice` memory without writing to it.

## Connection and discovery

The module listens on TCP port **27182** on all Quest network interfaces. Connect directly over a local network, or use USB with `adb forward tcp:27182 tcp:27182`. Both transports use the same bidirectional TCP protocol. Sends have a two-second deadline, and TCP unacknowledged data has a five-second timeout. A failed session shuts down both socket directions and returns to accepting connections; no application protocol change is required. The service has no pairing or authentication; use it on a trusted local network.

For automatic discovery, broadcast the four ASCII bytes `QPD1` to UDP port **27183**. The Quest sends an eight-byte response to the sender:

| Offset | Size | Type | Value |
| ---: | ---: | --- | --- |
| 0 | 4 | ASCII | `QPO1` |
| 4 | 2 | uint16 LE | TCP service port (`27182` by default) |
| 6 | 2 | uint16 LE | Discovery protocol version (`1`) |

If the network blocks broadcasts, connect directly to the Quest IP address on TCP port 27182.

## TCP protocol

All integers and floats are **little-endian**. TCP is a byte stream: read the first four bytes to identify a frame, then read the remaining bytes for that frame. Sensor samples and command replies can arrive interleaved. The module polls sensor fields at 200 Hz and sends a sample when any field changes, plus a heartbeat about every 200 ms while values are unchanged. Haptic commands use the same connection.

The module sends controller status once every five seconds. It reads connection and battery from Meta's `trackinginterface_cli ls`, and tracking flags from `trackinginterface_cli getcontrollertrackingdata 0`, without a full `dumpsys tracking`. Charging still comes from `dumpsys OVRRemoteService`. When starting the binary manually, pass `27182 200 0 1` (port, sample rate, unlimited frames, status enabled); omitting the fifth argument disables status messages.

### Optional controller status: `QPS1` (Quest → client, variable length)

| Offset | Size | Type | Value |
| ---: | ---: | --- | --- |
| 0 | 4 | ASCII | `QPS1` |
| 4 | 4 | uint32 | Length of the JSON payload in bytes |
| 8 | Specified above | UTF-8 JSON | Object with `left` and `right` controller entries |

Each controller entry contains:

| Field | Type | Meaning |
| --- | --- | --- |
| `connected` | boolean | Controller appears in `trackinginterface_cli ls` |
| `battery_percent` | integer or null | Battery level, `0..100`; null if unknown |
| `charging` | boolean or null | Charging state from `dumpsys OVRRemoteService`; null if unknown |
| `tracked` | boolean | Position is both tracked and valid according to Meta's controller tracking API |
| `device_id` | string, optional | Controller ID when connected |
| `controller_type` | string, optional | Model from `trackinginterface_cli ls` |
| `serial` | string, optional | Serial from `trackinginterface_cli ls`, when present |
| `tracking_level` | string, optional | `6DOF`, `3DOF`, or `Invalid`, derived from the API's position and orientation flags |

A missing controller is represented with `connected: false`, unknown battery and charging, and `tracked: false`.

### Module version: `QPV1` (Quest to client, variable length)

Sent immediately after each TCP connection. An 8-byte header contains `QPV1` and a little-endian uint32 JSON length, followed by UTF-8 JSON with `version` and `versionCode` from the installed `module.prop`.

### Controller sensor sample: `QPR3` (Quest to client, 56 bytes)

| Offset | Size | Type | Value |
| ---: | ---: | --- | --- |
| 0 | 4 | ASCII | `QPR3` |
| 4 | 4 | uint32 | Sample sequence number |
| 8 | 8 | uint64 | Monotonic timestamp in nanoseconds |
| 16 | 20 | mixed | Left controller values |
| 36 | 20 | mixed | Right controller values |

Each controller block contains little-endian `u16` X and Y, followed by `f32` thumb-rest force, rear stylus sensor, trigger finger proximity, and lateral trigger slide. Their scale and polarity are not yet characterized.

QPR3 is the default format on every new connection. A client can switch to QPR4 with `QPC1` opcode `2`.

### Extended controller sample: `QPR4` (Quest to client, 72 bytes, opt-in)

| Offset | Size | Type | Value |
| ---: | ---: | --- | --- |
| 0 | 4 | ASCII | `QPR4` |
| 4 | 4 | uint32 | Sample sequence number |
| 8 | 8 | uint64 | Monotonic timestamp in nanoseconds |
| 16 | 28 | mixed | Left controller values |
| 44 | 28 | mixed | Right controller values |

Each 28-byte controller block:

| Offset | Size | Type | Value |
| ---: | ---: | --- | --- |
| 0 | 2 | uint16 | Thumb-rest X |
| 2 | 2 | uint16 | Thumb-rest Y |
| 4 | 4 | float32 | Thumb-rest force |
| 8 | 4 | float32 | Thumb-rest contact size (area) |
| 12 | 4 | float32 | Rear stylus sensor |
| 16 | 4 | float32 | Trigger finger proximity (curl) |
| 20 | 4 | float32 | Trigger slide (position) |
| 24 | 1 | uint8 | Thumb-rest state; bit 0 = touching, other bits are passed through unchanged |
| 25 | 3 | — | Zero |

A change in any field, including size and state, sends a sample.

### Sensor source

Thumb-rest and trigger values are double-buffered in each `TS_CONTROLLER_*` shared-memory page. The module reads a `u32` counter, then the record selected by `counter & 1`, then the counter again, and retries up to four times if the counter changed. If every attempt races the writer, that record keeps its previous values for the sample.

| Record | Counter | Record address | Fields |
| --- | ---: | --- | --- |
| Thumb rest | `0x1e8` | `0x1f0 + 16 * (counter & 1)` | X `u16`, Y `u16`, force `f32`, size `f32`, state `u8` at +12 |
| Trigger | `0x210` | `0x218 + 0x24 * (counter & 1)` | proximity (curl) `f32` at +0, slide (position) `f32` at +4 |

The rear stylus sensor is read directly from `0x110`.

Each rear stylus value is sent unchanged while it is moving. If its raw value stays at the same intermediate level for 50 ms, the streamer sends 0 below 0.5 or 1 at and above 0.5 until the raw value changes again. The two controllers are tracked independently.

The module waits for a TCP client before opening or sampling trackingservice memory. LAN discovery still responds while waiting.

### Haptic command: `QPC1` (client → Quest, 16 bytes)

| Offset | Size | Type | Value |
| ---: | ---: | --- | --- |
| 0 | 4 | ASCII | `QPC1` |
| 4 | 1 | uint8 | Opcode: `1` = set haptic channel |
| 5 | 1 | uint8 | Controller: `0` left, `1` right |
| 6 | 1 | uint8 | Channel: `0` hand, `1` index, `2` thumb |
| 7 | 1 | uint8 | Amplitude: `0..255` |
| 8 | 2 | uint16 | Duration in milliseconds: `1..5000` for a pulse |
| 10 | 4 | uint32 | Request ID, echoed in the reply |
| 14 | 2 | uint16 | Reserved, must be `0` |

Amplitude `0` with duration `0` stops the selected channel. A nonzero amplitude requires a duration of 1–5000 ms. Commands on one connection are processed in order.

### Frame format command: `QPC1` opcode `2` (client → Quest, 16 bytes)

Available from module `versionCode` 12.

| Offset | Size | Type | Value |
| ---: | ---: | --- | --- |
| 0 | 4 | ASCII | `QPC1` |
| 4 | 1 | uint8 | Opcode: `2` = set sample frame format |
| 5 | 1 | uint8 | Format: `3` = QPR3, `4` = QPR4 |
| 6 | 1 | uint8 | Must be `0` |
| 7 | 1 | uint8 | Must be `0` |
| 8 | 2 | uint16 | Must be `0` |
| 10 | 4 | uint32 | Request ID, echoed in the reply |
| 14 | 2 | uint16 | Reserved, must be `0` |

The module replies with `QPA1` status `0`, or `1` for an invalid command, which leaves the format unchanged. Every sample sent after a status `0` reply uses the new format; samples before it use the old one. The format returns to QPR3 on each new connection. Older modules reply `1` to opcode `2` and keep the connection open.

### Haptic reply: `QPA1` (Quest → client, 16 bytes)

| Offset | Size | Type | Value |
| ---: | ---: | --- | --- |
| 0 | 4 | ASCII | `QPA1` |
| 4 | 4 | uint32 | Request ID from the command |
| 8 | 4 | int32 | Status code (below) |
| 12 | 4 | uint32 | Reserved, always `0` |

| Status | Meaning |
| ---: | --- |
| 0 | Haptic CLI commands completed, or frame format changed |
| 1 | Invalid command |
| 2 | Controller not found |
| 3 | Start command failed |
| 4 | Stop command failed |

For a pulse, the reply arrives after the module sends the stop command. Status `0` does not independently verify physical vibration.

## Install

Download **`qptp-magisk.zip`** from the [latest release](https://github.com/Lateir/qptp-module/releases/latest), install it through Magisk, and reboot. The GitHub-generated source archives are not installable Magisk modules.

This repository contains the module files and update metadata. The ZIP is published as a Release asset. `module.prop` points Magisk to the public [update.json](https://raw.githubusercontent.com/Lateir/qptp-module/main/update.json); each release increments `versionCode` and updates the JSON after its ZIP has been published.

## Build

The included `bin/qpro_streamer` is a static aarch64 Linux executable. To rebuild it from source with Zig 0.15.2:

```powershell
zig cc -target aarch64-linux-musl -O2 -static -s .\src\streamer.c -o .\bin\qpro_streamer
python .\build_module.py
```

`build_module.py` validates the version ordering and creates the installable ZIP. It keeps Magisk scripts as LF text and marks the native executable through `customize.sh`. GitHub Actions rebuilds the binary and ZIP for each release tag. Follow [RELEASING.md](RELEASING.md) when publishing the next version.

## License

This project's original code is available under [0BSD](LICENSE).
