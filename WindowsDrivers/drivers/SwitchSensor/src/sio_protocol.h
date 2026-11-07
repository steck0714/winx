/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SWITCH_SIO_PROTOCOL_H
#define SWITCH_SIO_PROTOCOL_H

#include <stdint.h>

enum {
    SioInit = 0x01, SioVersion = 0x03, SioInput = 0x41, SioAck = 0x80,
    SioHeaderBytes = 8, SioMaxPayload = 208, SioNeutralHat = 8
};

typedef struct {
    uint8_t bytes[SioHeaderBytes + SioMaxPayload];
    uint16_t used;
    uint16_t total;
} SIO_STREAM;

typedef struct {
    uint16_t buttons;
    uint8_t hat, dpad; /* dpad preserves the four physical switches */
    int16_t axis[4];
} SIO_PAD;

/* Gamepad button indices after decoding Switchroot's SIO input layout. */
enum { SioPadZl = 1u << 6, SioPadZr = 1u << 7, SioPadPlus = 1u << 9,
       SioPadRightStick = 1u << 11, SioPadCapture = 1u << 13 };
enum { SioDown = 1, SioUp = 2, SioRight = 4, SioLeft = 8 };

typedef struct {
    uint8_t id, buttons;
    int8_t dx, dy, wheel, pan;
} SIO_MOUSE_REPORT;

enum { SioOskIdle, SioOskPress, SioOskRelease };
typedef struct {
    uint8_t id, modifiers, reserved, keys[6];
} SIO_KEYBOARD_REPORT;

typedef struct {
    int32_t fraction[2], scrollFraction[2];
    uint8_t disabled, captureDown, plusDown, synced, blockedButtons, osk;
    uint8_t blockedArrows, arrows, sentArrows;
} SIO_POINTER_STATE;

/* Keep the chosen mode across suspend and UART reconnection. The first real
 * frame establishes a baseline so an already-held Capture is never a new press. */
static inline void SioPointerResync(SIO_POINTER_STATE *state)
{
    state->synced = 0;
    if (state->osk == SioOskPress) state->osk = SioOskIdle;
    state->fraction[0] = state->fraction[1] = 0;
    state->scrollFraction[0] = state->scrollFraction[1] = 0;
    state->arrows = 0; /* Keep sentArrows until VHF accepts the release. */
}

typedef struct {
    uint8_t count;
    int32_t accelMg[3]; /* newest sample, in console coordinates */
} SIO_MOTION;

static inline uint8_t SioCrc8(const uint8_t *bytes, unsigned count)
{
    uint8_t crc = 0;
    for (unsigned i = 0; i < count; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (uint8_t)((crc << 1) ^ ((crc & 0x80) ? 0x8d : 0));
    }
    return crc;
}

static inline void SioCommand(uint8_t command, uint8_t header[SioHeaderBytes])
{
    header[0] = 0x91;
    header[1] = command;
    header[2] = header[3] = 0;
    header[4] = command == SioVersion ? 3 : 0;
    header[5] = command == SioVersion ? 4 : 0;
    header[6] = 0;
    header[7] = SioCrc8(header, 7);
}

/* When this returns 1, bytes contains a complete frame until the next byte.
 * The header CRC is the one checked by Hekate's SIO input path. */
static inline int SioPush(SIO_STREAM *s, uint8_t byte)
{
    if (!s->used && byte != 0x92) return 0;
    s->bytes[s->used++] = byte;
    if (s->used == SioHeaderBytes) {
        unsigned size = s->bytes[2] | ((unsigned)s->bytes[3] << 8);
        if (!(s->bytes[1] & SioAck) || size > SioMaxPayload ||
            SioCrc8(s->bytes, 7) != s->bytes[7]) {
            unsigned next = 1;
            while (next < SioHeaderBytes && s->bytes[next] != 0x92) ++next;
            s->used = (uint16_t)(SioHeaderBytes - next);
            for (unsigned i = 0; i < s->used; ++i) s->bytes[i] = s->bytes[next + i];
            return 0;
        }
        s->total = (uint16_t)(SioHeaderBytes + size);
    }
    if (s->used < SioHeaderBytes || s->used != s->total) return 0;
    s->used = 0;
    return 1;
}

