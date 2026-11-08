/* I2C slave model for PY3/PY4. Decode edges independently of the firmware
 * implementation and acknowledge only the MAX77620 register transactions. */
enum wire_phase { wire_address, wire_register, wire_write, wire_read, wire_wait };
static struct {
    uint32_t config, oe, out;
    bool connected, active, slave_low, nack_address, force_scl_low;
    enum wire_phase phase;
    unsigned int clocks, byte, reg, starts, stops, reads, power_writes;
} wire;

enum { wire_scl = 8, wire_sda = 16, wire_pins = 24 };

static unsigned int master_lines(void)
{
    return wire_pins & ~(wire.config & wire.oe & ~wire.out);
}

static unsigned int bus_lines(void)
{
    unsigned int levels = master_lines();
    if (!wire.connected || (wire.active && wire.force_scl_low))
        levels &= ~wire_scl;
    return wire.slave_low ? levels & ~wire_sda : levels;
}

static void wire_falling(void)
{
    if (wire.clocks == 9) {
        switch (wire.phase) {
        case wire_address:
            assert((wire.byte >> 1) == 0x3c);
            wire.phase = wire.nack_address ? wire_wait :
                         (wire.byte & 1) ? wire_read : wire_register;
            break;
        case wire_register:
            assert(wire.byte == 0x05 || wire.byte == 0x0b ||
                   wire.byte == 0x41 || wire.byte == 0x42);
            wire.reg = wire.byte;
            wire.phase = wire_write;
            break;
        case wire_write:
            if (wire.reg == 0x41) {
                assert(!(pmic[0x42] & 0x80));
                assert(wire.byte == (pmic[0x41] | 0x80));
                wire.power_writes++;
            } else {
                assert(wire.reg == 0x42);
                assert(wire.byte == (pmic[0x42] & 0x7f));
                if (!mismatch) pmic[0x42] = wire.byte;
            }
            wire.phase = wire_wait;
            break;
        case wire_read:
            if (wire.reg == 0x05 || wire.reg == 0x0b) pmic[wire.reg] = 0;
            wire.reads++;
            wire.phase = wire_wait;
            break;
        case wire_wait: assert(false);
        }
        wire.clocks = 0;
        wire.byte = 0;
    }
    wire.slave_low = false;
    if (wire.phase == wire_read && wire.clocks < 8)
        wire.slave_low = !(pmic[wire.reg] & (0x80U >> wire.clocks));
    if (wire.clocks == 8 && wire.phase != wire_read && wire.phase != wire_wait)
        wire.slave_low = !(wire.nack_address && wire.phase == wire_address);
}

static uint32_t wire_reg_read(unsigned int reg)
{
    switch (reg) {
    case 0x00: return wire.config;
    case 0x10: return wire.oe;
    case 0x20: return wire.out;
    case 0x30: return bus_lines();
    default: assert(false); return 0;
    }
}

static void wire_reg_write(unsigned int reg, uint32_t value)
{
    unsigned int before = bus_lines(), mask = value >> 8;
    assert(!(mask & ~wire_pins));
    uint32_t *target;
    switch (reg) {
    case 0x80: target = &wire.config; break;
    case 0x90: target = &wire.oe; break;
    case 0xa0: target = &wire.out; break;
    default: assert(false); return;
    }
    *target = (*target & ~mask) | (value & mask);
    /* Open drain: a line driven by the GPIO must always drive zero. */
    assert(!(wire.config & wire.oe & wire.out & wire_pins));
    unsigned int after = bus_lines();
    if ((before & after & wire_scl) && ((before ^ after) & wire_sda)) {
        if (after & wire_sda) {
            wire.active = false;
            wire.stops++;
        } else {
            wire.active = true;
            wire.phase = wire_address;
            wire.byte = wire.clocks = 0;
            wire.starts++;
        }
        wire.slave_low = false;
    } else if (wire.active && ((before ^ after) & wire_scl)) {
        if (after & wire_scl) {
            if (wire.clocks < 8 && wire.phase != wire_read)
                wire.byte = ((wire.byte << 1) | !!(after & wire_sda)) & 0xff;
            if (wire.clocks == 8 && wire.phase == wire_read)
                assert(after & wire_sda); /* master NACKs the final byte */
            wire.clocks++;
        } else {
            wire_falling();
        }
    }
}
