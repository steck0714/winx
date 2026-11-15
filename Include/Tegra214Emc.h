// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef LITE_EMC_HANDOFF_H
#define LITE_EMC_HANDOFF_H
/* Independent of LITE_PERFORMANCE and DFLL handoff. Channel 7 is reserved for
 * the Windows EMC owner; atomic TF-A/UEFI channels 0..3 remain untouched.
 * Other Windows firmware clients (including a future SC7 owner) must not use 7.
 */
#define LITE_EMC_HANDOFF_ADDRESS 0xA9FB0200u
#define LITE_EMC_HANDOFF_MAGIC 0x48434D45u
#define LITE_EMC_HANDOFF_VERSION 1u
#define LITE_EMC_CHANNEL 7u
#define LITE_EMC_MAIL_BASE 0xFFE00000u
#define LITE_EMC_MAIL_SIZE 0x00100000u
#define LITE_EMC_IRAM_BASE 0x40000000u
#define LITE_EMC_IRAM_SIZE 0x00040000u
#define LITE_EMC_REGION_DRAM 1u
#define LITE_EMC_REGION_IRAM 2u
typedef struct {
    unsigned int Magic, Version, Size, Hidrev, Sku;
    unsigned int Channel, MailAddress, GpuSpeedo, SocSpeedo;
} LITE_EMC_HANDOFF;
/* BPMP owns the selected address. Both regions are supported by the existing
 * atomic UEFI client; the observed channel 0 is at 0x40028fc0 on this device.
 * Accept only a complete aligned mailbox, not the rest of either aperture. */
static inline unsigned int LiteEmcMailRegion(unsigned int address)
{
    if (address & 3u) return 0;
    if (address >= LITE_EMC_MAIL_BASE &&
        address - LITE_EMC_MAIL_BASE <= LITE_EMC_MAIL_SIZE - 128u)
        return LITE_EMC_REGION_DRAM;
    if (address >= LITE_EMC_IRAM_BASE &&
        address - LITE_EMC_IRAM_BASE <= LITE_EMC_IRAM_SIZE - 128u)
        return LITE_EMC_REGION_IRAM;
    return 0;
}
static inline int LiteEmcHandoffValid(const LITE_EMC_HANDOFF *h)
{
    return h->Magic == LITE_EMC_HANDOFF_MAGIC && h->Version == LITE_EMC_HANDOFF_VERSION &&
        h->Size == sizeof(*h) && (h->Hidrev & 0xFFF0u) == 0x2120u && h->Sku == 0x83u &&
        h->Channel == LITE_EMC_CHANNEL && LiteEmcMailRegion(h->MailAddress) != 0 &&
        h->GpuSpeedo > 0 && h->GpuSpeedo <= 5000u && h->SocSpeedo > 0 && h->SocSpeedo <= 5000u;
}
#endif
