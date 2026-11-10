/* SPDX-License-Identifier: MIT */
/*
 * gm20b.sys -- legacy (non-PnP) WDM driver that runs the staged GM20B bring-up (core/) and exposes \\.\Gm20b.
 *
 * Design rules (see docs/WINDOWS.md):
 *  - Demand start only; DriverEntry returns STATUS_SUCCESS even when the bring-up failed, so the driver stays loaded
 *    and the journal / IOCTLs can be inspected.  The bring-up is gated by the registry value MaxStage (default 0).
 *  - Fixed physical addresses mapped with MmMapIoSpaceEx; no ACPI / bl33 change needed.  Polling only: no ISR, no DPC.
 *  - Everything runs at PASSIVE_LEVEL (DriverEntry, IOCTLs, DriverUnload).  No pool allocations: one global in .bss.
 *  - Raw MMIO IOCTLs exist only with AllowRawMmio=1; writes are refused to MC, to CAR except the SET/CLR aliases and
 *    to PMC except GPU_RG_CNTRL; BAR0/BAR1 are refused while CAR/PMC say the GPU is not powered.
 */
#include "win_priv.h"
#include "tegra_car_pmc.h"

struct win_dev g_win;

static WCHAR dev_name_w[]  = L"\\Device\\Gm20b";
static WCHAR link_name_w[] = L"\\DosDevices\\Gm20b";
static UNICODE_STRING dev_name  = NTK_USTR_INIT(dev_name_w);
static UNICODE_STRING link_name = NTK_USTR_INIT(link_name_w);

static u32 get_u32(const void *p)
{
	u32 v;

	gm20b_copy(&v, p, sizeof(v));
	return v;
}

static void put_u32(void *p, u32 v)
{
	gm20b_copy(p, &v, sizeof(v));
}

/* same test as stage 2's guard: clock gate on, not in reset, not clamped */
static int gpu_unpowered(struct win_dev *d)
{
	u32 clk = gm20b_rd(&d->g, GM20B_SP_CAR, CAR_CLK_OUT_ENB_X);
	u32 rst = gm20b_rd(&d->g, GM20B_SP_CAR, CAR_RST_DEVICES_X);
	u32 rg = gm20b_rd(&d->g, GM20B_SP_PMC, PMC_GPU_RG_CNTRL);

	return !(clk & GM20B_BIT(CAR_X_GPU_BIT)) || (rst & GM20B_BIT(CAR_X_GPU_BIT)) ||
	       (rg & GM20B_BIT(PMC_GPU_RG_CNTRL_GATED_BIT));
}

/* 0 = allowed, else a WIN_RAW_* reason */
static u32 raw_policy(struct win_dev *d, u32 sp, u32 off, int is_write)
{
	if (sp >= (u32)GM20B_SP_COUNT || (off & 3u) || off > gm20b_space_size((enum gm20b_space)sp) - 4u)
		return WIN_RAW_BADREQ;
	if (sp == (u32)GM20B_SP_MC && is_write)
		return WIN_RAW_POLICY;
	if (sp == (u32)GM20B_SP_CAR) {
		u32 is_alias = off == CAR_CLK_OUT_ENB_SET_X || off == CAR_CLK_OUT_ENB_CLR_X ||
			       off == CAR_RST_DEVICES_SET_X || off == CAR_RST_DEVICES_CLR_X;

		if (is_write != (int)is_alias)            /* aliases are write-only; everything else is read-only */
			return WIN_RAW_POLICY;
	}
	if (sp == (u32)GM20B_SP_PMC && is_write && off != PMC_GPU_RG_CNTRL)
		return WIN_RAW_POLICY;
	if ((sp == (u32)GM20B_SP_BAR0 || sp == (u32)GM20B_SP_BAR1) && !(d->g.cfg.flags & GM20B_CF_FORCE_BAR0)) {
		/* the guard itself reads CAR and PMC: announce that durably first (intent before access, as everywhere) */
		win_step(d, J_WIN_RAW_GUARD, GM20B_JADDR(sp & 0xfu, off), 0);
		if (gpu_unpowered(d))
			return WIN_RAW_UNPOWERED;
	}
	return 0;
}

