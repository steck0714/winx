/*
 * Nintendo Switch audio driver (speaker), PortCls WaveRT. Stage 1.
 *
 * ACPI device AUD0 (ACPI\SWOA0C01), UEFI R402+:
 *   Memory32Fixed 0x702D0000 (0x2000): AHUB. ADMAIF at +0x000, XBAR at
 *     +0x800, I2S1 at +0x1000.
 *   Memory32Fixed 0x702E2000 (0x1000): ADMA, channel 0 at +0x000, global
 *     registers at +0xC00.
 *   I2cSerialBus 0x1C on I2C1: the ALC5639 codec (not used yet).
 *
 * UEFI R402 turns on the APE clocks (d_audio, CAR i2s0 for the I2S at
 * 0x702D1000), sets up the codec (speaker path, -18 dB) and leaves this
 * path running: XBAR I2S1 <- ADMAIF1, ADMAIF1 TX (16-bit pack), I2S1 as
 * master, 48 kHz, 16 bit, stereo. The driver only feeds ADMAIF1 through
 * ADMA channel 0 from the WaveRT buffer.
 *
 * Reading an APE register with the APE clocks off hangs the CPU (UEFI R395),
 * so StartDevice first checks the clocks, resets and the APE power partition
 * in CAR and PMC, and fails (Code 10) if UEFI did not prepare them.
 *
 * The WaveRT buffer is split into 8 periods (ADMA SRC_BUF 7, TC = one
 * period), ADMA runs it in continuous mode. ADMA counts finished periods in
 * XFER_STATUS (16 bits); the position inside the current period is estimated
 * from the performance counter (Linux reports the position per period).
 *
 * One render pin, no volume node: Windows scales the volume in software.
 * Diagnostics go to Services\switchaudio\Parameters.
 *
 * 1.0.0.2: 1.0.0.1 showed the speaker but played nothing. The registry now
 * gets the state of ADMA, ADMAIF1, XBAR and I2S1 at the start of the device
 * (Start*), 0 and 100 ms after each stream start (Run0*, Run100*) and at each
 * stop (Stop*, with the periods ADMA finished). Parameters\TestTone = 1 plays
 * a one-second tone from the driver's own buffer at the next device start.
 *
 * 1.0.0.3: the tone played, but Windows opened 58 streams and never ran one.
 * The buffer was MmNonCached, which ARM64 maps as Device memory; the audio
 * engine (audiodg) maps the buffer too and its unaligned or DC ZVA writes
 * fault there. Now MmWriteCombined (Normal non-cacheable: no cache, so ADMA
 * still sees the data). Buffers counts AllocateAudioBuffer calls, StateLog
 * keeps the last eight KS states (one hex digit each, newest last).
 *
 * 1.0.0.4: with 1.0.0.3 Windows took the buffer eight times and closed the
 * stream each time without a single state change. CallLog keeps the last
 * eight calls into the stream (one hex digit each, newest last, repeats
 * folded): 1 NewStream, 2 SetFormat, 3 AllocateAudioBuffer, 4 FreeAudioBuffer,
 * 5 GetHWLatency, 6 GetPositionRegister, 7 GetClockRegister, 8 GetPosition,
 * 9 SetState, A the stream freed. The buffer is now exactly the requested
 * size when that makes periods of at least 1024 bytes (5.3 ms).
 *
 * 1.0.0.5: with the requested size Windows played, but crunchy. XFER_STATUS
 * restarts from 0 when the channel is enabled, and the start value was read
 * before that (0x3FA left over), so the first update jumped 0xFCC0 periods
 * and the position stayed two periods off: Windows wrote where ADMA had
 * already read. The start value is now read after the enable.
 *
 * 1.0.0.6: clean, but with an occasional dropout. A period change was dated
 * at the poll that saw it, up to one engine period (10 ms) late, so the
 * position could lag ADMA by most of a period and Windows wrote where ADMA
 * had already fetched. The change is now dated where the nominal rate puts
 * it, kept between the previous poll and this one. The reported FIFO is one
 * period, for more headroom. Gaps counts polls that missed a whole period.
 *
 * 1.0.0.7: headphones. UEFI R403 powers the codec's headphone path (muted)
 * and makes PV6, the jack switch (low = plugged), a GPIO input. A thread
 * reads PV6 every 200 ms; after two equal readings it unmutes the output
 * the jack picks and mutes the other, over SPB (the I2cSerialBus of AUD0).
 * Windows still shows one endpoint. With an older UEFI (HPO MIX without
 * HPOVOL, or PV6 not a GPIO) the thread is not started: speaker only.
 * Traces: HpoMixer, JackGpio, Jack (1 = headphones), JackSwitches, JackSpb.
 *
 * 1.0.0.8: record codec power, mixer, clock and private bias/tuning state
 * after a jack-route change. This diagnoses Lite speaker silence when ADMA
 * and I2S already advance, without adding work to the render callback.
 *
 * 1.0.0.9: read the pad/clock and digital mixer state for the Lite Linux
 * initialization comparison. Record PCM peak/nonzero counts at existing
 * start/stop snapshots; this reads application audio and generates no tone.
 */

#define INITGUID
/* The STATIC_ GUID initializers of ksmedia.h leave out the Data4 braces. */
#pragma clang diagnostic ignored "-Wmissing-braces"
#include <ntddk.h>
#include <portcls.h>
#include <ksmedia.h>
#include <ntstrsafe.h>
#include <spb.h>

#define POOL_TAG 'duAS'

/* -------------------------------------------------------------- registers */

#define CAR_PHYS                 0x60006000ULL
#define CAR_RST_DEVICES_L        0x004
#define CAR_CLK_OUT_ENB_L        0x010
#define CAR_RST_DEVICES_V        0x358
#define CAR_CLK_OUT_ENB_V        0x360
#define CAR_RST_DEVICES_Y        0x2A4
#define CAR_CLK_OUT_ENB_Y        0x298
#define CAR_L_I2S0               (1u << 30)
#define CAR_V_D_AUDIO            (1u << 10)
#define CAR_Y_APE                (1u << 6)
#define PMC_PHYS                 0x7000E400ULL
#define PMC_PWRGATE_STATUS       0x38
#define PMC_PG_AUD               (1u << 27)

#define AHUB_LENGTH              0x2000
#define ADMA_LENGTH              0x1000
#define ADMAIF_TX1_ENABLE        0x300
#define ADMAIF_TX1_STATUS        0x30C
#define ADMAIF_TX1_ACIF_CTRL     0x320
#define ADMAIF_GLOBAL_ENABLE     0x700
#define XBAR_I2S1_RX_MUX         0x840
#define I2S1_RX_ENABLE           0x1000
#define I2S1_RX_STATUS           0x100C
#define I2S1_ENABLE              0x1080
#define I2S1_CTRL                0x10A0
#define I2S1_RX_CIF_FIFO         0x1034
#define I2S1_STATUS              0x108C
#define ADMAIF_TX1_INT_STATUS    0x310

#define ADMA_CH_CMD              0x00
#define ADMA_CH_STATUS           0x0C
#define ADMA_CH_STATUS_PAUSED    (1u << 1)
#define ADMA_CH_CTRL             0x24
#define ADMA_CH_CTRL_PAUSE       (1u << 0)
#define ADMA_CH_CONFIG           0x28
#define ADMA_CH_FIFO_CTRL        0x2C
#define ADMA_CH_LOWER_SRC        0x34
#define ADMA_CH_LOWER_TRG        0x3C
#define ADMA_CH_TC               0x44
#define ADMA_CH_XFER_STATUS      0x54
#define ADMA_GLOBAL_CMD          0xC00
/* Request ADMAIF1 TX, memory to AHUB, continuous, flow control (as UEFI R398). */
#define ADMA_CTRL_TX1            ((1u << 28) | (4u << 12) | (2u << 8) | (1u << 1))
/* SRC_BUF = periods - 1, burst 16 words, WRR weight 1. */
#define ADMA_CONFIG(periods)     ((((periods) - 1u) << 28) | (5u << 20) | 1u)
#define ADMA_FIFO_TX3            (3u << 8)

