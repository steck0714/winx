/* HDH-001 SIO framing and report regression. Run with a host C11 compiler. */
#include "../src/sio_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned Feed(SIO_STREAM *stream, const uint8_t *bytes, unsigned size)
{
    unsigned frames = 0;
    for (unsigned i = 0; i < size; ++i) frames += (unsigned)SioPush(stream, bytes[i]);
    return frames;
}

static void TestDesktopNavigation(void)
{
    SIO_POINTER_STATE state = {0};
    SIO_PAD pad = {0};
    SIO_MOTION motion;
    SIO_KEYBOARD_REPORT keys;
    uint8_t raw[12] = {0};
    raw[7] = raw[10] = 8;
    raw[8] = raw[11] = 0x80;
    assert(SioInputReport(raw, sizeof(raw), &pad, &motion));
    (void)SioPointerReport(&state, &pad, 8);

    /* Right/up on the Lite's inverted right stick, with R3 and two arrows.
     * The gamepad keeps its original axes, hat and thumb button. */
    raw[4] = 1u << (10 - 8);
    raw[5] = SioUp | SioRight;
    raw[10] = 4; raw[11] = 0xc0; /* raw RX=1024, RY=3072 */
    assert(SioInputReport(raw, sizeof(raw), &pad, &motion));
    assert(pad.axis[2] == 32767 && pad.axis[3] == -32767 && pad.hat == 1);
    assert(pad.buttons == SioPadRightStick);
    int wheel = 0, pan = 0;
    for (unsigned i = 0; i < 125; ++i) {
        SIO_MOUSE_REPORT m = SioPointerReport(&state, &pad, 8);
        assert(m.buttons == 4 && !m.dx && !m.dy);
        wheel += m.wheel; pan += m.pan;
    }
    assert(wheel == 31 && pan == 31);
    assert(SioNextKeyboardReport(&state, &keys) && !keys.modifiers &&
           keys.keys[0] == 0x52 && keys.keys[1] == 0x4f && !keys.keys[2]);
    /* A failed VHF submission is offered again until acknowledged. */
    assert(SioNextKeyboardReport(&state, &keys) && keys.keys[0] == 0x52);
    SioKeyboardReportAccepted(&state);
    assert(!SioNextKeyboardReport(&state, &keys)); /* Windows handles typematic */

    pad.axis[2] = -32767; pad.axis[3] = 32767;
    SIO_MOUSE_REPORT m = SioPointerReport(&state, &pad, 32);
    assert(m.pan == -1 && m.wheel == -1);
    pad.axis[2] = 5000; pad.axis[3] = -4999;
    m = SioPointerReport(&state, &pad, 32);
    assert(!m.pan && !m.wheel && !state.scrollFraction[0] && !state.scrollFraction[1]);

    pad.dpad = SioDown | SioLeft;
    (void)SioPointerReport(&state, &pad, 8);
    assert(SioNextKeyboardReport(&state, &keys) && keys.keys[0] == 0x51 && keys.keys[1] == 0x50);
    SioKeyboardReportAccepted(&state);
    pad.buttons |= SioPadPlus;
    (void)SioPointerReport(&state, &pad, 8);
    assert(SioNextKeyboardReport(&state, &keys) && !keys.modifiers && !keys.keys[0]);
    SioKeyboardReportAccepted(&state);
    assert(SioNextKeyboardReport(&state, &keys) && keys.modifiers == 9 && keys.keys[0] == 0x12 && !keys.keys[1]);
    SioKeyboardReportAccepted(&state);
    assert(SioNextKeyboardReport(&state, &keys) && !keys.modifiers && !keys.keys[0]);
    SioKeyboardReportAccepted(&state);
    assert(!SioNextKeyboardReport(&state, &keys));

    pad.dpad = 0; pad.buttons = 0; (void)SioPointerReport(&state, &pad, 8);
    pad.dpad = SioDown; pad.buttons = SioPadRightStick;
    m = SioPointerReport(&state, &pad, 8);
    assert(m.buttons == 4 && SioNextKeyboardReport(&state, &keys) && keys.keys[0] == 0x51);
    SioKeyboardReportAccepted(&state);
    pad.axis[2] = pad.axis[3] = 32767; pad.buttons |= SioPadCapture;
    m = SioPointerReport(&state, &pad, 32);
    assert(state.disabled && !m.buttons && !m.pan && !m.wheel);
    assert(SioNextKeyboardReport(&state, &keys) && !keys.keys[0]);
    SioKeyboardReportAccepted(&state);
    pad.buttons = SioPadRightStick;
    m = SioPointerReport(&state, &pad, 32);
    assert(!m.buttons && !m.pan && !m.wheel && !SioNextKeyboardReport(&state, &keys));
    pad.buttons |= SioPadCapture;
    m = SioPointerReport(&state, &pad, 32);
    assert(!state.disabled && !m.buttons && !m.pan && !m.wheel && !SioNextKeyboardReport(&state, &keys));
    pad.dpad = SioDown | SioRight; /* only Right is a fresh press */
    (void)SioPointerReport(&state, &pad, 8);
    assert(SioNextKeyboardReport(&state, &keys) && keys.keys[0] == 0x4f && !keys.keys[1]);
    SioKeyboardReportAccepted(&state);
    SioPointerResync(&state);
    assert(SioNextKeyboardReport(&state, &keys) && !keys.keys[0]);
    SioKeyboardReportAccepted(&state);
    m = SioPointerReport(&state, &pad, 32);
    assert(!m.buttons && !m.wheel && !m.pan && !SioNextKeyboardReport(&state, &keys));

    raw[5] = SioDown | SioUp | SioRight | SioLeft;
    assert(SioInputReport(raw, sizeof(raw), &pad, &motion));
    assert(pad.hat == SioNeutralHat && pad.dpad == 15);
}

