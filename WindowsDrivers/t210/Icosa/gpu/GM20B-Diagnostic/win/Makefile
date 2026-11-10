# winx-gm20b -- Windows ARM64 build of gm20b.sys and gm20bctl.exe with clang + lld-link (no WDK, no Windows SDK).
#   make            build both into ../build/win
#   make sys|ctl    build one of them
#   make check      build + static checks (PE structure, imports, struct layouts, no FP/SIMD) + host test of the shell
#   make dist       stage ../build/dist/gm20b-win/ (driver, tool, scripts, decoder, docs)
# The core is compiled in as it is in ../core (stages 3..8 are still stubs there).
ROOT    := ..
OUT     ?= $(ROOT)/build/win
CC       = clang
LINK    ?= lld-link
DLLTOOL ?= llvm-dlltool
PY      ?= python3

ARCH   := --target=aarch64-pc-windows-msvc -march=armv8-a -mgeneral-regs-only -mstrict-align -mno-outline-atomics
# -funwind-tables: clang emits no .pdata/.xdata for freestanding code otherwise, and the ARM64 kernel wants them
CFLAGS := $(ARCH) -ffreestanding -fno-builtin -fno-stack-protector -fno-strict-aliasing -funwind-tables -O2 -std=gnu11 \
          -Wall -Wextra -Werror -Wshadow -Wmisleading-indentation -Wstrict-prototypes -Wno-unused-parameter \
          -I$(ROOT)/core -I$(ROOT)/regs -Iinclude -Idriver