#define SA_RATE                  48000u
#define SA_CHANNELS              2u
#define SA_BITS                  16u
#define SA_FRAME                 (SA_CHANNELS * SA_BITS / 8u)
#define SA_BYTES_PER_SEC         (SA_RATE * SA_FRAME)
#define SA_PERIODS               8u
#define SA_PERIOD_ALIGN          64u            /* one ADMA burst */
#define SA_MIN_PERIOD            1024u          /* 256 frames, 5.3 ms */

/* ------------------------------------------------------------ the hardware */

/* 1.0.0.7: the jack switch and the codec registers for the outputs. */
#define GPIO_PHYS                0x6000D000ull
#define GPIO_LENGTH              0x1000u
#define GPIO_PV_CNF              0x504u          /* port V: bank 5, port 1 */
#define GPIO_PV_OE               0x514u
#define GPIO_PV_IN               0x534u
#define GPIO_PV6                 (1u << 6)
#define RT5640_SPK_VOL           0x01
#define RT5640_HP_VOL            0x02
#define RT5640_HPO_MIXER         0x45
#define RT5640_M_HPVOL_HM        (1u << 13)
#define SA_SPK_VOL               0x1414u         /* -18 dB, as UEFI R402 */
#define SA_HP_VOL                0x1818u         /* -24 dB, as UEFI R403 */
#define SA_MUTE                  0x8080u

typedef struct _SA_HW {
    volatile UCHAR *Ahub;
    volatile UCHAR *Adma;
    KSPIN_LOCK Lock;
    BOOLEAN Ready;
    HANDLE Spb;                     /* the codec on I2C1 */
    volatile UCHAR *Gpio;
    PKTHREAD JackThread;
    KEVENT JackStop;
} SA_HW;

static SA_HW g_Hw;
static WCHAR g_ParamsPath[] = L"switchaudio\\Parameters";
static LONG g_Buffers;
static ULONG g_StateLog;
static ULONG g_CallLog;

static ULONG RdAhub(ULONG Offset) { return READ_REGISTER_ULONG((volatile ULONG *)(g_Hw.Ahub + Offset)); }
static VOID WrAhub(ULONG Offset, ULONG Value) { WRITE_REGISTER_ULONG((volatile ULONG *)(g_Hw.Ahub + Offset), Value); }
static ULONG RdAdma(ULONG Offset) { return READ_REGISTER_ULONG((volatile ULONG *)(g_Hw.Adma + Offset)); }
static VOID WrAdma(ULONG Offset, ULONG Value) { WRITE_REGISTER_ULONG((volatile ULONG *)(g_Hw.Adma + Offset), Value); }

static VOID SaTrace(PCWSTR Name, ULONG Value)
{
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, g_ParamsPath, Name, REG_DWORD, &Value, sizeof(Value));
    }
}

static ULONG ReadPhys(ULONGLONG Address)
{
    PHYSICAL_ADDRESS Pa;
    volatile ULONG *Va;
    ULONG Value;
    Pa.QuadPart = (LONGLONG)Address;
    Va = (volatile ULONG *)MmMapIoSpaceEx(Pa, sizeof(ULONG), PAGE_READWRITE | PAGE_NOCACHE);
    if (Va == NULL) {
        return 0;
    }
    Value = READ_REGISTER_ULONG(Va);
    MmUnmapIoSpace((PVOID)Va, sizeof(ULONG));
    return Value;
}

/* Bit 0: APE partition on, 1: d_audio on and out of reset, 2: APE clock,
   3: I2S (CAR i2s0) clock. All four must be set before touching the AHUB. */
static ULONG SaCheckApe(VOID)
{
    ULONG Ok = 0;
    if ((ReadPhys(PMC_PHYS + PMC_PWRGATE_STATUS) & PMC_PG_AUD) != 0) Ok |= 1;
    if ((ReadPhys(CAR_PHYS + CAR_CLK_OUT_ENB_V) & CAR_V_D_AUDIO) != 0 &&
        (ReadPhys(CAR_PHYS + CAR_RST_DEVICES_V) & CAR_V_D_AUDIO) == 0) Ok |= 2;
    if ((ReadPhys(CAR_PHYS + CAR_CLK_OUT_ENB_Y) & CAR_Y_APE) != 0 &&
        (ReadPhys(CAR_PHYS + CAR_RST_DEVICES_Y) & CAR_Y_APE) == 0) Ok |= 4;
    if ((ReadPhys(CAR_PHYS + CAR_CLK_OUT_ENB_L) & CAR_L_I2S0) != 0 &&
        (ReadPhys(CAR_PHYS + CAR_RST_DEVICES_L) & CAR_L_I2S0) == 0) Ok |= 8;
    return Ok;
}

static VOID SaAdmaStop(VOID)
{
    if (g_Hw.Adma != NULL) {
        WrAdma(ADMA_CH_CMD, 0);
    }
}

static VOID SaTraceAs(PCWSTR Prefix, PCWSTR Name, ULONG Value)
{
    WCHAR Full[48];
    if (NT_SUCCESS(RtlStringCchPrintfW(Full, ARRAYSIZE(Full), L"%s%s", Prefix, Name))) {
        SaTrace(Full, Value);
    }
}

/* 1.0.0.4: one hex digit per call into the stream, repeats folded. */
static VOID SaCall(ULONG Code)
{
    ULONG Old = g_CallLog, New;
    if ((Old & 0xF) == Code) {
        return;
    }
    New = (Old << 4) | Code;
    if (InterlockedCompareExchange((volatile LONG *)&g_CallLog, (LONG)New, (LONG)Old) == (LONG)Old &&
        KeGetCurrentIrql() == PASSIVE_LEVEL) {
        SaTrace(L"CallLog", New);
    }
}

/* 1.0.0.2: the state of the whole path, PASSIVE_LEVEL only. */
static VOID SaSnapshot(PCWSTR Prefix)
{
    if (g_Hw.Ahub == NULL || g_Hw.Adma == NULL) {
        return;
    }
    SaTraceAs(Prefix, L"AdmaStatus", RdAdma(ADMA_CH_STATUS));
    SaTraceAs(Prefix, L"AdmaXfer", RdAdma(ADMA_CH_XFER_STATUS));
    SaTraceAs(Prefix, L"AdmaSrc", RdAdma(ADMA_CH_LOWER_SRC));
    SaTraceAs(Prefix, L"AdmaTc", RdAdma(ADMA_CH_TC));
    SaTraceAs(Prefix, L"AdmaGlobal", RdAdma(ADMA_GLOBAL_CMD));
    SaTraceAs(Prefix, L"AdmaifEnable", RdAhub(ADMAIF_TX1_ENABLE));
    SaTraceAs(Prefix, L"AdmaifStatus", RdAhub(ADMAIF_TX1_STATUS));
    SaTraceAs(Prefix, L"AdmaifInt", RdAhub(ADMAIF_TX1_INT_STATUS));
    SaTraceAs(Prefix, L"Xbar", RdAhub(XBAR_I2S1_RX_MUX));
    SaTraceAs(Prefix, L"I2sEnable", RdAhub(I2S1_ENABLE));
    SaTraceAs(Prefix, L"I2sRxEnable", RdAhub(I2S1_RX_ENABLE));
    SaTraceAs(Prefix, L"I2sRxStatus", RdAhub(I2S1_RX_STATUS));
    SaTraceAs(Prefix, L"I2sStatus", RdAhub(I2S1_STATUS));
    SaTraceAs(Prefix, L"I2sRxFifo", RdAhub(I2S1_RX_CIF_FIFO));
}

