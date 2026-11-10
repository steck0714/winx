#include "../src/mailbox.h"
#include <assert.h>
int main(void)
{
    SWUSB_REPLY r = SwUsbReply((SwUsbIncFalcon << 24) | 204000);
    assert(r.command == SwUsbAck && r.data == 204000);
    r = SwUsbReply(SwUsbDecFalcon << 24);
    assert(r.command == SwUsbNak && r.data == 204000);
    r = SwUsbReply((SwUsbLfpsOff << 24) | 2);
    assert(r.command == SwUsbAck && r.lfps == -1);
    r = SwUsbReply((SwUsbLfpsOn << 24) | 2);
    assert(r.command == SwUsbAck && r.lfps == 1);
    r = SwUsbReply((SwUsbLfpsOn << 24) | 4);
    assert(r.command == SwUsbNak && r.lfps == 0);
    r = SwUsbReply(SwUsbBandwidth << 24);
    assert(!r.command);
    r = SwUsbReply((unsigned)SwUsbAck << 24);
    assert(!r.command);
    r = SwUsbReply((SwUsbIncSs << 24) | 120000);
    assert(r.command == SwUsbAck && r.data == 120000);
    r = SwUsbReply(SwUsbSaveDfe << 24);
    assert(r.command == SwUsbNak);
    return 0;
}
