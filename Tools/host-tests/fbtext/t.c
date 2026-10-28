#include "Uefi.h"
#include <stdio.h>
#include <stdlib.h>
UINTN g_fb_addr;
#include "../../../Library/FbExceptionHandlerLib/FbText.c"
int main(void){
  UINT32 *fb = calloc(768*1280,4); g_fb_addr=(UINTN)fb;
  ExcDebug(0,"\n\nSynchronous Exception at 0x%016lx\nESR 0x96000061 FAR 0x%016lx\n",0x1234abcdUL,0xdeadbeefUL);
  FILE*f=fopen("out.pgm","w"); fprintf(f,"P2\n768 1280\n1\n");
  for(int y=0;y<1280;y++){for(int x=0;x<768;x++)fprintf(f,"%d ",(fb[y*768+x]==0xFFFFFFFFu)?1:0);fprintf(f,"\n");}
  fclose(f); return 0; }