static NTSTATUS raw_rw(struct win_dev *d, ULONG code, void *buf, ULONG in_len, ULONG out_len, ULONG_PTR *info)
{
	int wr = code == GM20B_IOCTL_RAW_WRITE;
	u32 sp, off, val = 0, why, ja;

	if (!d->allow_raw) {
		win_rec(d, J_WIN_RAW_DENIED, 0, WIN_RAW_OFF);
		return STATUS_ACCESS_DENIED;
	}
	if (wr ? in_len < (ULONG)sizeof(struct gm20b_ioctl_rawwr)
	       : (in_len < (ULONG)sizeof(struct gm20b_ioctl_rawrd) || out_len < 4u))
		return STATUS_BUFFER_TOO_SMALL;
	sp = get_u32(buf);
	off = get_u32((u8 *)buf + 4);
	if (wr)
		val = get_u32((u8 *)buf + 8);
	ja = GM20B_JADDR(sp & 0xfu, off);
	why = d->map_ok ? raw_policy(d, sp, off, wr) : WIN_RAW_BADREQ;
	if (why) {
		win_rec(d, J_WIN_RAW_DENIED, ja, why);
		return STATUS_ACCESS_DENIED;
	}
	if (wr) {
		win_step(d, J_WIN_RAWWR, ja, val);        /* durable BEFORE the access */
		gm20b_wr(&d->g, (enum gm20b_space)sp, off, val);
		win_rec(d, J_WIN_RAWWR_DONE, ja, 0);
	} else {
		win_step(d, J_WIN_RAWRD, ja, 0);
		val = gm20b_rd(&d->g, (enum gm20b_space)sp, off);
		win_rec(d, J_WIN_RAWRD_DONE, ja, val);
		put_u32(buf, val);
		*info = 4;
	}
	return STATUS_SUCCESS;
}

static void fill_status(struct win_dev *d, struct gm20b_ioctl_status *s)
{
	gm20b_zero(s, sizeof(*s));
	s->abi = GM20B_IOCTL_ABI;
	s->size = (gm_u32)sizeof(*s);
	s->max_stage = d->g.cfg.max_stage;
	s->stage_done = d->g.stage_done;
	s->init_status = d->init_status;
	s->last_err = d->g.last_err;
	s->fail_stage = d->g.fail_stage;
	s->flags = d->g.cfg.flags;
	s->allow_raw = d->allow_raw;
	s->j_head = d->g.j.head;
	s->j_flushed = d->g.j.flushed;
	s->j_lost = d->g.j.lost;
	s->boot0 = d->g.info.boot0;
	s->chipset = d->g.info.chipset;
	s->chiprev = d->g.info.chiprev;
	s->gpc_count = d->g.info.gpc_count;
	s->tpc_total = d->g.info.tpc_total;
	s->ltc_count = d->g.info.ltc_count;
	s->smmu_enabled = d->g.info.smmu_enabled;
	s->jf_open_status = (gm_u32)d->jf_open_status;
	s->jf_errors = d->jf_err_count;
	s->jf_last_error = (gm_u32)d->jf_last_err;
	s->bc_errors = d->bc_err_count;
	s->bad_access = d->bad_access;
	s->jf_bytes = d->jf_bytes;
	s->jf_dropped = d->jf_dropped;
	s->driver_version = GM20B_WIN_VERSION;
}

static NTSTATUS do_ioctl(struct win_dev *d, ULONG code, void *buf, ULONG in_len, ULONG out_len, ULONG_PTR *info)
{
	switch (code) {
	case GM20B_IOCTL_GET_STATUS: {
		struct gm20b_ioctl_status s;

		if (out_len < (ULONG)sizeof(s))
			return STATUS_BUFFER_TOO_SMALL;
		fill_status(d, &s);
		gm20b_copy(buf, &s, sizeof(s));
		*info = sizeof(s);
		return STATUS_SUCCESS;
	}
	case GM20B_IOCTL_READ_JOURNAL: {
		/* the core's in-memory ring (the last GM20B_J_RING records); the file has the complete history */
		struct gm20b_jentry *out = (struct gm20b_jentry *)buf;
		u32 from, head, oldest, n, max, i;

		if (in_len < 4u || out_len < (ULONG)sizeof(struct gm20b_jentry))
			return STATUS_BUFFER_TOO_SMALL;
		from = get_u32(buf);
		head = d->g.j.head;
		oldest = head > GM20B_J_RING ? head - GM20B_J_RING : 0u;
		if (from < oldest)
			from = oldest;
		max = out_len / (ULONG)sizeof(struct gm20b_jentry);
		n = head > from ? head - from : 0u;
		if (n > max)
			n = max;
		for (i = 0; i < n; i++)
			out[i] = d->g.j.ring[(from + i) % GM20B_J_RING];
		*info = (ULONG_PTR)n * sizeof(struct gm20b_jentry);
		return STATUS_SUCCESS;
	}
	case GM20B_IOCTL_ADVANCE: {
		u32 n;
		int rc;

		if (in_len < 4u)
			return STATUS_BUFFER_TOO_SMALL;
		n = get_u32(buf);
		win_step(d, J_WIN_IOCTL, code, n);
		rc = d->map_ok ? gm20b_advance(&d->g, n) : GM20B_E_STATE;
		gm20b_jflush(&d->g);
		d->init_status = rc;
		win_rec(d, J_WIN_IOCTL_DONE, code, (u32)rc);
		if (out_len >= 4u) {
			put_u32(buf, (u32)rc);
			*info = 4;
		}
		return STATUS_SUCCESS;                    /* the IOCTL worked; the bring-up status is in the output */
	}
	case GM20B_IOCTL_FINI:
		win_step(d, J_WIN_IOCTL, code, d->g.stage_done);
		gm20b_fini(&d->g);
		d->g.cfg.max_stage = 0;                   /* a later ADVANCE starts from scratch */
		d->init_status = 0;
		win_rec(d, J_WIN_IOCTL_DONE, code, 0);
		return STATUS_SUCCESS;
	case GM20B_IOCTL_RAW_READ:
	case GM20B_IOCTL_RAW_WRITE:
		return raw_rw(d, code, buf, in_len, out_len, info);
	default:
		return STATUS_INVALID_DEVICE_REQUEST;
	}
}

