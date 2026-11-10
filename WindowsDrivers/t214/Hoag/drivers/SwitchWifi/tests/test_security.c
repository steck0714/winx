/* SPDX-License-Identifier: ISC */
#include "../core/station_security.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_tx_across_key_write(void)
{
    swifi_eapol_tx tx = {0};
    assert(swifi_eapol_idle(&tx));

    /* A previous EAPOL is in the firmware and the next one is waiting for
     * a flow ring. Both belong to the old key. Later packets stay queued. */
    tx.pending = 1;
    assert(swifi_eapol_admit(&tx, SWIFI_TX_EAPOL));
    tx.changing = 1;
    assert(!swifi_eapol_idle(&tx));
    assert(!swifi_eapol_admit(&tx, SWIFI_TX_UNCLASSIFIED));
    assert(tx.head == SWIFI_TX_EAPOL);
    assert(swifi_eapol_admit(&tx, SWIFI_TX_EAPOL));
    ++tx.pending;
    tx.head = SWIFI_TX_DATA; /* NetAdapterCx advanced to a new packet. */
    assert(!swifi_eapol_admit(&tx, SWIFI_TX_EAPOL));
    --tx.pending;
    assert(!swifi_eapol_idle(&tx));
    --tx.pending;
    assert(swifi_eapol_idle(&tx));
    assert(!swifi_eapol_admit(&tx, SWIFI_TX_EAPOL)); /* Between drain and WSEC_KEY. */
    assert(!swifi_eapol_admit(&tx, SWIFI_TX_DATA));
    assert(swifi_eapol_idle(&tx));

    tx.changing = 0;
    assert(swifi_eapol_admit(&tx, SWIFI_TX_EAPOL));
    tx.head = SWIFI_TX_DATA;

    /* A full DMA pool left the previous head unclassified. When it proves
     * to be ordinary data, hold it for the new key and release the wait. */
    assert(!swifi_eapol_admit(&tx, SWIFI_TX_UNCLASSIFIED));
    tx.changing = 1;
    assert(!swifi_eapol_idle(&tx));
    assert(!swifi_eapol_admit(&tx, SWIFI_TX_DATA));
    assert(swifi_eapol_idle(&tx));
    assert(!swifi_eapol_admit(&tx, SWIFI_TX_UNCLASSIFIED));
    assert(swifi_eapol_idle(&tx));
    tx.changing = 0;
    assert(swifi_eapol_admit(&tx, SWIFI_TX_DATA));

    assert(!swifi_eapol_admit(&tx, SWIFI_TX_UNCLASSIFIED));
    tx.changing = 1;
    assert(swifi_eapol_admit(&tx, SWIFI_TX_EAPOL));
    tx.head = SWIFI_TX_DATA; /* The old head was cancelled before publication. */
    assert(swifi_eapol_idle(&tx));
    assert(!swifi_eapol_admit(&tx, SWIFI_TX_EAPOL));
}

