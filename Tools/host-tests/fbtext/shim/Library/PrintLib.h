
#include <stdio.h>
static inline UINTN AsciiVSPrint(char*b,UINTN n,const char*f,va_list a){return vsnprintf(b,n,f,a);}