/* Switchroot's fallback stick calibration is 1024 / 2048 / 3072. */
static inline int16_t SioAxis(int raw)
{
    int delta = raw - 2048;
    if (delta >= 1024) return 32767;
    if (delta <= -1024) return -32767;
    return (int16_t)(delta * 32767 / 1024);
}

static inline int SioSigned16(const uint8_t *p)
{
    int word = p[0] | ((int)p[1] << 8);
    return word >= 32768 ? word - 65536 : word;
}

static inline int SioInputReport(const uint8_t *p, unsigned length, SIO_PAD *pad, SIO_MOTION *motion)
{
    static const uint8_t sourceBit[] = {2, 3, 0, 1, 22, 6, 23, 7, 8, 9, 11, 10};
    static const uint8_t hat[3][3] = {{7, 0, 1}, {6, SioNeutralHat, 2}, {5, 4, 3}};
    if (length < 12) return 0;
    uint32_t buttons = p[3] | ((uint32_t)p[4] << 8) | ((uint32_t)p[5] << 16);
    pad->buttons = 0;
    for (unsigned i = 0; i < sizeof(sourceBit); ++i)
        pad->buttons |= (uint16_t)(((buttons >> sourceBit[i]) & 1u) << i);
    /* Home is a separate active-low GPIO. Capture is the fourteenth button. */
    pad->buttons |= (uint16_t)(buttons & SioPadCapture);
    pad->dpad = (uint8_t)((buttons >> 16) & 15u);
    int x = !!(pad->dpad & SioRight) - !!(pad->dpad & SioLeft);
    int y = !!(pad->dpad & SioDown) - !!(pad->dpad & SioUp);
    pad->hat = hat[y + 1][x + 1];
    for (unsigned stick = 0; stick < 2; ++stick) {
        const uint8_t *s = p + 6 + 3 * stick;
        int sx = SioAxis(s[0] | ((s[1] & 15) << 8));
        int sy = SioAxis((s[1] >> 4) | (s[2] << 4));
        pad->axis[2 * stick] = (int16_t)(stick ? -sx : sx);
        pad->axis[2 * stick + 1] = (int16_t)(stick ? -sy : sy);
    }
    motion->count = 0;
    if (length >= 13 && (p[12] >> 4) <= 1) {
        unsigned n = p[12] & 15;
        if (n && 13 + n * 12 <= length) {
            const uint8_t *last = p + 13 + (n - 1) * 12;
            motion->count = (uint8_t)n;
            motion->accelMg[0] = -SioSigned16(last + 2) * 1000 / 4096;
            motion->accelMg[1] = SioSigned16(last) * 1000 / 4096;
            motion->accelMg[2] = SioSigned16(last + 4) * 1000 / 4096;
        }
    }
    return 1;
}
/* Convert the calibrated left stick to relative desktop motion. Retain sub-count
 * movement for fine aiming. A centered axis discards its fraction immediately. */
static inline int8_t SioPointerAxis(int axis, unsigned ms, int32_t *fraction)
{
    const int deadZone = 3500;
    const int maxSpeed = 800;  /* HID counts per second at full deflection */
    int magnitude = axis < 0 ? -axis : axis;
    if (magnitude <= deadZone) {
        *fraction = 0;
        return 0;
    }
    int speed = (magnitude - deadZone) * maxSpeed / (32767 - deadZone);
    if (axis < 0) speed = -speed;
    int total = *fraction + speed * (int)ms;
    *fraction = total % 1000;
    return (int8_t)(total / 1000);
}

/* Fixed-point samples of xf86-input-joystick's default relative scroll curve.
 * Rates are thousandths of a detent per second, at eighths of stick travel
 * beyond its 5000-count dead zone. Accumulate time across SIO packets. */
static inline int8_t SioScrollAxis(int axis, unsigned ms, int32_t *fraction)
{
    static const int rate[] = {1000,1170,1960,3647,6433,10491,15971,23010,31732};
    const int deadZone = 5000, range = 32767 - deadZone;
    int magnitude = axis < 0 ? -axis : axis;
    if (magnitude <= deadZone) {
        *fraction = 0;
        return 0;
    }
    int position = (magnitude - deadZone) * 8;
    int index = position / range;
    int speed = index >= 8 ? rate[8] :
        rate[index] + (rate[index+1] - rate[index]) * (position % range) / range;
    if ((*fraction < 0 && axis > 0) || (*fraction > 0 && axis < 0)) *fraction = 0;
    int total = *fraction + (axis < 0 ? -speed : speed) * (int)ms;
    *fraction = total % 1000000;
    return (int8_t)(total / 1000000);
}

