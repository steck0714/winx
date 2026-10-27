// Host test of MouseCursorDxe.c: fake GOP framebuffer + fake mouse. Checks that moving the cursor restores the background,
// keeps pixels painted by someone else while the cursor was up, and clips at the screen edges.
#include <stdio.h>
#include <assert.h>
#include "Uefi.h"
#include "Protocol/GraphicsOutput.h"
#include "Protocol/SimplePointer.h"
EFI_GUID gEfiSimplePointerProtocolGuid, gEfiGraphicsOutputProtocolGuid;
typedef struct { void *x; } BS;
static EFI_GRAPHICS_OUTPUT_BLT_PIXEL FB[64*48];
static EFI_GOP_INFO Info = {64, 48};
static EFI_GOP_MODE GMode = {&Info};
static EFI_STATUS GBlt(EFI_GRAPHICS_OUTPUT_PROTOCOL*T, EFI_GRAPHICS_OUTPUT_BLT_PIXEL*B, EFI_GRAPHICS_OUTPUT_BLT_OPERATION Op,
                       UINTN SX, UINTN SY, UINTN DX, UINTN DY, UINTN W, UINTN H, UINTN D) {
  if (!D) D = W*4;
  for (UINTN y=0;y<H;y++) for (UINTN x=0;x<W;x++) {
    if (Op==EfiBltVideoToBltBuffer) { if (SX+W>64||SY+H>48) return 2; ((EFI_GRAPHICS_OUTPUT_BLT_PIXEL*)((UINT8*)B+(DY+y)*D))[DX+x] = FB[(SY+y)*64+SX+x]; }
    else { if (DX+W>64||DY+H>48) return 2; FB[(DY+y)*64+DX+x] = ((EFI_GRAPHICS_OUTPUT_BLT_PIXEL*)((UINT8*)B+(SY+y)*D))[SX+x]; }
  }
  return 0;
}
static EFI_GRAPHICS_OUTPUT_PROTOCOL Gop = {GBlt, &GMode};
static EFI_SIMPLE_POINTER_MODE SMode = {8,8,0,1,1};
static EFI_SIMPLE_POINTER_STATE Next; static int Have;
static EFI_STATUS SGet(EFI_SIMPLE_POINTER_PROTOCOL*T, EFI_SIMPLE_POINTER_STATE*S){ if(!Have) return EFI_NOT_READY; *S=Next; Have=0; return 0; }
static EFI_SIMPLE_POINTER_PROTOCOL Sp = {0,SGet,0,&SMode};
static struct { EFI_STATUS (*LocateProtocol)(EFI_GUID*,void*,void**); EFI_STATUS (*LocateHandleBuffer)(int,EFI_GUID*,void*,UINTN*,EFI_HANDLE**);
  EFI_STATUS (*HandleProtocol)(EFI_HANDLE,EFI_GUID*,void**); EFI_TPL (*RaiseTPL)(EFI_TPL); void (*RestoreTPL)(EFI_TPL); EFI_STATUS (*CreateEvent)(UINT32,UINTN,void*,void*,EFI_EVENT*); EFI_STATUS (*SetTimer)(EFI_EVENT,int,UINT64);} BSv;
