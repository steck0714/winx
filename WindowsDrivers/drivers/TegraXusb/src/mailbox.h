/* SPDX-License-Identifier: GPL-2.0-only
 * Switchroot xhci-tegra.c, tegra_xusb_mbox_handle. T210B01 has a fixed SS clock. */
#ifndef SWXUSB_MAILBOX_H
#define SWXUSB_MAILBOX_H
enum {
    SwUsbEnableMessages = 1,
    SwUsbIncFalcon = 2,
    SwUsbDecFalcon = 3,
    SwUsbIncSs = 4,
    SwUsbDecSs = 5,
    SwUsbBandwidth = 6,
    SwUsbSaveDfe = 9,
    SwUsbHsicStart = 12,
    SwUsbHsicStop = 13,
    SwUsbLfpsOff = 17,
    SwUsbLfpsOn = 18,
    SwUsbAck = 128,
    SwUsbNak = 129,
    SwUsbFalconKhz = 204000
};
typedef struct {
    unsigned command, data;
    int lfps;
} SWUSB_REPLY;
static inline SWUSB_REPLY SwUsbReply(unsigned word)
{
    unsigned command = word >> 24, data = word & 0xffffff;
    SWUSB_REPLY r = {SwUsbNak, data, 0};
    switch (command) {
    case SwUsbBandwidth:
    case SwUsbAck:
    case SwUsbNak:
        r.command = 0;
        break;
    case SwUsbIncFalcon:
    case SwUsbDecFalcon:
        r.command = data == SwUsbFalconKhz ? SwUsbAck : SwUsbNak;
        r.data = SwUsbFalconKhz;
        break;
    case SwUsbIncSs:
    case SwUsbDecSs:
        r.command = SwUsbAck;
        break;
    case SwUsbHsicStart:
    case SwUsbHsicStop:
        if (!(data & (1u << 9)))
            r.command = SwUsbAck;
        break;
    case SwUsbLfpsOff:
    case SwUsbLfpsOn:
        if (!((data >> 1) & 0xEu)) {
            r.command = SwUsbAck;
            if (data & 2u)
                r.lfps = command == SwUsbLfpsOn ? 1 : -1;
        }
        break;
    }
    return r;
}
#endif