DRV_SRC  := $(sort $(wildcard driver/*.c))
CORE_SRC := $(sort $(wildcard $(ROOT)/core/*.c))
DRV_OBJ  := $(patsubst driver/%.c,$(OUT)/drv/%.o,$(DRV_SRC))
CORE_OBJ := $(patsubst $(ROOT)/core/%.c,$(OUT)/core/%.o,$(CORE_SRC))
HDRS     := $(wildcard driver/*.h include/*.h $(ROOT)/core/*.h $(ROOT)/regs/*.h $(ROOT)/regs/nvgpu/*.h)

.PHONY: all sys ctl check check-host check-pe check-fp check-layout check-jcodes mutate dist clean
all: sys ctl
sys: $(OUT)/gm20b.sys
ctl: $(OUT)/gm20bctl.exe

$(OUT)/drv/%.o: driver/%.c $(HDRS) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/core/%.o: $(ROOT)/core/%.c $(HDRS) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/ntoskrnl.lib: ntoskrnl.def
	@mkdir -p $(OUT)
	$(DLLTOOL) -m arm64 -D ntoskrnl.exe -d $< -l $@

$(OUT)/kernel32.lib: kernel32.def
	@mkdir -p $(OUT)
	$(DLLTOOL) -m arm64 -D KERNEL32.dll -d $< -l $@

# /release stores the PE checksum (the kernel refuses drivers with a wrong one); /driver:wdm sets the WDM flag.
$(OUT)/gm20b.sys: $(DRV_OBJ) $(CORE_OBJ) $(OUT)/ntoskrnl.lib
	$(LINK) /nologo /subsystem:native,10.0 /driver:wdm /entry:DriverEntry /nodefaultlib /machine:arm64 /release \
	    /out:$@ /map:$(OUT)/gm20b.map $(DRV_OBJ) $(CORE_OBJ) $(OUT)/ntoskrnl.lib

$(OUT)/ctl/gm20bctl.o: tools/gm20bctl.c $(HDRS) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/gm20bctl.exe: $(OUT)/ctl/gm20bctl.o $(OUT)/kernel32.lib
	$(LINK) /nologo /subsystem:console,10.0 /entry:mainCRTStartup /nodefaultlib /machine:arm64 /release \
	    /out:$@ $(OUT)/ctl/gm20bctl.o $(OUT)/kernel32.lib

# ---- host test of the shell logic against a fake ntoskrnl (Linux, ASan/UBSan if available) --------------------
HOSTCC     = clang
HOSTSAN   := $(shell echo 'int main(void){return 0;}' | $(HOSTCC) -x c - -fsanitize=address,undefined -o /dev/null 2>/dev/null && echo -fsanitize=address,undefined -fno-omit-frame-pointer)
HOSTFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wshadow -Wmisleading-indentation -Wno-unused-function -Wno-unused-parameter \
             -DNTK_HOST_TEST -DGM20B_HOSTTEST -fshort-wchar -I$(ROOT)/core -I$(ROOT)/regs -Iinclude -Idriver
HOST_SRC   := tests/host_shell_test.c driver/gm20b_sys.c driver/hal_win.c driver/win_journal.c driver/win_cfg.c $(CORE_SRC)

$(OUT)/host_shell_test: $(HOST_SRC) $(HDRS) Makefile
	@mkdir -p $(OUT)
	$(HOSTCC) $(HOSTFLAGS) $(HOSTSAN) $(HOST_SRC) -o $@

# The scenarios also save journals produced by the real shell code (GM20B_DUMP_DIR); they must decode on the PC,
# and a simulated hang (file and registry value as they were at the first BOOT0 access) must name its culprit.
EX := $(OUT)/examples
check-host: $(OUT)/host_shell_test tools/journal_decode.py
	mkdir -p $(EX)
	GM20B_DUMP_DIR=$(EX) ASAN_OPTIONS=allow_user_segv_handler=1:detect_leaks=0 $(OUT)/host_shell_test
	$(PY) -I tools/journal_decode.py $(EX)/stage2-ok.bin > $(EX)/stage2-ok.txt
	$(PY) -I tools/journal_decode.py $(EX)/cold-chip.bin --explain > $(EX)/cold-chip.txt
	$(PY) -I tools/journal_decode.py $(EX)/hang-at-boot0.bin --explain > $(EX)/hang-at-boot0.txt
	$(PY) -I tools/journal_decode.py --hex "$$(cat $(EX)/hang-at-boot0.crumb.txt)" > $(EX)/hang-at-boot0.crumb.decoded.txt
	grep -q 'LAST RECORD: .*STEP J_S2_BOOT0_INTENT' $(EX)/hang-at-boot0.txt
	grep -q 'STEP J_S2_BOOT0_INTENT' $(EX)/hang-at-boot0.crumb.decoded.txt
	grep -q 'J_WIN_INIT_DONE .*OK, stages done 3' $(EX)/stage2-ok.txt
	grep -q 'J_S1_VERIFY_FAIL' $(EX)/cold-chip.txt

check-pe: all
	$(PY) -I tests/verify_pe.py kernel $(OUT)/gm20b.sys ntoskrnl.def
	$(PY) -I tests/verify_pe.py user $(OUT)/gm20bctl.exe kernel32.def

check-fp: all
	sh tests/check_fp.sh $(OUT)/gm20b.sys $(OUT)/gm20bctl.exe

check-layout:
	OUT=$(OUT) sh tests/check_layout.sh

check-jcodes:
	$(PY) -I tools/journal_decode.py --selftest

check: check-pe check-fp check-layout check-jcodes check-host
	@echo "win: all checks passed"

# Mutation sanity check (not part of `check`, about 1-2 minutes): breaks the code on purpose in scratch copies of the tree
# and requires that some test notices every one of the mutants.  Add a mutant to tests/mutate.py whenever you add a guard.
mutate:
	$(PY) -I tests/mutate.py

# ---- distribution folder ----------------------------------------------------------------------------------------
DIST := $(ROOT)/build/dist/gm20b-win
dist: all check-host
	rm -rf $(DIST)
	mkdir -p $(DIST)/examples $(DIST)/provenance
	cp $(OUT)/gm20b.sys $(OUT)/gm20bctl.exe $(OUT)/gm20b.map $(DIST)/
	for f in scripts/*.cmd; do sed 's/$$/\r/' $$f > $(DIST)/$$(basename $$f); done
	cp tools/journal_decode.py $(DIST)/
	$(PY) -I tools/journal_decode.py --dump-codes $(DIST)/jcodes.tsv
	-cp $(ROOT)/docs/WINDOWS.md $(ROOT)/docs/DESIGN.md $(ROOT)/NOTICE.md $(DIST)/
	-cp $(ROOT)/docs/provenance/*.md $(DIST)/provenance/
	cp $(EX)/*.txt $(EX)/*.bin $(DIST)/examples/
	cd $(DIST) && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS
	@echo "staged $(DIST)  (gm20b.sys is UNSIGNED: sign it with your own test certificate)"

clean:
	rm -rf $(OUT)
