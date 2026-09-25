# Changelog

## v2.2

- Add bidirectional commands and acknowledgements to the existing TCP stream.
- Trigger a timed pulse on the hand, index, or thumb haptic channel of either Touch Pro controller.
- Keep the 200 Hz sensor stream running while a haptic command executes.

## v2.1

- Stream thumb-rest X, Y and force for both Touch Pro controllers over USB ADB forwarding.
- Sample controller state at 200 Hz with a device monotonic timestamp.
- Provide Magisk update metadata through this repository.