static VOID SaSleepMs(ULONG Ms)
{
    LARGE_INTEGER Due;
    Due.QuadPart = -(LONGLONG)Ms * 10000;
    (VOID)KeDelayExecutionThread(KernelMode, FALSE, &Due);
}

static ULONG SaReadParam(PCWSTR Name)
{
    RTL_QUERY_REGISTRY_TABLE Table[2];
    ULONG Value = 0;
    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_REQUIRED | RTL_QUERY_REGISTRY_TYPECHECK;
    Table[0].Name = (PWSTR)Name;
    Table[0].EntryContext = &Value;
    Table[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_NONE;
    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_SERVICES, g_ParamsPath, Table, NULL, NULL))) {
        return 0;
    }
    return Value;
}

/* 1.0.0.2: Parameters\TestTone = 1 plays one second of 1 kHz (-18 dBFS) at the
   next start, from the driver's own buffer: checks the hardware path without
   the Windows audio engine. The value is cleared afterwards. */
static VOID SaTestTone(VOID)
{
    PHYSICAL_ADDRESS Low, High, Skip;
    ULONG *Words;
    ULONG Index;
    /* One 1 kHz period at 48 kHz, 1/8 of full scale (the UEFI R400 tone). */
    static const SHORT Sine[48] = {
            0,   535,  1060,  1567,  2048,  2493,  2896,  3250,  3547,  3784,  3956,  4061,
         4096,  4061,  3956,  3784,  3547,  3250,  2896,  2493,  2048,  1567,  1060,   535,
            0,  -535, -1060, -1567, -2048, -2493, -2896, -3250, -3547, -3784, -3956, -4061,
        -4096, -4061, -3956, -3784, -3547, -3250, -2896, -2493, -2048, -1567, -1060,  -535,
    };

    SaTrace(L"TestTone", 0);
    Low.QuadPart = 0;
    High.QuadPart = 0xFFFFFFFFLL;
    Skip.QuadPart = 0;
    Words = (ULONG *)MmAllocateContiguousNodeMemory(PAGE_SIZE, Low, High, Skip, PAGE_READWRITE | PAGE_NOCACHE,
                                                    MM_ANY_NODE_OK);
    if (Words == NULL) {
        SaTrace(L"ToneResult", 1);
        return;
    }
    for (Index = 0; Index < 48; ++Index) {
        Words[Index] = ((ULONG)(USHORT)Sine[Index] << 16) | (USHORT)Sine[Index];
    }
    SaSnapshot(L"ToneBefore");
    WrAhub(ADMAIF_GLOBAL_ENABLE, 1);
    WrAhub(ADMAIF_TX1_ENABLE, 1);
    WrAhub(I2S1_RX_ENABLE, 1);
    WrAhub(I2S1_ENABLE, 1);
    WrAdma(ADMA_GLOBAL_CMD, 1);
    WrAdma(ADMA_CH_CMD, 0);
    WrAdma(ADMA_CH_TC, 48 * sizeof(ULONG));
    WrAdma(ADMA_CH_CTRL, ADMA_CTRL_TX1);
    WrAdma(ADMA_CH_LOWER_SRC, (ULONG)MmGetPhysicalAddress(Words).QuadPart);
    WrAdma(ADMA_CH_LOWER_TRG, 0);
    WrAdma(ADMA_CH_FIFO_CTRL, ADMA_FIFO_TX3);
    WrAdma(ADMA_CH_CONFIG, ADMA_CONFIG(1));
    WrAdma(ADMA_CH_CMD, 1);
    SaSleepMs(500);
    SaSnapshot(L"Tone");
    SaSleepMs(500);
    SaAdmaStop();
    SaSleepMs(5);
    MmFreeContiguousMemory(Words);
    SaTrace(L"ToneResult", 2);
}

/* ------------------------------------------------------------ C++ support */

void *__cdecl operator new(size_t Size, POOL_FLAGS Flags, ULONG Tag)
{
    return ExAllocatePool2(Flags, Size, Tag);
}
void __cdecl operator delete(void *P) { if (P != NULL) ExFreePoolWithTag(P, POOL_TAG); }
void __cdecl operator delete(void *P, size_t) { if (P != NULL) ExFreePoolWithTag(P, POOL_TAG); }
extern "C" int __cdecl _purecall(void) { KeBugCheckEx(0xDEADDEAD, 0x5A, 0, 0, 0); }

#define SA_NEW new (POOL_FLAG_NON_PAGED, POOL_TAG)

/* ------------------------------------------------------- filter descriptors */

static KSDATARANGE_AUDIO g_PcmRange = {
    { sizeof(KSDATARANGE_AUDIO), 0, 0, 0,
      STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO), STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
      STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) },
    SA_CHANNELS, SA_BITS, SA_BITS, SA_RATE, SA_RATE
};
static PKSDATARANGE g_PcmRanges[] = { (PKSDATARANGE)&g_PcmRange };

static KSDATARANGE g_BridgeRange = {
    sizeof(KSDATARANGE), 0, 0, 0,
    STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO), STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
    STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
};
static PKSDATARANGE g_BridgeRanges[] = { &g_BridgeRange };

/* Wave: pin 0 = render sink (from the audio engine), pin 1 = bridge out. */
static PCPIN_DESCRIPTOR g_WavePins[] = {
    { 1, 1, 0, NULL,
      { 0, NULL, 0, NULL, ARRAYSIZE(g_PcmRanges), g_PcmRanges, KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_SINK,
        &KSCATEGORY_AUDIO, NULL, { 0 } } },
    { 0, 0, 0, NULL,
      { 0, NULL, 0, NULL, ARRAYSIZE(g_BridgeRanges), g_BridgeRanges, KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE,
        &KSCATEGORY_AUDIO, NULL, { 0 } } },
};
static PCCONNECTION_DESCRIPTOR g_WaveConnections[] = { { PCFILTER_NODE, 0, PCFILTER_NODE, 1 } };
static GUID g_WaveCategories[3];
static PCFILTER_DESCRIPTOR g_WaveFilter = {
    0, NULL, sizeof(PCPIN_DESCRIPTOR), ARRAYSIZE(g_WavePins), g_WavePins,
    sizeof(PCNODE_DESCRIPTOR), 0, NULL, ARRAYSIZE(g_WaveConnections), g_WaveConnections,
    ARRAYSIZE(g_WaveCategories), g_WaveCategories
};

/* Topology: pin 0 = bridge in (from the wave filter), pin 1 = the speaker. */
static PCPIN_DESCRIPTOR g_TopoPins[] = {
    { 0, 0, 0, NULL,
      { 0, NULL, 0, NULL, ARRAYSIZE(g_BridgeRanges), g_BridgeRanges, KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE,
        &KSCATEGORY_AUDIO, NULL, { 0 } } },
    { 0, 0, 0, NULL,
      { 0, NULL, 0, NULL, ARRAYSIZE(g_BridgeRanges), g_BridgeRanges, KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE,
        &KSNODETYPE_SPEAKER, NULL, { 0 } } },
};
static PCCONNECTION_DESCRIPTOR g_TopoConnections[] = { { PCFILTER_NODE, 0, PCFILTER_NODE, 1 } };
static GUID g_TopoCategories[2];
static PCFILTER_DESCRIPTOR g_TopoFilter = {
    0, NULL, sizeof(PCPIN_DESCRIPTOR), ARRAYSIZE(g_TopoPins), g_TopoPins,
    sizeof(PCNODE_DESCRIPTOR), 0, NULL, ARRAYSIZE(g_TopoConnections), g_TopoConnections,
    ARRAYSIZE(g_TopoCategories), g_TopoCategories
};

