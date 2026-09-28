# Changelog

## v3.4

- Settle each rear stylus sensor to 0 or 1 after its raw value stays unchanged for 50 ms, using 0.5 as the midpoint. Resume raw values immediately when the sensor changes.

## v3.3

- Stream the rear stylus, trigger finger proximity, and trigger slide values for both Touch Pro controllers in QPR3 frames.
- Send the installed module version and versionCode immediately after each TCP connection.
- Wait for a client before opening or sampling trackingservice memory.

## v3.2

- Restore controller status every five seconds through Meta's tracking interface instead of repeatedly running `dumpsys tracking`.
- Report connection and battery from the device list, and position tracking from controller tracking flags. Charging remains sourced from `OVRRemoteService`.
- Keep the 200 Hz thumb-rest stream and haptic commands unchanged.

## v3.1

- Stop periodic controller-status queries by default to avoid repeatedly dumping `trackingservice` while streaming.
- Keep the 200 Hz thumb-rest stream and haptic commands active. Controller connection, battery, charging and tracking status are unavailable in the default stream.
- Allow the legacy status messages when starting `qpro_streamer` with a fifth argument of `1`.

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
