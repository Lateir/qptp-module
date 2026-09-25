# Releasing a module update

1. Increase `versionCode` in `module.prop` and change `version` to the next tag (for example, `v2.2` and `4`). Keep `id` and `updateJson` unchanged.
2. Update `CHANGELOG.md`, commit the changes, and push `main`. GitHub Actions rebuilds the binary from `src/streamer.c`.
3. Optionally run **Build and release Magisk module** from the Actions tab with **Run workflow** on `main`. This produces a downloadable test artifact without publishing a release or changing `update.json`.
4. Create and push the matching tag from the committed `main` state (for example, `git tag v2.2` and `git push origin v2.2`).

The tag run builds `qptp-magisk.zip`, publishes it as a GitHub Release asset, then commits the new `update.json` to `main`. The repository must allow GitHub Actions to write contents to `main`. If the manifest push fails, resolve it before creating another release. GitHub may cache raw files briefly.