static VOID SaInitDescriptors(VOID)
{
    g_WaveCategories[0] = KSCATEGORY_AUDIO;
    g_WaveCategories[1] = KSCATEGORY_RENDER;
    g_WaveCategories[2] = KSCATEGORY_REALTIME;
    g_TopoCategories[0] = KSCATEGORY_AUDIO;
    g_TopoCategories[1] = KSCATEGORY_TOPOLOGY;
}

static BOOLEAN SaFormatOk(PKSDATAFORMAT Format)
{
    PWAVEFORMATEX Wfx;
    if (Format == NULL || Format->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX) ||
        !IsEqualGUIDAligned(Format->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(Format->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) {
        return FALSE;
    }
    Wfx = &((PKSDATAFORMAT_WAVEFORMATEX)Format)->WaveFormatEx;
    if (Wfx->nChannels != SA_CHANNELS || Wfx->nSamplesPerSec != SA_RATE || Wfx->wBitsPerSample != SA_BITS) {
        return FALSE;
    }
    if (Wfx->wFormatTag == WAVE_FORMAT_PCM) {
        return TRUE;
    }
    if (Wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        Format->FormatSize >= sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE)) {
        PWAVEFORMATEXTENSIBLE Ext = (PWAVEFORMATEXTENSIBLE)Wfx;
        return IsEqualGUIDAligned(Ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) && Ext->Samples.wValidBitsPerSample == SA_BITS;
    }
    return FALSE;
}

/* ------------------------------------------------------------- the stream */

class CSaStream final : public IMiniportWaveRTStream {
public:
    LONG m_Ref;
    PPORTWAVERTSTREAM m_PortStream;
    PMDL m_Mdl;
    ULONG m_BufferSize;
    ULONG m_Period;
    ULONG m_Phys;
    KSSTATE m_State;
    BOOLEAN m_Started;
    ULONG m_LastXfer;
    ULONGLONG m_Periods;            /* finished periods since the start */
    LONGLONG m_LastChange;          /* performance counter at the last period change */
    LONGLONG m_Frequency;
    ULONG m_LastOffset;
    ULONG m_Runs;
    ULONG m_PosCalls;
    LONGLONG m_LastPoll;            /* performance counter at the last look at XFER_STATUS */
    ULONG m_Gaps;                   /* polls that saw more than one period finish */

    CSaStream(PPORTWAVERTSTREAM PortStream)
        : m_Ref(1), m_PortStream(PortStream), m_Mdl(NULL), m_BufferSize(0), m_Period(0), m_Phys(0),
          m_State(KSSTATE_STOP), m_Started(FALSE), m_LastXfer(0), m_Periods(0), m_LastChange(0),
          m_Frequency(1), m_LastOffset(0), m_Runs(0), m_PosCalls(0), m_LastPoll(0), m_Gaps(0)
    {
        m_PortStream->AddRef();
    }

    STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID Iid, PVOID *Object)
    {
        if (IsEqualGUIDAligned(Iid, IID_IUnknown) || IsEqualGUIDAligned(Iid, IID_IMiniportWaveRTStream)) {
            *Object = static_cast<IMiniportWaveRTStream *>(this);
            AddRef();
            return STATUS_SUCCESS;
        }
        *Object = NULL;
        return STATUS_INVALID_PARAMETER;
    }
    STDMETHODIMP_(ULONG) AddRef() { return (ULONG)InterlockedIncrement(&m_Ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG Ref = InterlockedDecrement(&m_Ref);
        if (Ref == 0) {
            KIRQL Irql;
            KeAcquireSpinLock(&g_Hw.Lock, &Irql);
            if (m_Started) {
                SaAdmaStop();
                m_Started = FALSE;
            }
            KeReleaseSpinLock(&g_Hw.Lock, Irql);
            m_PortStream->Release();
            delete this;
            SaCall(0xA);
        }
        return (ULONG)Ref;
    }

    IMP_IMiniportWaveRTStream;

    /* Called with g_Hw.Lock held. */
    VOID UpdatePeriods(LONGLONG Now)
    {
        ULONG Xfer = RdAdma(ADMA_CH_XFER_STATUS) & 0xFFFF;
        ULONG Delta = (Xfer - m_LastXfer) & 0xFFFF;
        if (Delta != 0) {
            /* Where the nominal rate puts the change, but between the polls. */
            LONGLONG Expected = m_LastChange +
                (LONGLONG)((ULONGLONG)Delta * m_Period * (ULONGLONG)m_Frequency / SA_BYTES_PER_SEC);
            if (Expected > Now) Expected = Now;
            if (Expected < m_LastPoll) Expected = m_LastPoll;
            if (Delta > 1) ++m_Gaps;
            m_Periods += Delta;
            m_LastXfer = Xfer;
            m_LastChange = Expected;
        }
        m_LastPoll = Now;
    }

    /* Existing passive-level start/stop diagnostics only, never GetPosition. */
    VOID SnapshotPcm(PCWSTR Prefix)
    {
        if (m_Mdl == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) return;
        volatile SHORT *Samples = (volatile SHORT *)m_PortStream->MapAllocatedPages(m_Mdl, MmWriteCombined);
        if (Samples == NULL) { SaTraceAs(Prefix, L"PcmMapped", 0); return; }
        ULONG Count = m_BufferSize / sizeof(SHORT), Peak = 0, Nonzero = 0;
        if (Count > 32768) Count = 32768;
        for (ULONG I = 0; I < Count; ++I) {
            LONG Sample = Samples[I];
            ULONG Magnitude = (ULONG)(Sample < 0 ? -Sample : Sample);
            if (Magnitude != 0) ++Nonzero;
            if (Magnitude > Peak) Peak = Magnitude;
        }
        m_PortStream->UnmapAllocatedPages((PVOID)Samples, m_Mdl);
        SaTraceAs(Prefix, L"PcmMapped", 1);
        SaTraceAs(Prefix, L"PcmSamples", Count);
        SaTraceAs(Prefix, L"PcmPeak", Peak);
        SaTraceAs(Prefix, L"PcmNonzero", Nonzero);
    }
};

