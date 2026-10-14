# app_device

Samples battery, IMU, USB enumeration, and physical AI/Boot keys outside the
render task. Applies backlight, sleep networking, rotation and motor requests.
Sensor failures are exposed explicitly; a missing optional sensor does not stop
USB recovery. The board task samples sensors; a serialized timer drives short
haptic pulses without blocking the render task.

UI calls `vibe_device_policy` to inhibit rotation during touch, text input, or
critical updates. Rotation uses the BSP's coupled panel/touch transform. Keys
use 40 ms debounce and a bounded queue; the power key belongs to the power circuit
and is not faked. The display adapter pauses rendering and drains presentation
before applying the BSP's panel/touch rotation, then requests a full redraw.