static EFI_STATUS LP(EFI_GUID*g,void*r,void**i){*i=&Gop;return 0;}
static int LHB_COUNT=1;
static EFI_STATUS LHB(int t,EFI_GUID*g,void*r,UINTN*c,EFI_HANDLE**h){if(!LHB_COUNT) return 14; *c=1;*h=malloc(sizeof(void*));(*h)[0]=(EFI_HANDLE)1;return 0;}
static EFI_STATUS HP(EFI_HANDLE h,EFI_GUID*g,void**i){*i=&Sp;return 0;}
static EFI_STATUS CE(UINT32 t,UINTN l,void*f,void*c,EFI_EVENT*e){return 0;}
static EFI_STATUS ST(EFI_EVENT e,int t,UINT64 p){return 0;}
static EFI_TPL RT(EFI_TPL t){return 4;}
static void RS(EFI_TPL t){}
#define gBS (&BSv)
#include "../../../Drivers/MouseCursorDxe/MouseCursorDxe.c"
static void Tick(int dx,int dy,int l,int r){ Next.RelativeMovementX=dx; Next.RelativeMovementY=dy; Next.LeftButton=l; Next.RightButton=r; Have=1; PollMouse(0,0); }
static int Cnt(void){ int n=0; for(int i=0;i<64*48;i++) if(FB[i].Red||FB[i].Green||FB[i].Blue) n++; return n; }
static int Diff(EFI_GRAPHICS_OUTPUT_BLT_PIXEL*Ref){ int d=0; for(int i=0;i<64*48;i++) if(memcmp(&Ref[i],&FB[i],3)) d++; return d; }
int main(void){
  BSv.LocateProtocol=LP; BSv.LocateHandleBuffer=LHB; BSv.HandleProtocol=HP; BSv.CreateEvent=CE; BSv.SetTimer=ST; BSv.RaiseTPL=RT; BSv.RestoreTPL=RS;
  for (int i=0;i<64*48;i++){FB[i].Red=10;FB[i].Green=20;FB[i].Blue=30;}
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Ref[64*48]; memcpy(Ref,FB,sizeof FB);
  EFI_SIMPLE_POINTER_STATE St;
  PollMouse(0,0); assert(memcmp(Ref,FB,sizeof FB)==0);                    // hidden until the first event
  assert(Sp.GetState==WrapGetState);                                      // hooked
  Tick(0,0,0,0); assert(memcmp(Ref,FB,sizeof FB)!=0);                    // first event: cursor appears at the centre
  assert(mX==32 && mY==24);

  // consumers still get the events (no stealing): accumulated movement and button state
  assert(Sp.GetState(&Sp,&St)==0 && St.RelativeMovementX==0);            // the (0,0) event itself is "pending" once
  assert(Sp.GetState(&Sp,&St)==EFI_NOT_READY);
  Tick(2,0,0,0); Tick(3,1,1,0);
  assert(Sp.GetState(&Sp,&St)==0);
  assert(St.RelativeMovementX==5 && St.RelativeMovementY==1 && St.LeftButton==1 && St.RightButton==0);
  assert(Sp.GetState(&Sp,&St)==EFI_NOT_READY);

  // acceleration: a fast flick moves further than the raw counts, a slow one is about 1:1
  int x0=mX; Tick(10,0,0,0); int fast=mX-x0; assert(fast>=15 && fast<=17);   // speed 10 -> 1.625x
  Sp.GetState(&Sp,&St);
  // sub-pixel remainder: 16 single-count ticks move 17 px (1.0625x), not 16 and not 0
  mX=10; mFracX=0; x0=mX; for(int i=0;i<16;i++) Tick(1,0,0,0); assert(mX-x0==17);
  Sp.GetState(&Sp,&St);
  // negative direction is symmetric and returns to the start within the remainder
  for(int i=0;i<16;i++) Tick(-1,0,0,0); assert(mX==10 || mX==9 || mX==11);
  Sp.GetState(&Sp,&St);

  // no consumer for > 500 ms: the pending movement is dropped, a later consumer is not hit by one huge jump
  Tick(4,4,0,0);
  for(int i=0;i<30;i++) PollMouse(0,0);
  Tick(1,1,0,0);
  assert(Sp.GetState(&Sp,&St)==0 && St.RelativeMovementX==1 && St.RelativeMovementY==1);

  // foreign paint under the cursor survives the next move
  mX=32; mY=24; mFracX=mFracY=0; Tick(0,0,0,0);
  FB[24*64+43] = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL){1,2,3,0};                // arrow row 0 col 11 is transparent
  Tick(10,0,0,0);
  assert(FB[24*64+43].Blue==1 && FB[24*64+43].Green==2 && FB[24*64+43].Red==3);
  FB[24*64+43]=Ref[24*64+43];

  // clamping and clipping at the edges
  Tick(-500,-500,0,0); assert(mX==0 && mY==0);
  Tick(500,500,1,0);   assert(mX==63 && mY==47);
  Tick(-500,-500,0,1);
  CursorErase();
  printf("pixels differing from background after erase: %d\n", Diff(Ref));
  assert(Diff(Ref)==0);
  Tick(0,0,1,1); assert(mDrawn);

  // video mode change: state is reset and the cursor re-centred, no crash
  Info.HorizontalResolution=32; Info.VerticalResolution=24; Tick(0,0,0,0);
  assert(mX==16 && mY==12);
  Info.HorizontalResolution=64; Info.VerticalResolution=48;

  // device removal: records are dropped
  LHB_COUNT=0; PollMouse(0,0); assert(mRec[0].Sp==NULL);
  printf("mouse cursor host test OK\n");
  return 0;
}
