# Switch Lite built-in controls

`switchsio` 0.2.2.0 reads the built-in HDH-001 controls over UART-C. Windows VHF exposes a gamepad, a relative mouse and a keyboard collection. The keyboard sends the shortcut for the Windows On-Screen Keyboard.

Games with generic HID or DirectInput support can read the gamepad in either mode. XInput-only games require an Xbox-compatible input device. [Microsoft controller API comparison](https://learn.microsoft.com/en-us/windows/win32/xinput/xinput-and-directinput)

| Lite input | Windows desktop action |
| --- | --- |
| Left stick | Move the pointer. Center the stick to stop. |
| ZR | Hold or release the left mouse button, including dragging. |
| ZL | Hold or release the right mouse button. |
| + (Plus) | Open the Windows On-Screen Keyboard with Ctrl + Windows + O. In gamepad-only mode, games receive the usual Plus/Start button. |
| Capture | Switch between desktop controls and gamepad-only mode. Capture is the square button below the left stick. |

The mappings follow Switchroot **L4T Ubuntu**. Its Lite serial driver decodes ZR at raw button bit 7, ZL at bit 23 and the left stick as two packed 12-bit axes. Windows uses the same axis directions, a center dead zone and fractional motion for small deflections. Switching to gamepad-only mode releases any mouse drag. A trigger held when desktop control returns must be released before the next click. [Ubuntu desktop mapping](https://wiki.switchroot.org/wiki/linux/linux-features#joycon-mousekeyboard-mapping) · [Switchroot Lite SIO input](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/input/joystick/joycon-serdev.c#L1444-L1503)

Each new Plus press in desktop mode sends one shortcut tap. Holding Plus during reconnection or while switching modes requires a release and a fresh press. The selected mode persists across suspend and UART reconnection. A fresh driver starts in desktop mode. The trimmed Windows installation on the test Lite includes `osk.exe`. [Microsoft accessibility shortcuts](https://support.microsoft.com/en-us/accessibility/windows/windows-keyboard-shortcuts-for-accessibility)

Use the ARM64 `switchsio` package with the Lite firmware's `SWITCH_LITE_SIO=1` profile (`ACPI\SWOA0902`). Use `Install-LiteSystem.cmd online input` or `Install-LiteSystem.cmd offline W:\ input` for a controller-only update. For the combined USB-C and input kit, use `usb-input`. [Shared installer and signing](../TegraXusb/README.md#package-and-install)

On the Lite, Windows enumerated the gamepad and mouse and the 0.2.0.0 driver recorded incoming SIO input. The card has the signed 0.2.1.0 Capture update. The 0.2.2.0 Plus shortcut passed host report tests and the ARM64 package is signed. Opening the keyboard with the physical button is pending.
