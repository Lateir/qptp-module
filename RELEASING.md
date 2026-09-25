# Releasing a module update

1. Increase `versionCode` in `module.prop`; also change its `version` (for example, `v2.2` with code `4`). Keep `id` and `updateJson` unchanged.
2. Update `CHANGELOG.md`. If `src/streamer.c` changed, rebuild `bin/qpro_streamer` with the Zig command in `README.md`.
3. Run `python build_module.py`. The existing `update.json` may still advertise the previous release at this point. Inspect the ZIP before publishing.
4. Commit and push the module changes. Create a release with a matching tag (for example, `v2.2`) and upload **`qpro_touch_stream_magisk.zip`** as its asset. GitHub's automatic source archives are not installable modules.
5. Verify that the release asset URL downloads the ZIP. Then change `update.json` to the new `version`, `versionCode`, and release `zipUrl`; commit and push it to `main`. Magisk will then see the new release.

The changelog URL remains `https://raw.githubusercontent.com/Lateir/qptp-module/main/CHANGELOG.md`. GitHub may cache raw files briefly. Never advertise a release in `update.json` before its ZIP is publicly available.
