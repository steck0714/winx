/* SPDX-License-Identifier: MIT
 * Internal ARM64 ETL reader. Usage: trace-export input.etl output.bin seconds
 * Reads an existing trace; CREATE_NEW preserves prior evidence. ProcessTrace
 * supplies UTC FILETIME timestamps and the originating CPU, including classic
 * records whose EventLogReader XML has no CPU or rendering metadata.
 *
 * Little-endian stream: 32-byte file_header (SWETW001), followed by a 48-byte
 * frame and exactly frame.bytes payload bytes per record. header.total counts
 * the full input, while frames retain only its final requested seconds.
 * No event payload is interpreted here. Native preflight compared timestamps,
 * event families and all four CPU numbers against the source ETL.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <shellapi.h>
#include <stdint.h>
#include <string.h>

static UINT64 image_cookie=0x2b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used={
 .Size=sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64),.SecurityCookie=(ULONG_PTR)&image_cookie};
#pragma pack(push,1)
struct file_header { char magic[8];uint64_t total,last;uint32_t seconds,stride; };
struct frame {
 uint64_t time;GUID provider;uint32_t pid,tid;uint16_t cpu,id,flags,bytes;
 uint8_t opcode,version,level,reserved;uint32_t zero;
};
#pragma pack(pop)
_Static_assert(sizeof(struct file_header)==32,"file header");
_Static_assert(sizeof(struct frame)==48,"frame header");
static HANDLE output;
static uint8_t buffer[65536];
static DWORD used,write_error;
static uint64_t total,last,threshold;
static BOOL writing;

static void flush(void)
{
 DWORD offset=0,n;
 while(offset<used && !write_error) {
  if(!WriteFile(output,buffer+offset,used-offset,&n,NULL)) write_error=GetLastError();
  else if(!n) write_error=ERROR_WRITE_FAULT;
  else offset+=n;
 }
 used=0;
}
static void append(const void *data,DWORD bytes)
{
 const uint8_t *p=data;
 while(bytes && !write_error) {
  DWORD n=(DWORD)sizeof(buffer)-used;if(n>bytes)n=bytes;
  memcpy(buffer+used,p,n);used+=n;p+=n;bytes-=n;
  if(used==sizeof(buffer))flush();
 }
}
static void WINAPI event(PEVENT_RECORD record)
{
 uint64_t t=(uint64_t)record->EventHeader.TimeStamp.QuadPart;
 if(!writing) {++total;if(t>last)last=t;return;}
 if(t<threshold || write_error)return;
 struct frame f={0};
 f.time=t;f.provider=record->EventHeader.ProviderId;
 f.pid=record->EventHeader.ProcessId;f.tid=record->EventHeader.ThreadId;
 f.cpu=record->BufferContext.ProcessorNumber;
 f.id=record->EventHeader.EventDescriptor.Id;f.flags=record->EventHeader.Flags;
 f.bytes=record->UserDataLength;f.opcode=record->EventHeader.EventDescriptor.Opcode;
 f.version=record->EventHeader.EventDescriptor.Version;f.level=record->EventHeader.EventDescriptor.Level;
 append(&f,sizeof(f));append(record->UserData,f.bytes);
}
static ULONG read_trace(WCHAR *path)
{
 EVENT_TRACE_LOGFILEW log={0};
 log.LogFileName=path;log.ProcessTraceMode=PROCESS_TRACE_MODE_EVENT_RECORD;
 log.EventRecordCallback=event;
 TRACEHANDLE h=OpenTraceW(&log);
 if(h==INVALID_PROCESSTRACE_HANDLE)return GetLastError();
 ULONG status=ProcessTrace(&h,1,NULL,NULL);CloseTrace(h);return status;
}
void start(void)
{
 int argc;WCHAR **argv=CommandLineToArgvW(GetCommandLineW(),&argc);
 DWORD status;uint32_t seconds=0;
 if(!argv || argc!=4)ExitProcess(ERROR_BAD_ARGUMENTS);
 for(const WCHAR *p=argv[3];*p;++p) {
  if(*p<L'0' || *p>L'9' || seconds>600)ExitProcess(ERROR_BAD_ARGUMENTS);
  seconds=seconds*10+(*p-L'0');
 }
 if(seconds<1 || seconds>600)ExitProcess(ERROR_BAD_ARGUMENTS);
 status=read_trace(argv[1]);if(status)ExitProcess(status);
 threshold=last>(uint64_t)seconds*10000000?last-(uint64_t)seconds*10000000:0;
 output=CreateFileW(argv[2],GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
 if(output==INVALID_HANDLE_VALUE)ExitProcess(GetLastError());
 struct file_header header={{'S','W','E','T','W','0','0','1'},total,last,seconds,sizeof(struct frame)};
 append(&header,sizeof(header));writing=TRUE;
 status=read_trace(argv[1]);flush();
 if(!FlushFileBuffers(output) && !status)status=GetLastError();
 CloseHandle(output);LocalFree(argv);
 if(!status)status=write_error;
 if(!status) {
  const char message[]="ETL raw CPU/header export completed\r\n";DWORD n;
  WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),message,sizeof(message)-1,&n,NULL);
 }
 ExitProcess(status);
}
