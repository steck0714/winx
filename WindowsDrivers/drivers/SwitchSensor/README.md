# Switch Lite built-in controls

`switchsio` reads the built-in HDH-001 controls over UART-C. Windows VHF exposes a HID gamepad, mouse and keyboard. The controls below follow Switchroot L4T Ubuntu. [Ubuntu mapping](https://wiki.switchroot.org/wiki/linux/linux-features#joycon-mousekeyboard-mapping) · [Switchroot mapping notes](https://wiki.switchroot.org/wiki/linux/linux-changelog)

| Desktop input | Windows action |
| --- | --- |
| Left stick | Move the pointer. |
| Right stick | Scroll up/down or left/right. Small deflections scroll slowly, a full tilt scrolls faster and centering stops scrolling. Horizontal scrolling depends on the application. |
| Press right stick (R3) | Hold or release the middle mouse button. |
| Left D-pad | Send the four keyboard arrow keys. Hold to repeat at the Windows keyboard rate. Diagonals send both keys. |
| ZR / ZL | Hold or release the left / right mouse button. |
| + (Plus) | Open the Windows On-Screen Keyboard. |
| Capture (square button) | Toggle desktop mappings and gamepad-only mode. |

The gamepad reports both sticks, the D-pad hat and the buttons in either mode. In gamepad-only mode, games receive Plus as Start and R3 as the right thumb button. Games supporting generic HID or DirectInput can read these controls. XInput-only games require an Xbox-compatible input device. [Microsoft controller API comparison](https://learn.microsoft.com/en-us/windows/win32/xinput/xinput-and-directinput)

The right stick sends relative HID Wheel and AC Pan detents. Its dead zone is 5000 of 32767 per axis. Fractional detents accumulate while tilted and clear on centering or a direction change. Scroll speed follows the X.Org joystick driver's default relative-scroll curve, sampled in fixed point. [X.Org scroll calculation](https://gitlab.freedesktop.org/xorg/driver/xf86-input-joystick/-/blob/master/src/jstk_axis.c) · [Switchroot Lite stick orientation](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/2d0059fd3167a8df756de2aa0489d4aa70a9fc15/drivers/input/joystick/joycon-serdev.c#L1444-L1503)

Capture releases held desktop keys and mouse buttons. Press them again after restoring desktop mode or reconnecting the controller. Each new Plus press sends one Ctrl + Windows + O tap. Held arrows are released before the shortcut so its modifiers cannot turn them into Windows desktop-switching commands. The mode survives suspend and UART reconnection. A fresh driver starts in desktop mode. [Windows accessibility shortcuts](https://support.microsoft.com/en-us/accessibility/windows/windows-keyboard-shortcuts-for-accessibility)

Use the ARM64 package with the existing Lite SIO firmware (`SWITCH_LITE_SIO=1`, `ACPI\SWOA0902`). To update the controller alone, use `Install-LiteSystem.cmd online input` in an elevated command prompt or `Install-LiteSystem.cmd offline W:\ input` from Windows PE. [Shared installer and signing](../TegraXusb/README.md#package-and-install)

Windows enumerated the gamepad and mouse and received SIO input on this Lite. Version 0.2.2.0 with the Plus shortcut was installed on the test card. The 0.2.3.0 wheel, middle button and D-pad keyboard mappings passed host report tests and were built for ARM64. Physical desktop navigation and in-game input await confirmation.
