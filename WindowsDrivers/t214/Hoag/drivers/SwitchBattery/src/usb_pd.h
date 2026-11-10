/* BQ24193 input-current codes. Lite limits and port reserve come from
 * Switchroot's Vali power tree and bm92txx.c. All arguments use mV/mA. */
#ifndef SWITCH_USB_PD_H
#define SWITCH_USB_PD_H

enum {
    SwBatErista,
    SwBatLite
};

static inline unsigned SwBatCurrentCode(unsigned board, unsigned mv, unsigned advertised, unsigned operating,
                                        unsigned *actual)
{
    static const unsigned bq[] = {100, 150, 500, 900, 1200, 1500, 2000, 3000};
    unsigned limit = 500;
    if (board == SwBatLite && mv && advertised) {
        unsigned reserve = mv > 5000 ? 4500000u / mv : 2500000u / mv;
        unsigned budget = advertised;
        if (operating && operating < budget)
            budget = operating;
        if (mv == 5000)
            limit = 2000;
        else if (mv == 9000)
            limit = 1500;
        else if (mv == 12000)
            limit = 1200;
        else if (mv == 15000)
            limit = 900;
        if (!operating || mv > 15000)
            limit = 500;
        budget = budget > reserve ? budget - reserve : 0;
        if (budget < limit)
            limit = budget;
    } else if (board == SwBatErista && mv) {
        if (mv <= 9500)
            limit = 2000;
        else if (mv <= 12500)
            limit = 1500;
        else if (mv <= 15500)
            limit = 1200;
        if (advertised && advertised < limit)
            limit = advertised;
    }
    if (board == SwBatLite && limit < bq[0]) {
        *actual = 0;
        return 0x80; /* HIZ: the port reserve consumes the input budget. */
    }
    unsigned code = (sizeof(bq) / sizeof(bq[0])) - 1;
    while (code && bq[code] > limit)
        --code;
    *actual = bq[code];
    return code;
}
#endif
