# app_device

Samples battery, IMU, USB enumeration, and physical AI/Boot keys outside the
render task. Applies brightness, sleep networking, rotation and motor requests.
The control task starts physical key sampling and display preferences immediately.
A temporary worker initializes haptics, IMU, then the battery gauge, publishing
each peripheral under the snapshot lock only when it is usable. Slow battery
profile verification/sealing cannot delay keys or brightness. If the optional
worker cannot start, controls and USB status remain available; sensor availability
and battery validity remain false. The worker frees its task stack on completion.
Sensor failures are exposed explicitly; a missing optional sensor does not stop
USB recovery. The board task samples sensors; a serialized timer drives short
haptic pulses without blocking the render task.

UI calls `vibe_device_policy` to inhibit rotation during touch, text input, or
critical updates. Rotation uses the presenter's coupled panel/touch transform. Keys
use 40 ms debounce and a bounded queue; the power key belongs to the power circuit
and is not faked. The display adapter pauses rendering and drains presentation
before applying presenter rotation, then requests a full redraw.
