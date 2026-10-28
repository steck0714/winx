#ifndef __FTS4_H__
#define __FTS4_H__

#define FTS4_I2C_ADDR                0x49
#define FTS4_I2C_CHIP_ID             0x3670

// Commands
#define FTS4_CMD_READ_ONE_EVENT      0x85
#define FTS4_CMD_LATEST_EVENT        0x87   // returns newest event, clears the stack
#define FTS4_CMD_MS_MT_SENSE_ON      0x93
#define FTS4_CMD_CLEAR_EVENT_STACK   0xA1
#define FTS4_CMD_HW_REG_READ         0xB6   // u16be address, any size read
#define FTS4_CMD_HW_REG_WRITE        0xB6
#define FTS4_CMD_SWITCH_SENSE_MODE   0xC3

#define FTS4_FINGER_MODE             0x01

// HW registers
#define FTS4_HW_REG_CHIP_ID_INFO     0x0004
#define FTS4_HW_REG_EVENT_COUNT      0x0023
#define FTS4_HW_REG_SYS_RESET        0x0028

// Events
#define FTS4_EV_MULTI_TOUCH_ENTER    0x03
#define FTS4_EV_MULTI_TOUCH_LEAVE    0x04
#define FTS4_EV_MULTI_TOUCH_MOTION   0x05
#define FTS4_EV_CONTROLLER_READY     0x10

#define FTS4_MASK_EVENT_ID           0x0F
#define FTS4_MASK_TOUCH_ID           0xF0
#define FTS4_MASK_X_MSB              0x0F
#define FTS4_MASK_Y_LSB              0xF0

#define FTS4_EVENT_SIZE              8

// Raw panel limits and the logical (display) resolution.
#define FTS4_X_REAL_MAX              1264
#define FTS4_Y_REAL_MAX              704
#define FTS4_EDGE_OFFSET             15
#define TOUCH_RES_X                  1280
#define TOUCH_RES_Y                  720

// Touch reset line (hekate: DAP4_SCLK / GPIO PJ7) and its pinmux register.
#define TOUCH_RST_GPIO_PORT          GPIO_PORT_J
#define TOUCH_RST_GPIO_PIN           GPIO_PIN_7
#define TOUCH_RST_PINMUX_REG         (0x70003000UL + 0x150)  // PINMUX_AUX_DAP4_SCLK
#define TOUCH_RST_PINMUX_VAL         ((1 << 2) | 1)          // pull-down | function 1

// APB MISC pinmux regs for GEN3_I2C (I2C3) and the I2C4 pads that must be detached from it.
#define PINMUX_AUX_BASE              0x70003000UL
#define PINMUX_AUX_I2C_SCL(x)        (PINMUX_AUX_BASE + 0xBC + 8 * (x))
#define PINMUX_AUX_I2C_SDA(x)        (PINMUX_AUX_BASE + 0xC0 + 8 * (x))
#define PINMUX_INPUT_ENABLE          (1 << 6)

#define TOUCH_POLL_INTERVAL_100NS    100000   // 10 ms

#endif