int main(void)
{
    static const uint8_t commands[][8] = {
        {0x91,0x01,0,0,0,0,0,0x95}, {0x91,0x03,0,0,3,4,0,0xda},
        {0x91,0x41,0,0,0,0,0,0xb0}
    };
    uint8_t command[8];
    for (unsigned i = 0; i < 3; ++i) {
        SioCommand(commands[i][1], command);
        assert(memcmp(command, commands[i], 8) == 0);
    }
    SIO_STREAM stream = {0};
    uint8_t ack[] = {0x92,0x81,0,0,0,0,0,0};
    ack[7] = SioCrc8(ack,7);
    const uint8_t noise[] = {0, 0x92, 0x81, 0xff, 0xff};
    assert(Feed(&stream,noise,sizeof(noise)) == 0);
    assert(Feed(&stream,ack,3) == 0 && Feed(&stream,ack+3,5) == 1);
    assert(stream.total == 8 && stream.bytes[1] == 0x81);

    /* B, R, plus, capture, up and right. HOME comes from a separate pin. */
    uint8_t data[13 + 15 * 12] = {0};
    data[3] = 0x44; data[4] = 0x32; data[5] = 0x06;
    data[6] = 0; data[7] = 8; data[8] = 0xc0;  /* left (2048, 3072) */
    data[9] = 0; data[10] = 4; data[11] = 0x80; /* right (1024, 2048) */
    SIO_PAD pad = {0};
    SIO_MOTION motion = {0};
    assert(!SioInputReport(data,11,&pad,&motion));
    assert(SioInputReport(data,12,&pad,&motion));
    assert(pad.buttons == ((1u<<0)|(1u<<5)|(1u<<9)|(1u<<13)));
    assert(pad.hat == 1 && pad.axis[0] == 0 && pad.axis[1] == 32767);
    assert(pad.axis[2] == 32767 && pad.axis[3] == 0 && motion.count == 0);
    data[12] = 0x1f; /* ICM SIO report, fifteen samples */
    uint8_t *last = &data[13+14*12];
    last[1] = 0x10; last[3] = 0xf0; last[5] = 0x08;
    uint8_t header[8] = {0x92,0xc1,sizeof(data),0,0,0,0,0};
    header[6] = SioCrc8(data,sizeof(data)); header[7] = SioCrc8(header,7);
    assert(Feed(&stream,header,8) == 0);
    assert(Feed(&stream,data,127) == 0);
    assert(Feed(&stream,data+127,sizeof(data)-127) == 1);
    assert(SioInputReport(stream.bytes+8,stream.total-8,&pad,&motion));
    assert(motion.count == 15 && motion.accelMg[0] == 1000 &&
           motion.accelMg[1] == 1000 && motion.accelMg[2] == 500);
    assert(SioInputReport(data,sizeof(data)-1,&pad,&motion) && !motion.count);
    data[12] = 0xff; /* missing IMU must not disable the gamepad */
    assert(SioInputReport(data,sizeof(data),&pad,&motion) && !motion.count);
    memset(data,0,sizeof(data)); data[5] = 0x0f;
    assert(SioInputReport(data,12,&pad,&motion) && pad.hat == SioNeutralHat);
    /* Raw SIO bits from Switchroot Ubuntu: ZR=7, ZL=23. Other inputs remain gamepad-only. */
    memset(data,0,sizeof(data));
    data[7] = data[10] = 8;
    data[8] = data[11] = 0x80;
    SIO_POINTER_STATE pointer = {0};
    for (unsigned triggers = 0; triggers < 4; ++triggers) {
        data[3] = triggers & 1 ? 0x80 : 0;
        data[5] = triggers & 2 ? 0x80 : 0;
        assert(SioInputReport(data,12,&pad,&motion));
        SIO_MOUSE_REPORT mouse = SioPointerReport(&pointer,&pad,8);
        assert(mouse.id == 2 && mouse.buttons == triggers && !mouse.dx && !mouse.dy);
    }
    data[3] = 0x80; data[5] = 0;
    data[7] = 12; data[8] = 0x40; /* left stick right and up */
    assert(SioInputReport(data,12,&pad,&motion));
    SIO_MOUSE_REPORT mouse = SioPointerReport(&pointer,&pad,8);
    assert(mouse.buttons == 1 && mouse.dx == 6 && mouse.dy == -6);
    mouse = SioPointerReport(&pointer,&pad,8);
    assert(mouse.dx == 6 && mouse.dy == -6); /* a held stick keeps moving */
    mouse = SioPointerReport(&pointer,&pad,1000);
    assert(mouse.dx < 27 && mouse.dy > -27);
    pad.buttons = 0; pad.axis[0] = 1024; pad.axis[1] = -1024; pad.axis[2] = 32767;
    mouse = SioPointerReport(&pointer,&pad,8);
    assert(!mouse.buttons && !mouse.dx && !mouse.dy && !pointer.fraction[0] && !pointer.fraction[1]);
    /* A physical Capture edge releases a current drag and leaves the pad intact. */
    pad.buttons = SioPadZr; pad.axis[0] = 32767;
    mouse = SioPointerReport(&pointer,&pad,8);
    assert(mouse.buttons == 1 && mouse.dx > 0);
    pad.buttons |= SioPadCapture;
    mouse = SioPointerReport(&pointer,&pad,8);
    assert(pointer.disabled && !mouse.buttons && !mouse.dx && pad.buttons == (SioPadZr|SioPadCapture));
    mouse = SioPointerReport(&pointer,&pad,8);
    assert(pointer.disabled && !mouse.buttons && !mouse.dx);
    SioPointerResync(&pointer); /* still holding Capture across suspend */
    mouse = SioPointerReport(&pointer,&pad,32);
    assert(pointer.disabled && !mouse.buttons && !mouse.dx);
    pad.buttons = SioPadZr;
    (void)SioPointerReport(&pointer,&pad,8);
    pad.buttons |= SioPadCapture;
    mouse = SioPointerReport(&pointer,&pad,8);
    assert(!pointer.disabled && !mouse.buttons && !mouse.dx);
    mouse = SioPointerReport(&pointer,&pad,8);
    assert(!mouse.buttons && mouse.dx > 0);
    pad.buttons = 0; (void)SioPointerReport(&pointer,&pad,8);
    pad.buttons = SioPadZr; mouse = SioPointerReport(&pointer,&pad,8);
    assert(mouse.buttons == 1);
    /* Plus is raw bit 9 and stays in the gamepad report in both modes. */
    memset(data,0,sizeof(data)); data[7] = 8; data[8] = 0x80;
    data[4] = 2;
    assert(SioInputReport(data,12,&pad,&motion) && pad.buttons == SioPadPlus);
    (void)SioPointerReport(&pointer,&pad,8);
    assert(pointer.osk == SioOskPress);
    SIO_KEYBOARD_REPORT chord = SioOskReport(pointer.osk);
    assert(chord.id == 3 && chord.modifiers == 9 && chord.keys[0] == 0x12);
    pointer.osk = SioOskRelease; /* a press accepted by VHF */
    SioPointerResync(&pointer);
    chord = SioOskReport(pointer.osk);
    assert(pointer.osk == SioOskRelease && chord.modifiers == 0 && chord.keys[0] == 0);
    pointer.osk = SioOskIdle;
    (void)SioPointerReport(&pointer,&pad,8); /* held across reconnection */
    (void)SioPointerReport(&pointer,&pad,8);
    assert(pointer.osk == SioOskIdle);
    pad.buttons = SioPadCapture;
    (void)SioPointerReport(&pointer,&pad,8);
    assert(pointer.disabled);
    pad.buttons = SioPadPlus;
    (void)SioPointerReport(&pointer,&pad,8);
    assert(pointer.osk == SioOskIdle && pad.buttons == SioPadPlus);
    pad.buttons |= SioPadCapture; /* enable with Plus held */
    (void)SioPointerReport(&pointer,&pad,8);
    assert(!pointer.disabled && pointer.osk == SioOskIdle);
    pad.buttons = 0; (void)SioPointerReport(&pointer,&pad,8);
    pad.buttons = SioPadPlus; (void)SioPointerReport(&pointer,&pad,8);
    assert(pointer.osk == SioOskPress);
    SioPointerResync(&pointer); /* cancel a press not yet accepted */
    assert(pointer.osk == SioOskIdle);
    TestDesktopNavigation();
    puts("SIO gamepad, motion, pointer, wheel, D-pad arrows, Capture and Plus shortcut passed.");
    return 0;
}
