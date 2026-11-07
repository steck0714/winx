# Switch Lite built-in controls

`switchsio` 0.2.1.0 reads the built-in HDH-001 controls over UART-C. It exposes separate HID gamepad and relative mouse collections through Windows VHF. The original gamepad report, HOME input and accelerometer data remain available.

Games that accept generic HID or DirectInput controllers can read the gamepad collection. XInput-only titles require an Xbox-compatible input path. Tap **Capture** (the square button below the left stick) before playing to turn off all desktop pointer movement and clicks. Gamepad reports keep flowing. Tap Capture again to restore desktop controls. [Microsoft controller API comparison](https://learn.microsoft.com/en-us/windows/win32/xinput/xinput-and-directinput)

| Lite input | Windows desktop action |
| --- | --- |
| Left stick | Move the pointer. Centering the stick stops movement. |
| ZR | Hold or release the left mouse button, including dragging. |
| ZL | Hold or release the right mouse button. |
| Capture | Toggle desktop mouse mapping once per press. |

The mapping follows Switchroot **L4T Ubuntu**. Its Lite serial driver decodes ZR at raw button bit 7, ZL at bit 23 and the left stick as two packed 12-bit axes. Windows uses the same axis directions. The mouse path has a center dead zone and keeps fractional movement for small deflections. Turning the mapping off releases both mouse buttons, including an active drag. A trigger held when mapping returns must be released before the next click. The mode survives UART reconnection and suspend. A fresh driver instance starts with mouse mapping enabled. [Ubuntu desktop mapping](https://wiki.switchroot.org/wiki/linux/linux-features#joycon-mousekeyboard-mapping) · [Switchroot Lite SIO input code](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/input/joystick/joycon-serdev.c#L1444-L1503)

Use the ARM64 `switchsio` package with the Lite firmware's `SWITCH_LITE_SIO=1` profile (`ACPI\SWOA0902`). Use `Install-LiteSystem.cmd online input` or `Install-LiteSystem.cmd offline W:\ input` for a controller-only update. For the combined USB-C and input kit, use `usb-input`. [Shared installer and signing](../TegraXusb/README.md#package-and-install)

The original signed 0.2.0.0 package and SIO-enabled firmware were staged on the card. Version 0.2.1.0 adds the Capture toggle using the same firmware. Host report tests cover repeated motion, button transitions, drag release, a held Capture across suspend and re-enabling with ZR held. Physical input on Windows remains to be confirmed.
