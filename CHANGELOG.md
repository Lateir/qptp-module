# Changelog

## v3.0

- Send controller connection, battery, charging, tracking and device metadata in a status message about once per second.
- Send thumb-rest sensor samples when values change, plus a heartbeat about every 200 ms while unchanged.
- Preserve haptic commands and both USB and LAN transports on the same connection.

## v2.3

- Accept the existing sensor and haptic protocol over the local network as well as ADB forwarding.
- Answer UDP discovery queries on port 27183 with the TCP service port.

## v2.2

- Add bidirectional commands and acknowledgements to the existing TCP stream.
- Trigger a timed pulse on the hand, index, or thumb haptic channel of either Touch Pro controller.
- Keep the 200 Hz sensor stream running while a haptic command executes.

## v2.1

- Stream thumb-rest X, Y and force for both Touch Pro controllers over USB ADB forwarding.
- Sample controller state at 200 Hz with a device monotonic timestamp.
- Provide Magisk update metadata through this repository.
