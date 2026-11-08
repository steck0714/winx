/* Host model of the BCM4356 msgbuf-v5 wire protocol and DMA lifetime.
 * This exercises the actual transport header, not physical PCIe or radio. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define STATIC static
#define CONST const
#define VOID void
#define TRUE 1
#define FALSE 0
#define BIT16 (1U<<16)
#define BIT20 (1U<<20)
#define BIT28 (1U<<28)
#define EFI_SIZE_TO_PAGES(n) (((n)+4095)/4096)
#define EFI_PAGES_TO_SIZE(n) ((n)*4096)
#define EFI_ERROR(n) ((n)!=0)
#define EfiBootServicesData 4
#define MapOperationBusMasterCommonBuffer 2
#define CopyMem memcpy
#define ZeroMem(p,n) memset(p,0,n)
#define MemoryFence() __sync_synchronize()
#define AsciiStrLen strlen
typedef int BOOLEAN, EFI_STATUS;
typedef ptrdiff_t INTN;
typedef char CHAR8;
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef int16_t INT16;
typedef uint32_t UINT32;
typedef size_t UINTN;
typedef uint64_t PHYSICAL_ADDRESS;

#define TCM 0x180000U
#define SHARED (TCM+0x80)
#define INFO (TCM+0x100)
#define DESCRIPTORS (TCM+0x300)
#define BUS 0x90000000U
static UINT8 tcm[4096], *arena;
static UINTN arena_size;
static UINT32 mSharedAddr, mPcieCore, config, asid, reply_id;
static PHYSICAL_ADDRESS reply_buffer;
static PHYSICAL_ADDRESS event_buffer[8];
static UINT32 event_id[8];
static BOOLEAN mChipReady, stopped, mapped;
static unsigned int faults, ioctls, milliseconds;
static const UINT8 *clm_expected;
static UINTN clm_size, clm_uploaded;
enum { BAD_ID=1, BAD_LENGTH=2, BAD_STATUS=4, NO_REPLY=8 };

static void Log(const wchar_t *format, ...) { (void)format; }
static void Ms(UINTN n) { milliseconds += (unsigned int)n; }
static UINT32 Le32(const UINT8 *p) {
    return p[0]|((UINT32)p[1]<<8)|((UINT32)p[2]<<16)|((UINT32)p[3]<<24);
}
static BOOLEAN PexTcmContains(UINT32 p, UINT32 n) {
    return p>=TCM && p-TCM<=sizeof(tcm) && n<=sizeof(tcm)-(p-TCM);
}
static UINT32 TcmRd32(UINT32 p) {
    assert(!(p&3) && PexTcmContains(p,4)); return Le32(tcm+(p-TCM));
}
static void TcmWr32(UINT32 p, UINT32 v) {
    assert(!(p&3) && PexTcmContains(p,4));
    for(unsigned int i=0;i<4;i++) tcm[p-TCM+i]=(UINT8)(v>>(i*8));
}
static UINT32 PexCfgRd(UINT32 b,UINT32 d,UINT32 f,UINT32 r) {
    assert(b==1 && d==0 && f==0 && r==4); return config;
}
static void PexCfgWr(UINT32 b,UINT32 d,UINT32 f,UINT32 r,UINT32 v) {
    (void)PexCfgRd(b,d,f,r); config=v;
}
static UINT32 MmioRead32(UINTN p) { assert(p==0x70019238U);return asid; }
static void MmioWrite32(UINTN p,UINT32 v) { assert(p==0x70019238U);asid=v; }
static EFI_STATUS DmaAllocateBuffer(int type,UINTN pages,VOID **p) {
    assert(type==EfiBootServicesData && !arena);
    arena_size=pages*4096; arena=calloc(1,arena_size); assert(arena);*p=arena;return 0;
}
static EFI_STATUS DmaMap(int op,VOID *p,UINTN *n,PHYSICAL_ADDRESS *bus,VOID **map) {
    assert(op==MapOperationBusMasterCommonBuffer && p==arena && *n==arena_size);
    *bus=BUS;*map=arena;mapped=TRUE;return 0;
}
static EFI_STATUS DmaUnmap(VOID *p) {
    assert(p==arena && mapped && stopped && !(config&4));mapped=FALSE;return 0;
}
static EFI_STATUS DmaFreeBuffer(UINTN pages,VOID *p) {
    assert(p==arena && pages*4096==arena_size && !mapped && stopped);
    free(arena);arena=NULL;return 0;
}
static VOID PexOff(VOID) { stopped=TRUE;mChipReady=FALSE;mSharedAddr=0; }
static BOOLEAN BpWr32(UINT32 p,UINT32 v);
#include "../WifiMsgbuf.h"
#include "../WifiScan.h"

/* Device-side model: resolve only addresses actually published in TCM. */
static UINT8 *bus_pointer(PHYSICAL_ADDRESS p,UINTN n) {
    assert(arena && p>=BUS && p-BUS<=arena_size && n<=arena_size-(UINTN)(p-BUS));
    return arena+(UINTN)(p-BUS);
}
static PHYSICAL_ADDRESS address_at(UINT32 p) {
    return TcmRd32(p)|((PHYSICAL_ADDRESS)TcmRd32(p+4)<<32);
}
static UINT8 *index_pointer(unsigned int group,unsigned int ring) {
    unsigned int width=(TcmRd32(SHARED)&BIT20)?2:4;
    if(TcmRd32(SHARED)&BIT16) return bus_pointer(address_at(INFO+20+group*8)+ring*width,width);
    UINT32 p=TcmRd32(INFO+4+group*4)+ring*4;
    assert(PexTcmContains(p,4));return tcm+(p-TCM);
}
static UINT16 read_index(unsigned int group,unsigned int ring) {
    return MsgLe16(index_pointer(group,ring));
}
static void write_index(unsigned int group,unsigned int ring,UINT16 value) {
    MsgPut16(index_pointer(group,ring),value);
}
static UINT8 *ring_item(unsigned int ring,UINT16 index) {
    UINT32 shape=TcmRd32(DESCRIPTORS+ring*16+4);
    assert(index<(shape&0xffff));
    return bus_pointer(address_at(DESCRIPTORS+ring*16+8)+index*(shape>>16),shape>>16);
}
static void complete(UINT8 type,UINT32 id,UINT16 length) {
    UINT16 producer=read_index(2,0), next=(UINT16)((producer+1)%64);
    assert(next!=read_index(3,0));
    UINT8 *p=ring_item(2,producer); memset(p,0,24);p+=TcmRd32(SHARED+36);
    p[0]=type;MsgPut32(p+4,id);MsgPut16(p+8,(faults&BAD_STATUS)?0xffff:0);
    MsgPut16(p+12,(faults&BAD_LENGTH)?9000:length);write_index(2,0,next);
}
static void put_be32(UINT8 *p,UINT32 value) {
    for(unsigned int i=0;i<4;i++) p[i]=(UINT8)(value>>(24-i*8));
}
static UINT16 scan_packet(UINT8 *p,UINT32 status) {
    UINT16 length=status==8?212:72;
    memset(p,0,length);p[12]=0x88;p[13]=0x6c;p[20]=0x10;p[21]=0x18;p[23]=1;
    put_be32(p+28,69);put_be32(p+32,status);put_be32(p+44,length-72);
    if(status==8) {
        UINT8 *result=p+72,*bss=result+12;
        MsgPut32(result,140);MsgPut16(result+8,0x1234);MsgPut16(result+10,1);
        MsgPut32(bss,109);MsgPut32(bss+4,128);
        memcpy(bss+8,"\x00\x11\x22\x33\x44\x55",6);
        bss[18]=7;memcpy(bss+19,"fixture",7);bss[88]=6;MsgPut16(bss+78,(UINT16)-42);
    }
    return length;
}
static void emit_scan(unsigned int slot,UINT32 status) {
    assert(slot<8 && event_buffer[slot]);
    UINT16 length=scan_packet(bus_pointer(event_buffer[slot],8192)+TcmRd32(SHARED+36),status);
    complete(0xe,event_id[slot],length);event_buffer[slot]=0;
}
static BOOLEAN BpWr32(UINT32 p,UINT32 v) {
    if(p==mPcieCore+0x4c) {assert(v==0 && !(config&4));return TRUE;}
    if(p==mPcieCore+0x144) {assert(v==1 && (config&4));return TRUE;}
    assert(p==mPcieCore+0x140 && v==1 && (config&4) && asid==0);
    while(read_index(1,0)!=read_index(0,0)) {
        UINT16 consumer=read_index(1,0);
        UINT8 *p=ring_item(0,consumer);
        if(p[0]==0xd) {
            UINT32 id=Le32(p+4),slot=id&0xff;
            assert(slot<8 && !event_buffer[slot] && MsgLe16(p+8)==8192);
            event_id[slot]=id;
            event_buffer[slot]=Le32(p+16)|((PHYSICAL_ADDRESS)Le32(p+20)<<32);
        } else if(p[0]==0xb) {
            assert(MsgLe16(p+8)==8192);reply_id=Le32(p+4);
            reply_buffer=Le32(p+16)|((PHYSICAL_ADDRESS)Le32(p+20)<<32);
        } else {
            assert(p[0]==9 && Le32(p+4)==0xfffe && reply_buffer);
            UINT32 cmd=Le32(p+8);UINT16 length=MsgLe16(p+16);
            UINT8 *input=bus_pointer(Le32(p+24)|((PHYSICAL_ADDRESS)Le32(p+28)<<32),MsgLe16(p+14));
            UINT8 *output=bus_pointer(reply_buffer,8192);UINT16 returned;
            if(cmd==1) {assert(length==4);MsgPut32(output,2);returned=4;}
            else if(cmd==262) {assert(!strcmp((char *)input,"ver"));strcpy((char *)output,"BCM4356 model");returned=14;}
            else if(cmd==2 || cmd==49) {assert(Le32(input)==1);returned=0;}
            else {
                assert(cmd==263);returned=0;
                if(!strcmp((char *)input,"event_msgs")) {
                    assert(length==11+18 && input[11+69/8]==(1U<<(69%8)));
                } else if(!strcmp((char *)input,"mpc")) {
                    assert(length==8 && Le32(input+4)==0);
                } else if(!strcmp((char *)input,"clmload")) {
                    UINT8 *chunk=input+8;
                    UINT32 count=Le32(chunk+4);
                    UINT16 flags=0x1000;
                    assert(clm_expected && count>0 && count<=1400 && count<=clm_size-clm_uploaded);
                    if(!clm_uploaded) flags|=2;
                    if(clm_uploaded+count==clm_size) flags|=4;
                    assert(MsgLe16(chunk)==flags && MsgLe16(chunk+2)==2 && Le32(chunk+8)==0);
                    assert(length==8+15+count && !memcmp(chunk+12,clm_expected+clm_uploaded,count));
                    assert(chunk[12+count]==0 && chunk[13+count]==0 && chunk[14+count]==0);
                    clm_uploaded+=count;
                } else {
                    assert(!strcmp((char *)input,"escan") && length==78);
                    UINT8 *scan=input+6;
                    assert(Le32(scan)==1 && MsgLe16(scan+4)==1 && MsgLe16(scan+6)==0x1234);
                    for(unsigned int i=44;i<50;i++) assert(scan[i]==0xff);
                    assert(scan[50]==2 && scan[51]==1 && Le32(scan+68)==0);
                    for(unsigned int i=52;i<68;i+=4) assert(Le32(scan+i)==0xffffffffU);
                    emit_scan(0,8); /* event interleaved before IOCTL completion */
                    emit_scan(1,0);
                }
            }
            ioctls++;
            if(!(faults&NO_REPLY)) {
                complete(0xa,0xfffe,0);
                complete(0xc,reply_id+((faults&BAD_ID)?1:0),returned);
            }
        }
        write_index(1,0,(UINT16)((consumer+1)%64));
    }
    return TRUE;
}
static void reset_model(UINT32 flags) {
    assert(!arena);memset(tcm,0,sizeof(tcm));
    mChipReady=TRUE;mSharedAddr=SHARED;mPcieCore=0x18003000;
    config=6;asid=0x80000003;stopped=mapped=FALSE;reply_buffer=0;
    faults=ioctls=milliseconds=0;
    clm_expected=NULL;clm_size=clm_uploaded=0;
    memset(event_buffer,0,sizeof(event_buffer));memset(event_id,0,sizeof(event_id));
    TcmWr32(SHARED,flags|BIT28);TcmWr32(SHARED+48,INFO);
    TcmWr32(INFO,DESCRIPTORS);TcmWr32(INFO+52,8);
    for(unsigned int i=0;i<4;i++) TcmWr32(INFO+4+i*4,TCM+0x400+i*0x40);
}
static void verify_release(PEX_MSGBUF *m) {
    MsgRelease(m);assert(!arena && !mapped && stopped && asid==0x80000003 && !(config&4));
}
int main(void) {
    const UINT32 modes[]={5,5|BIT16,5|BIT16|BIT20};
    for(unsigned int mode=0;mode<3;mode++) {
        reset_model(modes[mode]);PEX_MSGBUF m={0};assert(MsgInitialize(&m));
        for(unsigned int n=0;n<70;n++) { /* producer and consumer wrap on both rings */
            UINT8 data[4]={0};UINT16 length;
            assert(MsgIoctl(&m,1,data,4,&length));assert(length==4 && Le32(data)==2);
        }
        assert(ioctls==70);verify_release(&m);
        reset_model(modes[mode]);assert(PexControlProbe());
        assert(ioctls==2 && !arena && stopped && asid==0x80000003);
        reset_model(modes[mode]);assert(PexScanProbe(NULL,0));
        assert(ioctls==5 && !arena && stopped && asid==0x80000003);
        UINT8 clm[3001];for(UINTN i=0;i<sizeof(clm);i++) clm[i]=(UINT8)i;
        reset_model(modes[mode]);clm_expected=clm;clm_size=sizeof(clm);
        assert(PexScanProbe(clm,sizeof(clm)) && clm_uploaded==sizeof(clm));
        assert(ioctls==8 && !arena && stopped);
        reset_model(modes[mode]);clm_expected=clm;clm_size=sizeof(clm);faults=BAD_STATUS;
        assert(!PexScanProbe(clm,sizeof(clm)) && clm_uploaded==1400 && ioctls==1);
        assert(!arena && stopped);
    }
    for(unsigned int bit=1;bit<=NO_REPLY;bit<<=1) {
        reset_model(5|BIT16|BIT20);PEX_MSGBUF m={0};assert(MsgInitialize(&m));faults=bit;
        UINT8 data[4]={0};UINT16 length;assert(!MsgIoctl(&m,1,data,4,&length));
        if(bit==NO_REPLY) assert(milliseconds==2000);
        verify_release(&m);
    }
    reset_model(5);PEX_MSGBUF m={0};assert(MsgInitialize(&m));
    write_index(1,0,1);UINT8 message[40]={0};assert(!MsgSubmit(&m,message));
    verify_release(&m);
    reset_model(6);assert(!PexControlProbe() && !arena && !mapped);
    UINT8 packet[212];PEX_SCAN scan={.SyncId=0x1234};
    assert(scan_packet(packet,8)==sizeof(packet));
    assert(ScanEvent(&scan,packet,sizeof(packet)) && scan.Records==1);
    packet[72+12+18]=33;assert(!ScanEvent(&scan,packet,sizeof(packet)));
    scan_packet(packet,8);put_be32(packet+44,1000);assert(!ScanEvent(&scan,packet,sizeof(packet)));
    scan_packet(packet,8);MsgPut16(packet+72+8,0x4321);
    assert(ScanEvent(&scan,packet,sizeof(packet)) && scan.Records==1);
    scan_packet(packet,8);MsgPut16(packet+72+12+116,128);MsgPut32(packet+72+12+120,1);
    assert(!ScanEvent(&scan,packet,sizeof(packet)));
    scan_packet(packet,4);assert(ScanEvent(&scan,packet,72) && scan.Done && scan.Failed);
    UINT8 padded[216]={0};scan_packet(padded,8);put_be32(padded+44,144);
    assert(ScanEvent(&scan,padded,sizeof(padded)));
    puts("msgbuf IOCTL/event/scan wire format, indices, wrap/full, malformed replies, timeout and DMA teardown passed");
}
