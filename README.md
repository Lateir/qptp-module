# Quest Pro Touch Plus

Magisk module for reading the Touch Pro thumb-rest X, Y and force fields from both controllers. The device listens only on `127.0.0.1:27182`; a PC connects over USB with `adb forward tcp:27182 tcp:27182`. Each sample is a 32-byte `QPR2` frame containing a sequence number, a monotonic timestamp and `<HHfHHf>` values (left X/Y/force, right X/Y/force). The module reads `trackingservice` memory and does not write to it.

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
