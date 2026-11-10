/* SPDX-License-Identifier: MIT */
/*
 * Configuration from HKLM\SYSTEM\CurrentControlSet\Services\gm20b\Parameters (all REG_DWORD, all optional):
 *   MaxStage      highest stage to run, 0..8            (default 0 = only validate the HAL, no hardware access)
 *   Flags         GM20B_CF_* bits                      (default 0)
 *   GpcKhz        S3 target with GM20B_CF_USE_PLL      (default 0 = stage default)
 *   RailUv        S1 target with GM20B_CF_ALLOW_RAIL   (default 0 = leave the rail alone)
 *   PollScalePct  scales every bounded wait            (default 0 = 100)
 *   AllowRawMmio  1 enables the RAW_READ/RAW_WRITE IOCTLs (default 0)
 * Every value actually used is journaled (J_WIN_REG_*), so the journal shows what the run was configured with.
 */
#include "win_priv.h"

#define USTR(var, text) static WCHAR var##_w[] = text; static UNICODE_STRING var = NTK_USTR_INIT(var##_w)

USTR(u_params,   L"Parameters");
USTR(u_maxstage, L"MaxStage");
USTR(u_flags,    L"Flags");
USTR(u_gpckhz,   L"GpcKhz");
USTR(u_railuv,   L"RailUv");
USTR(u_pollpct,  L"PollScalePct");
USTR(u_rawmmio,  L"AllowRawMmio");

static int query_dword(HANDLE key, PUNICODE_STRING name, u32 *out)
{
	union { KEY_VALUE_PARTIAL_INFORMATION i; u8 raw[32]; } buf;
	ULONG got = 0;
	NTSTATUS st;

	gm20b_zero(&buf, sizeof(buf));
	st = ZwQueryValueKey(key, name, KeyValuePartialInformation, &buf, (ULONG)sizeof(buf), &got);
	if (!NT_SUCCESS(st) || buf.i.Type != REG_DWORD || buf.i.DataLength != 4u)
		return -1;
	gm20b_copy(out, buf.i.Data, 4);
	return 0;
}

void win_cfg_load(struct win_dev *d, PUNICODE_STRING reg_path, struct gm20b_cfg *cfg)
{
	struct {
		PUNICODE_STRING name;
		u32 code;
		u32 *dst;
	} tab[6];
	OBJECT_ATTRIBUTES oa;
	HANDLE pk = NULL;
	u32 i, v, found;

	gm20b_zero(cfg, sizeof(*cfg));
	d->allow_raw = 0;

	InitializeObjectAttributes(&oa, reg_path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
	d->key = NULL;
	d->key_status = ZwOpenKey(&d->key, KEY_SET_VALUE | KEY_QUERY_VALUE, &oa);
	if (!NT_SUCCESS(d->key_status))
		d->key = NULL;
	if (d->key) {
		InitializeObjectAttributes(&oa, &u_params, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, d->key, NULL);
		d->params_status = ZwOpenKey(&pk, KEY_QUERY_VALUE, &oa);
		if (!NT_SUCCESS(d->params_status))
			pk = NULL;
	} else {
		d->params_status = d->key_status;
	}
	win_rec(d, J_WIN_KEY, (u32)d->key_status, (u32)d->params_status);

	tab[0].name = &u_maxstage; tab[0].code = J_WIN_REG_MAXSTAGE; tab[0].dst = &cfg->max_stage;
	tab[1].name = &u_flags;    tab[1].code = J_WIN_REG_FLAGS;    tab[1].dst = &cfg->flags;
	tab[2].name = &u_gpckhz;   tab[2].code = J_WIN_REG_GPCKHZ;   tab[2].dst = &cfg->gpc_khz;
	tab[3].name = &u_railuv;   tab[3].code = J_WIN_REG_RAILUV;   tab[3].dst = &cfg->rail_uv;
	tab[4].name = &u_pollpct;  tab[4].code = J_WIN_REG_POLLPCT;  tab[4].dst = &cfg->poll_scale_pct;
	tab[5].name = &u_rawmmio;  tab[5].code = J_WIN_REG_RAWMMIO;  tab[5].dst = &d->allow_raw;
	for (i = 0; i < 6u; i++) {
		found = 0;
		if (pk && query_dword(pk, tab[i].name, &v) == 0) {
			*tab[i].dst = v;
			found = 1;
		}
		win_rec(d, tab[i].code, *tab[i].dst, found);
	}
	if (pk)
		(void)ZwClose(pk);
}