static inline SIO_MOUSE_REPORT SioPointerReport(SIO_POINTER_STATE *state, const SIO_PAD *pad,
                                                unsigned elapsedMs)
{
    SIO_MOUSE_REPORT report = {2, 0, 0, 0, 0, 0};
    uint8_t buttons = (uint8_t)(((pad->buttons & SioPadZr) ? 1 : 0) |
                                ((pad->buttons & SioPadZl) ? 2 : 0) |
                                ((pad->buttons & SioPadRightStick) ? 4 : 0));
    uint8_t capture = (pad->buttons & SioPadCapture) != 0;
    uint8_t plus = (pad->buttons & SioPadPlus) != 0;

    if (!state->synced) {
        state->synced = 1;
        state->blockedButtons = buttons;
        state->blockedArrows = pad->dpad;
        elapsedMs = 0;
    } else if (capture && !state->captureDown) {
        state->disabled = !state->disabled;
        if (state->osk == SioOskPress) state->osk = SioOskIdle;
        state->fraction[0] = state->fraction[1] = 0;
        state->scrollFraction[0] = state->scrollFraction[1] = 0;
        state->blockedButtons = buttons;
        state->blockedArrows = pad->dpad;
        elapsedMs = 0;
    } else if (!state->disabled && plus && !state->plusDown && state->osk == SioOskIdle) {
        state->osk = SioOskPress;
    }
    state->plusDown = plus;
    state->captureDown = capture;
    state->blockedButtons &= buttons;
    /* Release arrows before Ctrl+Win+O so a held direction cannot turn the
     * shortcut into desktop switching. Fresh presses work after the chord. */
    if (state->osk != SioOskIdle) state->blockedArrows = pad->dpad;
    state->blockedArrows &= pad->dpad;
    state->arrows = state->disabled ? 0 : (uint8_t)(pad->dpad & ~state->blockedArrows);
    if (state->disabled) return report;

    /* A trigger held when mouse mode returns must be released before a click.
     * Limit the first movement after a late UART packet. */
    unsigned ms = elapsedMs > 32 ? 32 : elapsedMs;
    report.buttons = (uint8_t)(buttons & ~state->blockedButtons);
    report.dx = SioPointerAxis(pad->axis[0], ms, &state->fraction[0]);
    report.dy = SioPointerAxis(pad->axis[1], ms, &state->fraction[1]);
    report.wheel = SioScrollAxis(-pad->axis[3], ms, &state->scrollFraction[1]); /* up is positive */
    report.pan = SioScrollAxis(pad->axis[2], ms, &state->scrollFraction[0]); /* right is positive */
    return report;
}

/* Send a tap, even when Plus is held. A failed submission leaves the same
 * transition pending so that Ctrl and Windows are always released in order. */
static inline SIO_KEYBOARD_REPORT SioOskReport(uint8_t phase)
{
    SIO_KEYBOARD_REPORT report = {3, 0, 0, {0}};
    if (phase == SioOskPress) {
        report.modifiers = 0x09; /* Left Ctrl + Left GUI */
        report.keys[0] = 0x12;   /* HID Keyboard O */
    }
    return report;
}

/* Only advance sentArrows or the OSK phase after a successful submission. */
static inline int SioNextKeyboardReport(const SIO_POINTER_STATE *state, SIO_KEYBOARD_REPORT *report)
{
    if (state->arrows != state->sentArrows) {
        static const uint8_t usages[] = {0x51, 0x52, 0x4f, 0x50}; /* Down, Up, Right, Left */
        *report = SioOskReport(SioOskIdle);
        unsigned count = 0;
        for (unsigned bit = 0; bit < 4; ++bit)
            if (state->arrows & (1u << bit)) report->keys[count++] = usages[bit];
        return 1;
    }
    if (state->osk == SioOskIdle) return 0;
    *report = SioOskReport(state->osk);
    return 1;
}

static inline void SioKeyboardReportAccepted(SIO_POINTER_STATE *state)
{
    if (state->arrows != state->sentArrows)
        state->sentArrows = state->arrows;
    else
        state->osk = state->osk == SioOskPress ? SioOskRelease : SioOskIdle;
}

#endif
