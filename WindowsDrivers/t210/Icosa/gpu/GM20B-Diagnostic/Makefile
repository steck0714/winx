# winx-gm20b -- host tests + freestanding Windows-ARM64 compile checks.
#   make test          build + run the host test-suite (mock GM20B), with ASan/UBSan if available
#   make check-win     compile every core/*.c for aarch64-pc-windows-msvc, freestanding, -Werror
#   make check-includes  core/ and regs/ may only include <stdint|stddef|stdbool|stdarg>.h
#   make check         all of the above
#   make STAGES="5 6" BUILD=build/B test   build only your stages for real (others = stubs) in your own dir
#   make driver        (win/) cross-build gm20b.sys + gm20bctl.exe with clang/lld  [see win/README]
CC       = clang
BUILD   := build
INC     := -Icore -Iregs -Isim
# STAGES=all (default) builds everything. STAGES="5 6" builds only stage 5+6 for real and links the
# skeleton stubs (core/stubs) for the others, so parallel implementers do not break each other.
# Per-stage test/mock files are named sim/test_sN_*.c and sim/mock_sN_*.c.
STAGES ?= all
ifeq ($(STAGES),all)
CORE_SRC := $(sort $(wildcard core/*.c))
SIM_SRC  := $(sort $(wildcard sim/*.c))
else
CORE_SRC := core/gm20b_core.c $(sort $(foreach n,$(STAGES),$(wildcard core/stage$(n)_*.c))) \
            $(sort $(foreach n,$(filter-out $(STAGES),1 2 3 4 5 6 7 8),$(wildcard core/stubs/stage$(n)_*.c)))
SIM_SRC  := sim/mock.c sim/test_main.c sim/test_core.c sim/test_regs.c \
            $(sort $(foreach n,$(STAGES),$(wildcard sim/test_s$(n)_*.c sim/mock_s$(n)_*.c)))
endif
HDRS     := $(wildcard core/*.h regs/*.h regs/nvgpu/*.h sim/*.h)

WARN := -Wall -Wextra -Werror -Wno-unused-parameter -Wshadow -Wundef -Wstrict-prototypes -Wmisleading-indentation
HOSTFLAGS := -std=gnu11 -O1 -g $(WARN) -DGM20B_TESTHOOKS $(INC)
SAN := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -fsanitize=address,undefined -o /dev/null 2>/dev/null && echo -fsanitize=address,undefined -fno-omit-frame-pointer)

WINFLAGS := --target=aarch64-pc-windows-msvc -ffreestanding -fno-builtin -fno-stack-protector \
            -mgeneral-regs-only -O2 -std=gnu11 $(WARN) -Wno-unused-function $(INC)

.PHONY: all test check check-win check-includes check-driver clean driver mutate
all: check

$(BUILD)/host/test_runner: $(CORE_SRC) $(SIM_SRC) $(HDRS) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(HOSTFLAGS) $(SAN) $(CORE_SRC) $(SIM_SRC) -o $@

test: $(BUILD)/host/test_runner
	$(BUILD)/host/test_runner $(T)

check-win:
	@mkdir -p $(BUILD)/win
	@for f in $(CORE_SRC); do \
	  echo "  CC(win-arm64) $$f"; \
	  $(CC) $(WINFLAGS) -c $$f -o $(BUILD)/win/$$(basename $$f .c).o || exit 1; \
	done
	@echo "no FP/SIMD registers used:"; \
	n=$$(llvm-objdump -d $(BUILD)/win/*.o 2>/dev/null | grep -cE '[[:space:]](ldr|str|ldp|stp|fmov|fadd|fmul|movi|dup|ins|ld1|st1)[[:space:]]+[qdsv][0-9]+' || true); \
	echo "  fp/simd insns: $$n"; test "$$n" = "0"

check-includes:
	@bad=$$(grep -nE '^[[:space:]]*#[[:space:]]*include[[:space:]]*<' core/*.c core/*.h regs/*.h 2>/dev/null | grep -vE '<(stdint|stddef|stdbool|stdarg)\.h>' || true); \
	if [ -n "$$bad" ]; then echo "forbidden system includes in core/regs:"; echo "$$bad"; exit 1; else echo "  includes ok"; fi

check: check-includes test check-win check-driver

driver:
	$(MAKE) -C win

# mutation sanity check: break the code on purpose in scratch copies, every mutant must be caught (1-2 minutes)
mutate:
	$(MAKE) -C win mutate

# Windows shell: PE structure, imports, struct layouts vs mingw-w64 DDK headers, no FP/SIMD, host test with a fake ntoskrnl
check-driver:
	$(MAKE) -C win check

clean:
	rm -rf $(BUILD)