int main(void)
{
    test_tx_across_key_write();
    const uint8_t ssid[] = {'t',0,'s'}, peer[] = {2,4,6,8,10,12};
    uint8_t beacon[96] = {0}, rsn[SWIFI_RSN_MAX], pmkid[16] = {0xfe}, wire[SWIFI_KEY_LEN];
    swifi_bss_profile profile;
    beacon[10]=1; beacon[12]=0; beacon[13]=3; memcpy(beacon+14,ssid,3);
    assert(swifi_match_bss(beacon,17,ssid,3,0,SWIFI_AUTH_OPEN,&profile));
    assert(!swifi_match_bss(beacon,17,ssid,3,0,SWIFI_AUTH_PSK_CCMP,&profile));
    beacon[10]=0x11;
    assert(!swifi_match_bss(beacon,17,ssid,3,0,3,&profile)); /* WEP is not open. */
    const uint8_t ap_rsn[]={48,24,1,0,0,15,172,4,2,0,0,15,172,2,0,15,172,4,1,0,0,15,172,2,0,0};
    memcpy(beacon+17,ap_rsn,sizeof(ap_rsn));
    size_t n=17+sizeof(ap_rsn);
    assert(swifi_match_bss(beacon,n,ssid,3,0,3,&profile) && profile.auth==SWIFI_AUTH_PSK_CCMP);
    beacon[n-2]=0x40; assert(!swifi_match_bss(beacon,n,ssid,3,0,3,&profile)); beacon[n-2]=0;
    assert(!swifi_match_bss(beacon,n-1,ssid,3,0,3,&profile));
    assert(!swifi_match_bss(beacon,n,(const uint8_t*)"xyz",3,0,3,&profile));
    memset(beacon+14,0,3);
    assert(!swifi_match_bss(beacon,n,ssid,3,0,3,&profile));
    assert(swifi_match_bss(beacon,n,ssid,3,1,3,&profile));
    uint8_t join[SWIFI_JOIN_LEN];
    assert(swifi_join_request(join,&profile,peer,0xd024));
    assert(join[0]==3 && !memcmp(join+4,ssid,3) && join[36]==255 && join[40]==16);
    assert(join[44]==0x40 && join[45]==1 && join[48]==0x90 && join[49]==1);
    assert(join[55]==255 && !memcmp(join+56,peer,6) && join[64]==1 && join[68]==0x24 && join[69]==0xd0);
    assert(!swifi_join_request(join,&profile,(const uint8_t*)"\xff\xff\xff\xff\xff\xff",0xd024));
    assert(swifi_rsn_request(rsn,pmkid)==40 && rsn[1]==38 && !memcmp(rsn+24,pmkid,16));
    assert(swifi_assoc_ies_match(rsn,40,&profile,pmkid));
    rsn[24]^=1;assert(!swifi_assoc_ies_match(rsn,40,&profile,pmkid));rsn[24]^=1;
    rsn[19]=8; assert(!swifi_assoc_ies_match(rsn,40,&profile,pmkid));
    assert(swifi_rsn_request(rsn,NULL)==22 && swifi_assoc_ies_match(rsn,22,&profile,NULL));
    assert(!swifi_assoc_ies_match(rsn,22,&profile,pmkid));
    profile.auth=SWIFI_AUTH_OPEN;
    assert(!swifi_assoc_ies_match(rsn,22,&profile,NULL));
    assert(swifi_assoc_ies_match(NULL,0,&profile,NULL));
    uint8_t key[16]={0}, pn[6]={1,2,3,4,5,6};
    for (unsigned i=0;i<16;++i) key[i]=(uint8_t)(i+32);
    assert(swifi_ccmp_key(wire,0,peer,key,pn));
    assert(wire[4]==16 && !memcmp(wire+8,key,16) && wire[112]==4 && wire[116]==0 && wire[132]==1);
    assert(!memcmp(wire+140,pn+2,4) && !memcmp(wire+144,pn,2) && !memcmp(wire+156,peer,6));
    assert(swifi_ccmp_key(wire,2,NULL,key,pn) && wire[0]==2 && wire[116]==2);
    assert(swifi_ccmp_key(wire,0,peer,NULL,NULL) && wire[4]==0 && wire[112]==0 && !memcmp(wire+156,peer,6));
    assert(!swifi_ccmp_key(wire,4,NULL,key,pn));
    assert(swifi_keep_key_on_retry(1,1,pn,pn)==1);
    uint8_t requested[6]={0,0,0,0,0,7};
    assert(swifi_keep_key_on_retry(1,1,requested,pn)==0);
    requested[5]=5;
    assert(swifi_keep_key_on_retry(1,1,requested,pn)==1);
    assert(swifi_keep_key_on_retry(0,1,pn,pn)==0);
    assert(swifi_keep_key_on_retry(1,0,pn,pn)==0);
    puts("station security: RSN selection, PMKID, join, CCMP key/RSC and removal passed");
    return 0;
}
