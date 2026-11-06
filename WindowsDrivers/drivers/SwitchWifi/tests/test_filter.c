/* SPDX-License-Identifier: ISC */
#include "../core/receive_filter.h"
#include "../core/station_security.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const uint8_t station[6] = {2,0,1,2,3,4}, other[6] = {2,1,2,3,4,5};
    const uint8_t broadcast[6] = {255,255,255,255,255,255};
    /* IPv6 solicited-node multicast and IPv4 multicast; the Windows
     * NetAdapter callback uses a wider stride than the WDI TLV. */
    uint8_t list[2][12] = {{0x33,0x33,0xff,0,0,1},{1,0,0x5e,0,0,0xfb}};
    const uint8_t unknown[6] = {0x33,0x33,0xff,0,0,2};
    swifi_receive_filter f = {0};
    assert(swifi_filter_list(&f,list[0],2,sizeof(list[0])));
    f.flags = SWIFI_DIRECTED | SWIFI_BROADCAST | SWIFI_MULTICAST;
    assert(swifi_filter_accepts(&f,station,station));
    assert(!swifi_filter_accepts(&f,station,other));
    assert(swifi_filter_accepts(&f,station,broadcast));
    assert(swifi_filter_accepts(&f,station,list[0]) && swifi_filter_accepts(&f,station,list[1]));
    assert(!swifi_filter_accepts(&f,station,unknown));
    uint8_t wire[16] = {0xaa};
    assert(swifi_filter_wire(&f,wire,sizeof(wire)) == 16);
    assert(!memcmp(wire,(uint8_t[]){2,0,0,0},4));
    assert(!memcmp(wire+4,list[0],6) && !memcmp(wire+10,list[1],6));
    assert(!swifi_filter_wire(&f,wire,15));
    list[1][0] = 2;
    assert(!swifi_filter_list(&f,list[0],2,sizeof(list[0])) && f.count==2 && f.addresses[1][0]==1);
    assert(!swifi_filter_list(&f,list[0],SWIFI_MAX_MULTICAST+1,sizeof(list[0])));
    f.flags = SWIFI_ALL_MULTICAST;
    assert(swifi_filter_accepts(&f,station,unknown) && !swifi_filter_accepts(&f,station,broadcast));
    assert(!swifi_filter_accepts(&f,station,station));
    f.flags = SWIFI_DIRECTED;
    assert(swifi_filter_wire(&f,wire,sizeof(wire))==4 && wire[0]==0);
    assert(f.count==2 && !swifi_filter_accepts(&f,station,list[0]));
    assert(swifi_filter_list(&f,NULL,0,0) && f.count==0);
    assert(!swifi_filter_accepts(&f,station,unknown));
    f.flags = 0;
    assert(!swifi_filter_accepts(&f,station,station));
    /* Re-evaluate a clear EAPOL queued before Windows changed its key rule. */
    assert(swifi_clear_eapol_allowed(1,1,1));
    assert(swifi_clear_eapol_allowed(2,0,1) && !swifi_clear_eapol_allowed(2,1,1));
    assert(swifi_clear_eapol_allowed(0,0,0) && !swifi_clear_eapol_allowed(0,0,1));
    assert(!swifi_clear_eapol_allowed(3,0,0));
    puts("station receive filters: address list, broadcast, IPv6, masks and queued key policy passed");
    return 0;
}