STDMETHODIMP_(NTSTATUS) CSaStream::SetFormat(PKSDATAFORMAT DataFormat)
{
    SaCall(2);
    return SaFormatOk(DataFormat) ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

STDMETHODIMP_(NTSTATUS) CSaStream::SetState(KSSTATE State)
{
    KIRQL Irql;
    LARGE_INTEGER Freq;
    LONGLONG Now = KeQueryPerformanceCounter(&Freq).QuadPart;
    ULONG Wait;
    BOOLEAN Starting = FALSE, Stopping = FALSE;
    ULONGLONG StoppedPeriods = 0;

    SaCall(9);

    KeAcquireSpinLock(&g_Hw.Lock, &Irql);
    switch (State) {
    case KSSTATE_RUN:
        if (m_Mdl == NULL || m_Period == 0) {
            break;
        }
        if (!m_Started) {
            /* The path UEFI left running; make sure it still is. */
            WrAhub(ADMAIF_GLOBAL_ENABLE, 1);
            WrAhub(ADMAIF_TX1_ENABLE, 1);
            WrAhub(I2S1_RX_ENABLE, 1);
            WrAhub(I2S1_ENABLE, 1);
            WrAdma(ADMA_GLOBAL_CMD, 1);
            WrAdma(ADMA_CH_CMD, 0);
            WrAdma(ADMA_CH_TC, m_Period);
            WrAdma(ADMA_CH_CTRL, ADMA_CTRL_TX1);
            WrAdma(ADMA_CH_LOWER_SRC, m_Phys);
            WrAdma(ADMA_CH_LOWER_TRG, 0);
            WrAdma(ADMA_CH_FIFO_CTRL, ADMA_FIFO_TX3);
            WrAdma(ADMA_CH_CONFIG, ADMA_CONFIG(SA_PERIODS));
            m_Periods = 0;
            m_LastOffset = 0;
            m_Frequency = Freq.QuadPart;
            m_LastChange = Now;
            m_LastPoll = Now;
            m_Gaps = 0;
            WrAdma(ADMA_CH_CMD, 1);
            /* XFER_STATUS restarts from 0 on the enable: read it after. */
            m_LastXfer = RdAdma(ADMA_CH_XFER_STATUS) & 0xFFFF;
            m_Started = TRUE;
            Starting = TRUE;
        } else {
            WrAdma(ADMA_CH_CTRL, ADMA_CTRL_TX1);       /* pause bit off */
            m_LastChange = Now;
            m_LastPoll = Now;
        }
        break;
    case KSSTATE_PAUSE:
        if (m_Started && m_State == KSSTATE_RUN) {
            UpdatePeriods(Now);
            WrAdma(ADMA_CH_CTRL, ADMA_CTRL_TX1 | ADMA_CH_CTRL_PAUSE);
            for (Wait = 0; Wait < 100 && (RdAdma(ADMA_CH_STATUS) & ADMA_CH_STATUS_PAUSED) == 0; ++Wait) {
                KeStallExecutionProcessor(10);
            }
        }
        break;
    case KSSTATE_STOP:
    case KSSTATE_ACQUIRE:
        if (m_Started) {
            UpdatePeriods(Now);
            StoppedPeriods = m_Periods;
            Stopping = TRUE;
            SaAdmaStop();
            m_Started = FALSE;
        }
        m_Periods = 0;
        m_LastOffset = 0;
        break;
    default:
        break;
    }
    m_State = State;
    g_StateLog = (g_StateLog << 4) | ((ULONG)State & 0xF);
    KeReleaseSpinLock(&g_Hw.Lock, Irql);
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        SaTrace(L"StateLog", g_StateLog);
    }

    /* 1.0.0.2: what each start and stop looked like (KS state changes come at PASSIVE_LEVEL). */
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        if (State == KSSTATE_RUN && Starting) {
            SaTrace(L"Runs", ++m_Runs);
            SaSnapshot(L"Run0");
            SaSleepMs(100);
            SaSnapshot(L"Run100");
            SnapshotPcm(L"Run100");
        } else if (State == KSSTATE_STOP || State == KSSTATE_ACQUIRE) {
            if (Stopping) {
                SaTrace(L"StopPeriods", (ULONG)StoppedPeriods);
                SaTrace(L"StopPositionCalls", m_PosCalls);
                SaTrace(L"StopGaps", m_Gaps);
                SaSnapshot(L"Stop");
                SnapshotPcm(L"Stop");
            }
        }
    }
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CSaStream::GetPosition(PKSAUDIO_POSITION Position)
{
    KIRQL Irql;
    ULONG Offset = 0;

    SaCall(8);
    KeAcquireSpinLock(&g_Hw.Lock, &Irql);
    ++m_PosCalls;
    if (m_Started && m_Period != 0) {
        LONGLONG Now = KeQueryPerformanceCounter(NULL).QuadPart;
        ULONGLONG Inside = 0;
        UpdatePeriods(Now);
        if (m_State == KSSTATE_RUN && m_Frequency > 0) {
            Inside = (ULONGLONG)(Now - m_LastChange) * SA_BYTES_PER_SEC / (ULONGLONG)m_Frequency;
            Inside -= Inside % SA_FRAME;
            if (Inside > m_Period - SA_FRAME) {
                Inside = m_Period - SA_FRAME;
            }
        }
        Offset = (ULONG)((m_Periods % SA_PERIODS) * m_Period + Inside);
        /* Never step back inside the same period (a late counter update). */
        if (Offset < m_LastOffset && m_LastOffset - Offset < m_Period) {
            Offset = m_LastOffset;
        }
        m_LastOffset = Offset;
    }
    KeReleaseSpinLock(&g_Hw.Lock, Irql);
    Position->PlayOffset = Offset;
    Position->WriteOffset = (Offset + m_Period) % (m_BufferSize != 0 ? m_BufferSize : 1);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CSaStream::AllocateAudioBuffer(ULONG RequestedSize, PMDL *AudioBufferMdl, ULONG *ActualSize,
                                                       ULONG *OffsetFromFirstPage, MEMORY_CACHING_TYPE *CacheType)
{
    PHYSICAL_ADDRESS Low, High, Phys;
    ULONG Period = (RequestedSize + SA_PERIODS - 1) / SA_PERIODS;
    PMDL Mdl;
    PVOID Va;

    SaCall(3);
    if (m_Mdl != NULL) {
        return STATUS_DEVICE_BUSY;
    }
    if (Period < SA_MIN_PERIOD) Period = SA_MIN_PERIOD;
    Period = (Period + SA_PERIOD_ALIGN - 1) & ~(SA_PERIOD_ALIGN - 1);   /* 64 bytes: whole frames too */
    Low.QuadPart = 0;
    High.QuadPart = 0xFFFFFFFFLL;                  /* ADMA takes 32-bit addresses */
    Mdl = m_PortStream->AllocateContiguousPagesForMdl(Low, High, (SIZE_T)Period * SA_PERIODS);
    if (Mdl == NULL) {
        SaTrace(L"BufferFail", Period * SA_PERIODS);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Phys = m_PortStream->GetPhysicalPageAddress(Mdl, 0);
    Va = m_PortStream->MapAllocatedPages(Mdl, MmWriteCombined);
    if (Va != NULL) {
        RtlZeroMemory(Va, (SIZE_T)Period * SA_PERIODS);
        m_PortStream->UnmapAllocatedPages(Va, Mdl);
    }
    m_Mdl = Mdl;
    m_Period = Period;
    m_BufferSize = Period * SA_PERIODS;
    m_Phys = (ULONG)Phys.QuadPart;
    *AudioBufferMdl = Mdl;
    *ActualSize = m_BufferSize;
    *OffsetFromFirstPage = 0;
    *CacheType = MmWriteCombined;
    SaTrace(L"Buffers", (ULONG)InterlockedIncrement(&g_Buffers));
    SaTrace(L"Requested", RequestedSize);
    SaTrace(L"BufferSize", m_BufferSize);
    SaTrace(L"BufferPhys", m_Phys);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(VOID) CSaStream::FreeAudioBuffer(PMDL AudioBufferMdl, ULONG BufferSize)
{
    UNREFERENCED_PARAMETER(BufferSize);
    SaCall(4);
    if (AudioBufferMdl != NULL) {
        KIRQL Irql;
        KeAcquireSpinLock(&g_Hw.Lock, &Irql);
        if (m_Started) {
            SaAdmaStop();
            m_Started = FALSE;
        }
        KeReleaseSpinLock(&g_Hw.Lock, Irql);
        m_PortStream->FreePagesFromMdl(AudioBufferMdl);
    }
    m_Mdl = NULL;
    m_Period = 0;
    m_BufferSize = 0;
}

STDMETHODIMP_(VOID) CSaStream::GetHWLatency(KSRTAUDIO_HWLATENCY *hwLatency)
{
    SaCall(5);
    /* ADMA FIFO plus ADMAIF, and the position moving in period steps. */
    hwLatency->FifoSize = m_Period > 768 ? m_Period : 768;
    hwLatency->ChipsetDelay = 0;
    hwLatency->CodecDelay = 0;
}

STDMETHODIMP_(NTSTATUS) CSaStream::GetPositionRegister(KSRTAUDIO_HWREGISTER *Register)
{
    SaCall(6);
    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_SUPPORTED;
}

STDMETHODIMP_(NTSTATUS) CSaStream::GetClockRegister(KSRTAUDIO_HWREGISTER *Register)
{
    SaCall(7);
    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_SUPPORTED;
}

/* --------------------------------------------------------- wave miniport */

class CSaWave final : public IMiniportWaveRT {
public:
    LONG m_Ref;
    LONG m_Streams;
    CSaWave() : m_Ref(1), m_Streams(0) {}

    STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID Iid, PVOID *Object)
    {
        if (IsEqualGUIDAligned(Iid, IID_IUnknown) || IsEqualGUIDAligned(Iid, IID_IMiniport) ||
            IsEqualGUIDAligned(Iid, IID_IMiniportWaveRT)) {
            *Object = static_cast<IMiniportWaveRT *>(this);
            AddRef();
            return STATUS_SUCCESS;
        }
        *Object = NULL;
        return STATUS_INVALID_PARAMETER;
    }
    STDMETHODIMP_(ULONG) AddRef() { return (ULONG)InterlockedIncrement(&m_Ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG Ref = InterlockedDecrement(&m_Ref);
        if (Ref == 0) delete this;
        return (ULONG)Ref;
    }

    IMP_IMiniportWaveRT;
};

STDMETHODIMP_(NTSTATUS) CSaWave::GetDescription(PPCFILTER_DESCRIPTOR *Description)
{
    *Description = &g_WaveFilter;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CSaWave::DataRangeIntersection(ULONG PinId, PKSDATARANGE DataRange, PKSDATARANGE MatchingDataRange,
                                                       ULONG OutputBufferLength, PVOID ResultantFormat,
                                                       PULONG ResultantFormatLength)
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(MatchingDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    UNREFERENCED_PARAMETER(ResultantFormatLength);
    return STATUS_NOT_IMPLEMENTED;                 /* PortCls's default handler */
}

STDMETHODIMP_(NTSTATUS) CSaWave::Init(PUNKNOWN UnknownAdapter, PRESOURCELIST ResourceList, PPORTWAVERT Port)
{
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(Port);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CSaWave::NewStream(PMINIPORTWAVERTSTREAM *Stream, PPORTWAVERTSTREAM PortStream, ULONG Pin,
                                           BOOLEAN Capture, PKSDATAFORMAT DataFormat)
{
    CSaStream *New;
    *Stream = NULL;
    if (Capture || Pin != 0 || !SaFormatOk(DataFormat)) {
        return STATUS_INVALID_PARAMETER;
    }
    New = SA_NEW CSaStream(PortStream);
    if (New == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    *Stream = New;
    SaCall(1);
    SaTrace(L"Streams", (ULONG)InterlockedIncrement(&m_Streams));
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CSaWave::GetDeviceDescription(PDEVICE_DESCRIPTION DeviceDescription)
{
    RtlZeroMemory(DeviceDescription, sizeof(*DeviceDescription));
    DeviceDescription->Version = DEVICE_DESCRIPTION_VERSION;
    DeviceDescription->Master = TRUE;
    DeviceDescription->ScatterGather = FALSE;
    DeviceDescription->Dma32BitAddresses = TRUE;
    DeviceDescription->InterfaceType = PNPBus;
    DeviceDescription->MaximumLength = 0x10000;
    return STATUS_SUCCESS;
}

/* ----------------------------------------------------- topology miniport */

class CSaTopo final : public IMiniportTopology {
public:
    LONG m_Ref;
    CSaTopo() : m_Ref(1) {}

    STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID Iid, PVOID *Object)
    {
        if (IsEqualGUIDAligned(Iid, IID_IUnknown) || IsEqualGUIDAligned(Iid, IID_IMiniport) ||
            IsEqualGUIDAligned(Iid, IID_IMiniportTopology)) {
            *Object = static_cast<IMiniportTopology *>(this);
            AddRef();
            return STATUS_SUCCESS;
        }
        *Object = NULL;
        return STATUS_INVALID_PARAMETER;
    }
    STDMETHODIMP_(ULONG) AddRef() { return (ULONG)InterlockedIncrement(&m_Ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG Ref = InterlockedDecrement(&m_Ref);
        if (Ref == 0) delete this;
        return (ULONG)Ref;
    }

    IMP_IMiniportTopology;
};

STDMETHODIMP_(NTSTATUS) CSaTopo::GetDescription(PPCFILTER_DESCRIPTOR *Description)
{
    *Description = &g_TopoFilter;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CSaTopo::DataRangeIntersection(ULONG PinId, PKSDATARANGE DataRange, PKSDATARANGE MatchingDataRange,
                                                       ULONG OutputBufferLength, PVOID ResultantFormat,
                                                       PULONG ResultantFormatLength)
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(MatchingDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    UNREFERENCED_PARAMETER(ResultantFormatLength);
    return STATUS_NOT_IMPLEMENTED;
}

STDMETHODIMP_(NTSTATUS) CSaTopo::Init(PUNKNOWN UnknownAdapter, PRESOURCELIST ResourceList, PPORTTOPOLOGY Port)
{
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(Port);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- adapter */

static NTSTATUS SaInstallSubdevice(PDEVICE_OBJECT DeviceObject, PIRP Irp, PCWSTR Name, REFCLSID PortClsid,
                                   PUNKNOWN Miniport, PRESOURCELIST ResourceList, PPORT *PortOut)
{
    PPORT Port = NULL;
    NTSTATUS Status = PcNewPort(&Port, PortClsid);
    if (NT_SUCCESS(Status)) {
        Status = Port->Init(DeviceObject, Irp, Miniport, NULL, ResourceList);
    }
    if (NT_SUCCESS(Status)) {
        Status = PcRegisterSubdevice(DeviceObject, (PWSTR)Name, Port);
    }
    if (NT_SUCCESS(Status)) {
        *PortOut = Port;
    } else if (Port != NULL) {
        Port->Release();
    }
    return Status;
}

/* ------------------------------------------------------------- headphones */

static NTSTATUS SaCodecWrite(UCHAR Reg, USHORT Value)
{
    IO_STATUS_BLOCK Iosb;
    UCHAR Buf[3] = { Reg, (UCHAR)(Value >> 8), (UCHAR)Value };
    return ZwWriteFile(g_Hw.Spb, NULL, NULL, NULL, &Iosb, Buf, sizeof(Buf), NULL, NULL);
}

static NTSTATUS SaCodecRead(UCHAR Reg, USHORT *Value)
{
    IO_STATUS_BLOCK Iosb;
    UCHAR Data[2] = { 0, 0 };
    SPB_TRANSFER_LIST_AND_ENTRIES(2) Sequence;
    NTSTATUS Status;

    SPB_TRANSFER_LIST_INIT(&Sequence.List, 2);
    Sequence.List.Transfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice, 0, &Reg, 1);
    Sequence.List.Transfers[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice, 0, Data, 2);
    Status = ZwDeviceIoControlFile(g_Hw.Spb, NULL, NULL, NULL, &Iosb, IOCTL_SPB_EXECUTE_SEQUENCE,
                                   &Sequence, sizeof(Sequence), NULL, 0);
    *Value = (USHORT)((Data[0] << 8) | Data[1]);
    return Status;
}

static VOID SaCodecSnapshot(VOID)
{
    static const struct { UCHAR Reg; PCWSTR Name; } Registers[] = {
        { 0x19, L"CodecDacVolume" }, { 0x29, L"CodecAdcDacMixer" },
        { 0x2A, L"CodecStereoDacMixer" }, { 0x73, L"CodecAddaClock" },
        { 0x83, L"CodecAsrc1" }, { 0x84, L"CodecAsrc2" },
        { 0x01, L"CodecSpkVolume" }, { 0x46, L"CodecSpkLeftMixer" },
        { 0x47, L"CodecSpkRightMixer" }, { 0x48, L"CodecSpkLeftOut" },
        { 0x49, L"CodecSpkRightOut" }, { 0x61, L"CodecPowerDigital" },
        { 0x63, L"CodecPowerAnalog1" }, { 0x64, L"CodecPowerAnalog2" },
        { 0x65, L"CodecPowerMixer" }, { 0x66, L"CodecPowerVolume" },
        { 0x70, L"CodecI2s1" }, { 0x80, L"CodecGlobalClock" },
        { 0x81, L"CodecPll1" }, { 0x82, L"CodecPll2" }
    };
    static const struct { UCHAR Index; PCWSTR Name; } Private[] = {
        { 0x1D, L"CodecPrivate1D" }, { 0x14, L"CodecBiasCur3" },
        { 0x20, L"CodecClassDTuning" }
    };
    USHORT Value;
    NTSTATUS Status;
    ULONG Index;
    for (Index = 0; Index < ARRAYSIZE(Registers); ++Index) {
        Status = SaCodecRead(Registers[Index].Reg, &Value);
        if (!NT_SUCCESS(Status)) { SaTrace(L"CodecReadStatus", (ULONG)Status); return; }
        SaTrace(Registers[Index].Name, Value);
    }
    for (Index = 0; Index < ARRAYSIZE(Private); ++Index) {
        Status = SaCodecWrite(0x6A, Private[Index].Index);
        if (NT_SUCCESS(Status)) Status = SaCodecRead(0x6C, &Value);
        if (!NT_SUCCESS(Status)) { SaTrace(L"CodecReadStatus", (ULONG)Status); return; }
        SaTrace(Private[Index].Name, Value);
    }
    SaTrace(L"CodecReadStatus", STATUS_SUCCESS);
}

static NTSTATUS SaSpbOpen(PRESOURCELIST ResourceList)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Conn = ResourceList->FindTranslatedEntry(CmResourceTypeConnection, 0);
    WCHAR PathBuf[64];
    UNICODE_STRING Path;
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;

    if (Conn == NULL) {
        return STATUS_NOT_FOUND;
    }
    Status = RtlStringCchPrintfW(PathBuf, ARRAYSIZE(PathBuf), L"\\Device\\RESOURCE_HUB\\%016I64x",
                                 ((ULONG64)Conn->u.Connection.IdHighPart << 32) | Conn->u.Connection.IdLowPart);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    RtlInitUnicodeString(&Path, PathBuf);
    InitializeObjectAttributes(&Oa, &Path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return ZwCreateFile(&g_Hw.Spb, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &Oa, &Iosb, NULL, 0, 0, FILE_OPEN,
                        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
}

/* Unmute the output the jack picks, the other one first muted. */
static NTSTATUS SaJackApply(BOOLEAN Headphones)
{
    NTSTATUS Status;
    if (Headphones) {
        Status = SaCodecWrite(RT5640_SPK_VOL, SA_SPK_VOL | SA_MUTE);
        if (NT_SUCCESS(Status)) Status = SaCodecWrite(RT5640_HP_VOL, SA_HP_VOL);
    } else {
        Status = SaCodecWrite(RT5640_HP_VOL, SA_HP_VOL | SA_MUTE);
        if (NT_SUCCESS(Status)) Status = SaCodecWrite(RT5640_SPK_VOL, SA_SPK_VOL);
    }
    return Status;
}

static VOID SaJackThread(PVOID Context)
{
    LARGE_INTEGER Period;
    ULONG Applied = 2, Last = 2, Switches = 0;

    UNREFERENCED_PARAMETER(Context);
    Period.QuadPart = -200LL * 10000;
    while (KeWaitForSingleObject(&g_Hw.JackStop, Executive, KernelMode, FALSE, &Period) == STATUS_TIMEOUT) {
        ULONG Now = (READ_REGISTER_ULONG((volatile ULONG *)(g_Hw.Gpio + GPIO_PV_IN)) & GPIO_PV6) == 0 ? 1 : 0;
        if (Now == Last && Now != Applied) {         /* two equal readings, 200 ms apart */
            NTSTATUS Status = SaJackApply(Now != 0);
            SaTrace(L"JackSpb", (ULONG)Status);
            if (NT_SUCCESS(Status)) {
                Applied = Now;
                SaTrace(L"Jack", Now);
                SaTrace(L"JackSwitches", ++Switches);
                SaCodecSnapshot();
            }
        }
        Last = Now;
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID SaJackStop(VOID)
{
    if (g_Hw.JackThread != NULL) {
        KeSetEvent(&g_Hw.JackStop, IO_NO_INCREMENT, FALSE);
        (VOID)KeWaitForSingleObject(g_Hw.JackThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g_Hw.JackThread);
        g_Hw.JackThread = NULL;
    }
    if (g_Hw.Spb != NULL) {
        ZwClose(g_Hw.Spb);
        g_Hw.Spb = NULL;
    }
    if (g_Hw.Gpio != NULL) {
        MmUnmapIoSpace((PVOID)g_Hw.Gpio, GPIO_LENGTH);
        g_Hw.Gpio = NULL;
    }
}

/* Only with UEFI R403 or later: the headphone path set up, PV6 a GPIO input. */
static VOID SaJackStart(PRESOURCELIST ResourceList)
{
    PHYSICAL_ADDRESS Pa;
    USHORT Hpo = 0;
    ULONG Cnf, Oe;
    HANDLE Thread;
    NTSTATUS Status;

    Status = SaSpbOpen(ResourceList);
    SaTrace(L"JackSpb", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        g_Hw.Spb = NULL;
        return;
    }
    Status = SaCodecRead(RT5640_HPO_MIXER, &Hpo);
    SaTrace(L"HpoMixer", NT_SUCCESS(Status) ? Hpo : 0xFFFF0000u | (ULONG)(Status & 0xFFFF));
    Pa.QuadPart = (LONGLONG)GPIO_PHYS;
    g_Hw.Gpio = (volatile UCHAR *)MmMapIoSpaceEx(Pa, GPIO_LENGTH, PAGE_READWRITE | PAGE_NOCACHE);
    if (!NT_SUCCESS(Status) || (Hpo & RT5640_M_HPVOL_HM) != 0 || g_Hw.Gpio == NULL) {
        SaJackStop();
        return;
    }
    Cnf = READ_REGISTER_ULONG((volatile ULONG *)(g_Hw.Gpio + GPIO_PV_CNF));
    Oe = READ_REGISTER_ULONG((volatile ULONG *)(g_Hw.Gpio + GPIO_PV_OE));
    SaTrace(L"JackGpio", (Cnf & 0xFF) | ((Oe & 0xFF) << 8) |
                         ((READ_REGISTER_ULONG((volatile ULONG *)(g_Hw.Gpio + GPIO_PV_IN)) & 0xFF) << 16));
    if ((Cnf & GPIO_PV6) == 0 || (Oe & GPIO_PV6) != 0) {
        SaJackStop();
        return;
    }
    KeClearEvent(&g_Hw.JackStop);
    Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, SaJackThread, NULL);
    if (NT_SUCCESS(Status)) {
        Status = ObReferenceObjectByHandle(Thread, SYNCHRONIZE, *PsThreadType, KernelMode, (PVOID *)&g_Hw.JackThread, NULL);
        ZwClose(Thread);
    }
    if (!NT_SUCCESS(Status)) {
        g_Hw.JackThread = NULL;
        SaJackStop();
    }
}

static NTSTATUS SaStartDevice(PDEVICE_OBJECT DeviceObject, PIRP Irp, PRESOURCELIST ResourceList)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Mem0, Mem1;
    ULONG Ape;
    PPORT WavePort = NULL, TopoPort = NULL;
    CSaWave *Wave;
    CSaTopo *Topo;
    NTSTATUS Status;

    (VOID)RtlCreateRegistryKey(RTL_REGISTRY_SERVICES, g_ParamsPath);
    SaTrace(L"Stage", 1);
    SaTrace(L"DriverVersion", 9);
    Mem0 = ResourceList->FindTranslatedEntry(CmResourceTypeMemory, 0);
    Mem1 = ResourceList->FindTranslatedEntry(CmResourceTypeMemory, 1);
    if (Mem0 == NULL || Mem1 == NULL || Mem0->u.Memory.Length < AHUB_LENGTH || Mem1->u.Memory.Length < ADMA_LENGTH) {
        SaTrace(L"Stage", 0x10);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Ape = SaCheckApe();
    SaTrace(L"ApeCheck", Ape);
    if (Ape != 0xF) {
        SaTrace(L"Stage", 0x11);                   /* UEFI older than R402: do not touch the AHUB */
        return STATUS_DEVICE_NOT_READY;
    }
    if (g_Hw.Ahub == NULL) {
        g_Hw.Ahub = (volatile UCHAR *)MmMapIoSpaceEx(Mem0->u.Memory.Start, AHUB_LENGTH, PAGE_READWRITE | PAGE_NOCACHE);
    }
    if (g_Hw.Adma == NULL) {
        g_Hw.Adma = (volatile UCHAR *)MmMapIoSpaceEx(Mem1->u.Memory.Start, ADMA_LENGTH, PAGE_READWRITE | PAGE_NOCACHE);
    }
    if (g_Hw.Ahub == NULL || g_Hw.Adma == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    SaAdmaStop();
    SaTrace(L"I2sEnable", RdAhub(I2S1_ENABLE));
    SaTrace(L"I2sCtrl", RdAhub(I2S1_CTRL));
    SaTrace(L"Xbar", RdAhub(XBAR_I2S1_RX_MUX));
    SaTrace(L"AdmaifCtrl", RdAhub(ADMAIF_TX1_ACIF_CTRL));
    if (RdAhub(I2S1_ENABLE) == 0 || RdAhub(XBAR_I2S1_RX_MUX) != 1) {
        SaTrace(L"Stage", 0x12);                   /* clocks on but the path is not set up */
        return STATUS_DEVICE_NOT_READY;
    }
    SaTrace(L"Stage", 2);
    // These always-on registers expose the electrical/clock state inherited
    // from firmware; AHUB activity alone cannot prove signals reach the codec.
    SaTrace(L"PadPower", ReadPhys(PMC_PHYS + 0x44));
    SaTrace(L"PadVoltageEnable", ReadPhys(PMC_PHYS + 0x48));
    SaTrace(L"PadVoltage", ReadPhys(PMC_PHYS + 0xE4));
    SaTrace(L"PadDpd", ReadPhys(PMC_PHYS + 0x1BC));
    SaTrace(L"PadDpd2", ReadPhys(PMC_PHYS + 0x1C4));
    SaTrace(L"PinDap1Fs", ReadPhys(0x70003124ULL));
    SaTrace(L"PinDap1Dout", ReadPhys(0x7000312CULL));
    SaTrace(L"PinDap1Sclk", ReadPhys(0x70003130ULL));
    SaTrace(L"Dap1GpioCnf", ReadPhys(GPIO_PHYS + 0x004));
    SaTrace(L"I2sClockSource", ReadPhys(CAR_PHYS + 0x1D8));
    SaTrace(L"ClockSpare", ReadPhys(CAR_PHYS + 0x55C));
    SaTrace(L"I2sTiming", RdAhub(0x10A4));
    SaSnapshot(L"Start");
    if (SaReadParam(L"TestTone") != 0) {
        SaTestTone();
    }

    Wave = SA_NEW CSaWave();
    Topo = SA_NEW CSaTopo();
    if (Wave == NULL || Topo == NULL) {
        if (Wave != NULL) Wave->Release();
        if (Topo != NULL) Topo->Release();
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Status = SaInstallSubdevice(DeviceObject, Irp, L"Wave", CLSID_PortWaveRT, static_cast<IMiniportWaveRT *>(Wave),
                                ResourceList, &WavePort);
    SaTrace(L"WaveStatus", (ULONG)Status);
    if (NT_SUCCESS(Status)) {
        Status = SaInstallSubdevice(DeviceObject, Irp, L"Topology", CLSID_PortTopology,
                                    static_cast<IMiniportTopology *>(Topo), ResourceList, &TopoPort);
        SaTrace(L"TopoStatus", (ULONG)Status);
    }
    if (NT_SUCCESS(Status)) {
        Status = PcRegisterPhysicalConnection(DeviceObject, WavePort, 1, TopoPort, 0);
        SaTrace(L"ConnectStatus", (ULONG)Status);
    }
    Wave->Release();                               /* the ports keep their own references */
    Topo->Release();
    if (WavePort != NULL) WavePort->Release();
    if (TopoPort != NULL) TopoPort->Release();
    if (NT_SUCCESS(Status)) {
        g_Hw.Ready = TRUE;
        SaTrace(L"Stage", 3);
        SaJackStart(ResourceList);
    }
    return Status;
}

static PDRIVER_DISPATCH g_PcPnp;

static NTSTATUS SaDispatchPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UCHAR Minor = IoGetCurrentIrpStackLocation(Irp)->MinorFunction;
    NTSTATUS Status;
    if (Minor == IRP_MN_STOP_DEVICE || Minor == IRP_MN_REMOVE_DEVICE || Minor == IRP_MN_SURPRISE_REMOVAL) {
        KIRQL Irql;
        SaJackStop();
        KeAcquireSpinLock(&g_Hw.Lock, &Irql);
        SaAdmaStop();
        KeReleaseSpinLock(&g_Hw.Lock, Irql);
    }
    Status = g_PcPnp(DeviceObject, Irp);
    if (Minor == IRP_MN_REMOVE_DEVICE) {
        if (g_Hw.Ahub != NULL) MmUnmapIoSpace((PVOID)g_Hw.Ahub, AHUB_LENGTH);
        if (g_Hw.Adma != NULL) MmUnmapIoSpace((PVOID)g_Hw.Adma, ADMA_LENGTH);
        g_Hw.Ahub = NULL;
        g_Hw.Adma = NULL;
        g_Hw.Ready = FALSE;
    }
    return Status;
}

static NTSTATUS SaAddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject)
{
    return PcAddAdapterDevice(DriverObject, PhysicalDeviceObject, (PCPFNSTARTDEVICE)SaStartDevice, 2, 0);
}

extern "C" DRIVER_INITIALIZE DriverEntry;

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NTSTATUS Status;
    KeInitializeSpinLock(&g_Hw.Lock);
    KeInitializeEvent(&g_Hw.JackStop, NotificationEvent, FALSE);
    SaInitDescriptors();
    Status = PcInitializeAdapterDriver(DriverObject, RegistryPath, (PDRIVER_ADD_DEVICE)SaAddDevice);
    if (NT_SUCCESS(Status)) {
        g_PcPnp = DriverObject->MajorFunction[IRP_MJ_PNP];
        DriverObject->MajorFunction[IRP_MJ_PNP] = SaDispatchPnp;
    }
    return Status;
}
