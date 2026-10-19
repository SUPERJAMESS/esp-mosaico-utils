# macOS Iris descriptor capture

This fixture preserves the USB interface and serial driver hierarchy captured
with `ioreg -a -l -p IOService -r -c IOUSBHostDevice` during the 2026-10-18
macOS recovery failure. It includes the repeated VID/PID fields on
IOUSBHostInterface and AppleUSBACMData, and BSD nodes below data interfaces 1/3.
The owning firmware declares named CDC control interfaces 0/2.

Only the Iris device subtree and descriptor/serial association fields are kept.
BSD serial names are anonymized; unrelated devices and properties are removed.
The fixture is intentionally independent of an attached board or consumer workspace.
