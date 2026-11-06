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
    puts("SIO command vectors, stream recovery, gamepad and motion reports passed.");
    return 0;
}
