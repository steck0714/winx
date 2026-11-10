/* SPDX-License-Identifier: ISC */
#include "../core/rx_metadata.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_sideband(void)
{
    swifi_rx_metadata fields;
    /* Bytes before offset four are a BCM header, not TLVs. */
    uint8_t bytes[] = {8,1,250,0, 44,2,1,2, 8,1,185, 16,2,3,4, 255,8,1,215};
    assert(swifi_parse_rx_metadata(bytes,sizeof(bytes),&fields));
    assert(fields.have_rssi && fields.rssi_dbm == -71 && fields.have_timestamp && fields.other);
    assert(!swifi_parse_rx_metadata(bytes,14,&fields));
    assert(!fields.have_rssi && !fields.have_timestamp && !fields.other);
    assert(!swifi_parse_rx_metadata(bytes,3,&fields));
    assert(!swifi_parse_rx_metadata(bytes,SWIFI_RX_METADATA_SIZE+1,&fields));
    bytes[10]=0x80;
    assert(swifi_parse_rx_metadata(bytes,sizeof(bytes),&fields) && !fields.have_rssi);
    bytes[9]=0;
    assert(!swifi_parse_rx_metadata(bytes,sizeof(bytes),&fields));
    assert(swifi_parse_rx_metadata(NULL,0,&fields) && !fields.have_rssi);
    assert(swifi_signal_quality(-120)==0 && swifi_signal_quality(-76)==48 && swifi_signal_quality(-40)==100);
}

static void test_peer_rates(void)
{
    const uint8_t peer[6]={0x02,2,3,4,5,6};
    uint8_t sta[80]={4,0,sizeof(sta),0};
    uint32_t tx,rx;
    memcpy(sta+16,peer,6);
    sta[52]=1;
    sta[68]=0xe8; sta[69]=0xfd; /* 65000 kbps */
    sta[72]=0x10; sta[73]=0x27; /* 10000 kbps */
    assert(swifi_station_rates(sta,sizeof(sta),peer,&tx,&rx) && tx==65000 && rx==0);
    sta[64]=1;
    assert(swifi_station_rates(sta,sizeof(sta),peer,&tx,&rx) && rx==10000);
    sta[16]^=1;
    assert(!swifi_station_rates(sta,sizeof(sta),peer,&tx,&rx) && !tx && !rx);
    sta[16]^=1; sta[2]=60;
    assert(!swifi_station_rates(sta,sizeof(sta),peer,&tx,&rx));
    sta[2]=80; sta[0]=7;
    assert(!swifi_station_rates(sta,sizeof(sta),peer,&tx,&rx));
    sta[0]=4;
    assert(!swifi_station_rates(sta,75,peer,&tx,&rx));
}

int main(void)
{
    test_sideband();
    test_peer_rates();
    assert(swifi_prekey_ethernet(0,0,0,0));
    assert(!swifi_prekey_ethernet(0,1,0,0));
    assert(!swifi_prekey_ethernet(2,2,1,0));
    assert(!swifi_prekey_ethernet(2,2,0,1));
    puts("RX metadata: wire TLVs, peer rates and EAPOL key boundaries passed");
    return 0;
}