static NTSTATUS dispatch(PDEVICE_OBJECT dev, PIRP irp)
{
	struct win_dev *d = &g_win;
	PIO_STACK_LOCATION isl = IoGetCurrentIrpStackLocation(irp);
	NTSTATUS st = STATUS_SUCCESS;
	ULONG_PTR info = 0;

	(void)dev;
	switch (isl->MajorFunction) {
	case IRP_MJ_CREATE:
	case IRP_MJ_CLOSE:
		break;
	case IRP_MJ_DEVICE_CONTROL:
		if (__sync_lock_test_and_set(&d->busy, 1)) {
			st = STATUS_DEVICE_BUSY;
			break;
		}
		st = do_ioctl(d, isl->Parameters.DeviceIoControl.IoControlCode, irp->AssociatedIrp.SystemBuffer,
			      isl->Parameters.DeviceIoControl.InputBufferLength,
			      isl->Parameters.DeviceIoControl.OutputBufferLength, &info);
		win_sync(d);
		__sync_lock_release(&d->busy);
		break;
	default:
		st = STATUS_INVALID_DEVICE_REQUEST;
		break;
	}
	irp->IoStatus.Status = st;
	irp->IoStatus.Information = info;
	IofCompleteRequest(irp, IO_NO_INCREMENT);
	return st;
}

static void driver_unload(PDRIVER_OBJECT drv)
{
	struct win_dev *d = &g_win;

	(void)drv;
	if (d->link_created)
		(void)IoDeleteSymbolicLink(&link_name);
	if (d->ctl)
		IoDeleteDevice(d->ctl);
	d->link_created = 0;
	d->ctl = NULL;
	win_step(d, J_WIN_UNLOAD, d->g.stage_done, 0);
	gm20b_fini(&d->g);                                /* GPU back into reset, clocks we enabled off */
	win_rec(d, J_WIN_UNLOAD_DONE, 0, 0);
	win_sync(d);
	win_unmap_all(d);
	d->g_valid = 0;
	win_jf_close(d);
}

static void create_ctl(struct win_dev *d, PDRIVER_OBJECT drv)
{
	NTSTATUS st1, st2 = STATUS_UNSUCCESSFUL;

	d->ctl = NULL;
	d->link_created = 0;
	st1 = IoCreateDevice(drv, 0, &dev_name, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, TRUE, &d->ctl);
	if (NT_SUCCESS(st1)) {
		st2 = IoCreateSymbolicLink(&link_name, &dev_name);
		if (NT_SUCCESS(st2)) {
			d->link_created = 1;
		} else {
			IoDeleteDevice(d->ctl);
			d->ctl = NULL;
		}
	} else {
		d->ctl = NULL;
	}
	win_rec(d, J_WIN_DEVICE, (u32)st1, (u32)st2);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg_path)
{
	struct win_dev *d = &g_win;
	struct gm20b_hal hal;
	struct gm20b_cfg cfg;
	int rc;

	gm20b_zero(d, sizeof(*d));
	win_jf_open(d);
	win_rec(d, J_WIN_LOAD, GM20B_WIN_VERSION, GM20B_IOCTL_ABI);
	win_rec(d, J_WIN_FILE, (u32)d->jf_open_status, d->jf_created);
	win_cfg_load(d, reg_path, &cfg);
	d->cfg = cfg;
	win_sync(d);                                      /* the configuration of this run is durable now */

	rc = win_map_all(d);
	win_hal_fill(d, &hal);
	gm20b_ctx_init(&d->g, &hal, &cfg);
	d->g_valid = 1;
	if (rc == GM20B_OK) {
		win_step(d, J_WIN_INIT_BEGIN, d->g.cfg.max_stage, d->g.cfg.flags);
		d->init_status = gm20b_init(&d->g);
		gm20b_jflush(&d->g);
		win_rec(d, J_WIN_INIT_DONE, (u32)d->init_status, d->g.stage_done);
	} else {
		d->init_status = rc;                      /* a mapping failed: nothing was touched */
		win_rec(d, J_WIN_INIT_DONE, (u32)rc, 0);
	}

	drv->DriverUnload = driver_unload;
	drv->MajorFunction[IRP_MJ_CREATE] = dispatch;
	drv->MajorFunction[IRP_MJ_CLOSE] = dispatch;
	drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = dispatch;
	create_ctl(d, drv);
	win_sync(d);
	return STATUS_SUCCESS;                            /* stay loaded even if the bring-up failed */
}
