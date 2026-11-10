/* SPDX-License-Identifier: MIT */
/*
 * Format of \SystemRoot\gm20b-journal.bin (little endian), shared by the driver and the decoders.
 *
 *   struct gm20b_jf_header   32 bytes, written once when the file is created
 *   struct gm20b_jentry      24 bytes each (core/gm20b_hal.h), appended forever, over many driver loads
 *
 * Core records carry seq = 0,1,2,... counting from the start of each driver load.  Records written by the
 * Windows shell itself carry seq >= GM20B_JF_SEQ_WIN.  A new driver load always starts with J_WIN_LOAD.
 * The file is opened write-through and flushed after every flush request of the core; additionally the
 * newest record is mirrored into the registry value  Services\gm20b\LastEntry  (REG_BINARY, 24 bytes).
 */
#ifndef GM20B_JOURNAL_FILE_H
#define GM20B_JOURNAL_FILE_H

typedef unsigned int gm_u32;

#define GM20B_JF_MAGIC       0x4a303247u            /* bytes 'G' '2' '0' 'J' */
#define GM20B_JF_VERSION     1u
#define GM20B_JF_ENTRY_SIZE  24u
#define GM20B_JF_HDR_SIZE    32u
#define GM20B_JF_SEQ_WIN     0xF0000000u

#define GM20B_JF_PATH_NT     L"\\SystemRoot\\gm20b-journal.bin"
#define GM20B_JF_PATH_USER   "C:\\Windows\\gm20b-journal.bin"

struct gm20b_jf_header {
	gm_u32 magic;        /* GM20B_JF_MAGIC */
	gm_u32 version;      /* GM20B_JF_VERSION */
	gm_u32 entry_size;   /* GM20B_JF_ENTRY_SIZE */
	gm_u32 header_size;  /* GM20B_JF_HDR_SIZE */
	gm_u32 reserved[4];
};

#endif
