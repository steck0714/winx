#include "../src/usb_pd.h"
#include <assert.h>
#include <stdio.h>

static void check(unsigned board, unsigned mv, unsigned pdo, unsigned rdo, unsigned expected)
{
    unsigned actual;
    const unsigned bq[] = {100, 150, 500, 900, 1200, 1500, 2000, 3000};
    unsigned code = SwBatCurrentCode(board, mv, pdo, rdo, &actual);
    assert(actual == expected);
    if (actual == 0)
        assert(code == 0x80);
    else
        assert(code < 8 && bq[code] == actual);
}
int main(void)
{
    check(SwBatLite, 5000, 3000, 3000, 2000);
    check(SwBatLite, 9000, 3000, 3000, 1500);
    check(SwBatLite, 12000, 3000, 3000, 1200);
    check(SwBatLite, 15000, 2600, 2000, 900);
    check(SwBatLite, 9000, 3000, 1500, 900);
    check(SwBatLite, 5000, 3000, 400, 0);
    check(SwBatLite, 9000, 3000, 0, 500);
    check(SwBatLite, 5000, 600, 0, 100);
    check(SwBatLite, 5000, 400, 0, 0);
    check(SwBatErista, 9000, 3000, 0, 2000);
    check(SwBatErista, 15000, 1000, 0, 900);
    puts("PD contract, Vali input limits and legacy current bins passed");
}
