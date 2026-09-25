# Quest Pro Touch Plus

Magisk module for reading the Touch Pro thumb-rest X, Y and force fields from both controllers and sending haptic commands. It reads `trackingservice` memory without writing to it.

## Connection and discovery

The module listens on TCP port **27182** on all Quest network interfaces. Connect directly over a local network, or use USB with `adb forward tcp:27182 tcp:27182`. Both transports use the same bidirectional TCP protocol. The service has no pairing or authentication; use it on a trusted local network.

For automatic discovery, broadcast the four ASCII bytes `QPD1` to UDP port **27183**. The Quest sends an eight-byte response to the sender:

| Offset | Size | Type | Value |
| ---: | ---: | --- | --- |
| 0 | 4 | ASCII | `QPO1` |
| 4 | 2 | uint16 LE | TCP service port (`27182` by default) |
| 6 | 2 | uint16 LE | Discovery protocol version (`1`) |

If the network blocks broadcasts, connect directly to the Quest IP address on TCP port 27182.

## TCP protocol

All integers and floats are **little-endian**. TCP is a byte stream: read the first four bytes to identify a frame, then read the remaining bytes for that frame. Sensor frames and command replies can arrive interleaved. The module sends sensor frames at 200 Hz while it processes commands received on the same connection.

### Sensor sample: `QPR2` (Quest → client, 32 bytes)

| Offset | Size | Type | Value |
| ---: | ---: | --- | --- |
| 0 | 4 | ASCII | `QPR2` |
| 4 | 4 | uint32 | Sample sequence number, starting at 0 for each connection |
| 8 | 8 | uint64 | Device monotonic timestamp, nanoseconds |
| 16 | 2 | uint16 | Left thumb-rest X |
| 18 | 2 | uint16 | Left thumb-rest Y |
| 20 | 4 | float32 | Left thumb-rest force |
| 24 | 2 | uint16 | Right thumb-rest X |
| 26 | 2 | uint16 | Right thumb-rest Y |
| 28 | 4 | float32 | Right thumb-rest force |

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

### Haptic reply: `QPA1` (Quest → client, 16 bytes)

| Offset | Size | Type | Value |
| ---: | ---: | --- | --- |
| 0 | 4 | ASCII | `QPA1` |
| 4 | 4 | uint32 | Request ID from the command |
| 8 | 4 | int32 | Status code (below) |
| 12 | 4 | uint32 | Reserved, always `0` |

| Status | Meaning |
| ---: | --- |
| 0 | Haptic CLI commands completed |
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
