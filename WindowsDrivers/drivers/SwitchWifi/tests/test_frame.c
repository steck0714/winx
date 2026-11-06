/* SPDX-License-Identifier: ISC */
#include "../core/station_frame.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const uint8_t sta[6] = {2,0,0,0,0,1}, ap[6] = {2,0,0,0,0,2};
    const uint8_t remote[6] = {2,0,0,0,0,3}, all[6] = {255,255,255,255,255,255};
    uint8_t tx[1600] = {0x88,1}, eth[1600] = {0}, wifi[1600], priority = 0;
    const uint8_t snap_ip[] = {0xaa,0xaa,3,0,0,0,8,0,0x45,0,0,0x28};
    memcpy(tx+4,ap,6); memcpy(tx+10,sta,6); memcpy(tx+16,all,6);
    tx[24]=6; memcpy(tx+26,snap_ip,sizeof(snap_ip));
    size_t size = 26+sizeof(snap_ip);
    assert(swifi_station_tx(tx,size,sta,ap,eth,sizeof(eth),&priority)==18 && priority==6);
    assert(!memcmp(eth,all,6) && !memcmp(eth+6,sta,6) && !memcmp(eth+12,snap_ip+6,6));
    uint8_t copy[1600]; memcpy(copy,tx,size);
    assert(swifi_station_tx(copy,size,sta,ap,copy,sizeof(copy),&priority)==18 && !memcmp(copy,eth,18));
    assert(swifi_station_tx(tx,size,sta,ap,copy,17,&priority)==0);
    tx[24]|=0x80; assert(!swifi_station_tx(tx,size,sta,ap,copy,sizeof(copy),&priority)); tx[24]=0;
    tx[1]|=4; assert(!swifi_station_tx(tx,size,sta,ap,copy,sizeof(copy),&priority)); tx[1]=1;
    tx[22]=1; assert(!swifi_station_tx(tx,size,sta,ap,copy,sizeof(copy),&priority)); tx[22]=0;
    assert(!swifi_station_tx(tx,size,ap,sta,copy,sizeof(copy),&priority));
    tx[1]|=0x80; memmove(tx+30,tx+26,sizeof(snap_ip));
    assert(swifi_station_tx(tx,size+4,sta,ap,copy,sizeof(copy),&priority)==18);

    memcpy(eth,sta,6); memcpy(eth+6,remote,6);
    eth[12]=0x88; eth[13]=0x8e; /* EAPOL stays unprotected. */
    assert(swifi_station_rx(eth,18,sta,ap,0x123,0,wifi,sizeof(wifi))==36);
    assert(wifi[0]==8 && wifi[1]==2 && wifi[22]==0x30 && wifi[23]==0x12);
    assert(!memcmp(wifi+4,sta,6) && !memcmp(wifi+10,ap,6) && !memcmp(wifi+16,remote,6));
    assert(wifi[30]==0x88 && wifi[31]==0x8e && !memcmp(wifi+32,eth+14,4));
    assert(!swifi_station_rx(eth,18,sta,ap,0,0,wifi,35));
    assert(!swifi_station_rx(eth,18,ap,ap,0,0,wifi,sizeof(wifi)));
    memcpy(eth,all,6); eth[12]=0x81; eth[13]=0x37;
    assert(swifi_station_rx(eth,18,sta,ap,9,1,wifi,sizeof(wifi))==36 && wifi[29]==0xf8 && wifi[1]==0x42);
    /* RFC1042 IPX is kept as LLC/length, while bridge-tunnel IPX is Ethernet II. */
    memcpy(tx+30,wifi+24,12); tx[35]=0;
    assert(swifi_station_tx(tx,42,sta,ap,copy,sizeof(copy),&priority)==26 && copy[13]==12);
    tx[35]=0xf8;
    assert(swifi_station_tx(tx,42,sta,ap,copy,sizeof(copy),&priority)==18 && copy[12]==0x81);
    eth[12]=0; eth[13]=3; eth[14]=0x42; eth[15]=0x42; eth[16]=3;
    assert(swifi_station_rx(eth,60,sta,ap,1,0,wifi,sizeof(wifi))==27 && !memcmp(wifi+24,eth+14,3));
    eth[13]=47; assert(!swifi_station_rx(eth,60,sta,ap,1,0,wifi,sizeof(wifi)));
    uint8_t native[40] = {8,2};
    memcpy(native+4,sta,6); memcpy(native+10,ap,6); memcpy(native+16,ap,6);
    const uint8_t pae[] = {0xaa,0xaa,3,0,0,0,0x88,0x8e,2,3,0,2,0xab,0xcd};
    memcpy(native+24,pae,sizeof(pae));
    native[1]|=0x40; assert(!swifi_clear_eapol(native,38,sta,ap)); native[1]=2;
    assert(!swifi_clear_eapol(native,37,sta,ap));
    assert(swifi_clear_eapol(native,38,sta,ap)==20);
    assert(!memcmp(native,sta,6) && !memcmp(native+6,ap,6) && native[12]==0x88 && native[19]==0xcd);
    puts("station frame fixtures passed");
    return 0;
}
