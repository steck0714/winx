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
    puts("SIO framing, gamepad, motion, desktop mouse, Capture toggle and Plus keyboard shortcut passed.");
    return 0;
}
