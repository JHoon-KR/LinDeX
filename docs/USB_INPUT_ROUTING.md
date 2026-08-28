# USB input routing

[English](USB_INPUT_ROUTING.md) | [한국어](ko/USB_INPUT_ROUTING.md)

## Goal

While an external-monitor Debian session is active, LinDeX can make a USB-dock
keyboard, mouse, and indirect touchpad exclusive to that session. Android and
Debian continue to share unrelated USB functions such as storage, audio,
network, serial, and capture devices. The phone's built-in touchscreen is not
grabbed by this USB mode.

## WebUI modes

- `linux-exclusive` (recommended for a dock desktop): eligible USB input event
  nodes are grabbed only for the active monitor session.
- `shared`: Android and Debian may both receive events.
- **Phone touch sharing** is a separate option and remains off by default.

## Ownership model

The input helper is loaded only into the owned compositor/seat process. It
classifies the opened file descriptor rather than trusting its pathname:

1. it must be a Linux input character device;
2. udev or sysfs ancestry must identify a USB bus device;
3. event capabilities must identify a keyboard, relative mouse, or indirect
   touchpad; and
4. direct touchscreens and unrelated event nodes are excluded.

The helper applies `EVIOCGRAB` to that exact descriptor. When the compositor
ends and the final duplicate closes, the kernel releases the grab. LinDeX does
not use a global input daemon, broad PID search, Android reboot, or persistent
ownership record for this lifetime.

If the grab is denied, the desktop keeps the descriptor open and leaves input
shared so recovery remains possible. Dev diagnostics go to the bounded session
stream; release mode creates no input log file.

## Safety boundary

- Do not preload the helper globally into the Debian environment.
- Do not attach it to a shared Android or global seat daemon.
- Bluetooth input is outside the USB-only classification.
- USB touchscreens are not treated as indirect touchpads.
- Device enumeration and USB power remain shared; the feature controls event
  delivery only.

## Final dock matrix

The feature remains pending final device-matrix status until the packaged
module passes all of these checks with the real dock:

1. shared mode delivers expected events to both environments;
2. exclusive mode keeps keyboard, mouse, and indirect touchpad in Debian;
3. storage, audio, network, and other USB functions continue working;
4. the phone touchscreen continues controlling Android;
5. normal session stop immediately returns USB input to Android;
6. forced DP removal also releases the owned descriptors; and
7. reconnect creates a fresh session without a stale grab.

Record the result in [Validation status](VALIDATION_STATUS.md). Do not claim
generic dock validation from host-only tests.
