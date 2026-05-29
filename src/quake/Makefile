# quake — openfpgaOS custom core.
#
# Everything lives under engine/: stock Id Software files + the
# openfpgaOS-specific drivers (main.c, sys_of.c, vid_of.c, in_of.c,
# snd_of.c, of_accel.c). One source directory — no split between
# engine and SDK shims — so every rebuild picks up every change.

APP       = quake
TARGET    = pocket
SDK_DIR   = ../sdk
ROOT      = $(realpath $(CURDIR)/../..)
OBJ_DIR   = $(ROOT)/.obj/$(APP)
BUILD_DIR = $(OBJ_DIR)
DIST      = $(ROOT)/dist/$(APP)
OUT       = $(ROOT)/build/$(APP)
RUNTIME   = $(ROOT)/runtime

SRCS        = $(wildcard engine/*.c)
SRCS_CXX    =

# Engine was written in the 90s — relax the warning diet so vanilla
# id Software code compiles cleanly. Hot-path renderers get -ffast-math
# too, since the scan/edge walkers rely on FP associativity Quake
# already tolerated.
CFLAGS  = -Iengine
CFLAGS += -std=gnu11 -fcommon -DQUAKE_OPENFPGA=1
CFLAGS += -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function
CFLAGS += -Wno-missing-field-initializers -Wno-sign-compare
CFLAGS += -Wno-implicit-function-declaration -Wno-pointer-sign
CFLAGS += -Wno-discarded-qualifiers -Wno-parentheses -Wno-strict-prototypes
CFLAGS += -Wno-old-style-definition -Wno-format -Wno-format-security
CFLAGS += -Wno-implicit-fallthrough -Wno-unused-result
CFLAGS += -Wno-maybe-uninitialized -Wno-address
CFLAGS += -Wno-enum-conversion -Wno-int-conversion
# Hot renderer paths see a decent lift from fast-math — only the code
# that was already tolerant of it on x87, not global.
CFLAGS += -ffast-math -fno-math-errno -fno-trapping-math

# `-O3 + LTO` measured ~13% faster than `-O2` on a dense-geometry scene
# (113.1 ms → 98.3 ms, AET total 846 / poly 412).  Trades +10% text
# (411888 → 451992) for cross-TU inlining of the r_edge ↔ r_draw ↔
# d_scan hot path.  An earlier "regression" against a 68.2 ms reading
# was apples-to-oranges — that capture was at AET 151 (5× sparser).
CFLAGS  += -O3 -flto=auto
LDFLAGS += -O3 -flto=auto

.DEFAULT_GOAL := all

include $(SDK_DIR)/sdk.mk

all: $(OBJ_DIR)/app.elf release
	@$(SIZE) $<

release: $(OBJ_DIR)/app.elf
	@rm -rf $(OUT)
	@mkdir -p $(dir $(OUT))
	@cp -r $(DIST) $(OUT)
	@cp $(RUNTIME)/bitstream.rbf_r $(RUNTIME)/loader.bin $$(ls -d $(OUT)/Cores/*/)/
	@mkdir -p $$(ls -d $(OUT)/Assets/*/)/common
	@cp $(RUNTIME)/os.bin $$(ls -d $(OUT)/Assets/*/)/common/
	@cp $< $$(ls -d $(OUT)/Assets/*/)/common/$(APP).elf
	@for f in *.wad *.pak *.dat *.png; do [ -f "$$f" ] && cp "$$f" $$(ls -d $(OUT)/Assets/*/)/common/; done 2>/dev/null; true
	@echo "Ready: build/$(APP)/"

debug: all
	@$(ROOT)/scripts/debug.sh "$$(ls -d $(OUT)/Assets/*/common)/$(APP).elf"

copy: all
	@$(SDK_DIR)/platforms/$(TARGET)/copy.sh "$(APP)" "$$(ls -d $(OUT)/Assets/*/common)/$(APP).elf"

package: all
	@$(ROOT)/scripts/package.sh $(APP)

test: app_pc

clean: sdk-clean
	rm -rf $(OUT)

.PHONY: all release debug copy package test clean
