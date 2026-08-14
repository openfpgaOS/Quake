# quake — openfpgaOS custom core.
#
# Everything lives under engine/: stock Id Software files + the
# openfpgaOS-specific drivers (main.c, sys_of.c, vid_of.c, in_of.c,
# snd_of.c, of_accel.c). One source directory — no split between
# engine and SDK shims — so every rebuild picks up every change.

APP       = quake
TARGET   ?= pocket
MISTER_IP ?= mister.local
SDK_DIR   = ../sdk
ROOT      = $(realpath $(CURDIR)/../..)
OBJ_DIR   = $(ROOT)/.obj/$(APP)
BUILD_DIR = $(OBJ_DIR)
DIST      = $(ROOT)/dist/$(APP)
OUT       = $(ROOT)/build/$(TARGET)/$(APP)
RUNTIME   = $(ROOT)/runtime
# Game data files shipped into the per-target deliverable, when present
# alongside the Makefile (PAKs etc. are usually user-supplied).
GAME_ASSETS = $(wildcard *.wad *.pak *.dat *.png)

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

# Per-target deliverable: pocket = APF SD tree (image.sh), mister = the
# per-game bundle built by game-dist below.  image.sh is a POCKET-only script
# (platforms/mister/ has none), so the mister path must never fall through to
# it — that is how a mister build "links fine, then dies with error 127".
ifeq ($(TARGET),mister)
release: game-dist        # MiSTer: the per-game bundle IS the release artifact
else
release: $(OBJ_DIR)/app.elf
	@$(SDK_DIR)/platforms/$(TARGET)/image.sh "$(APP)" "$(OBJ_DIR)/app.elf" "$(ROOT)" "$(DIST)" $(GAME_ASSETS)
endif

debug: all
	@$(ROOT)/scripts/debug.sh "$$(ls -d $(OUT)/Assets/*/common 2>/dev/null)/$(APP).elf"

# MISTER_IP rides the environment so the same call serves every platform.
copy: all
ifeq ($(TARGET),mister)
	@MISTER_IP="$(MISTER_IP)" $(SDK_DIR)/platforms/mister/copy.sh game "$(GAME)" "$(GAME_ELF)" "$(OBJ_DIR)/app.elf"
else
	@MISTER_IP="$(MISTER_IP)" $(SDK_DIR)/platforms/$(TARGET)/copy.sh "$(APP)" "$(OBJ_DIR)/app.elf"
endif

# Engine-only update — identical to `make copy`: for MiSTer that pushes the
# loose games/OpenfpgaOS/<Game>/<GameElf> (the F-loaded engine), leaving
# boot.vhd (your PAKs) and the saves volume untouched.  Kept as an alias.
copy-app: all
	@$(MAKE) copy

# `make package` for MiSTer builds the per-game locked distribution (a
# boot.vhd shell + .mgl launchers + loose inis + setup.sh + external_files.csv,
# zipped for games/OpenfpgaOS/); every other target keeps the single-image
# path.  scripts/package.sh -> platforms/mister/package.sh auto-detects which.
#
# For MiSTer it ALSO generates the Downloader custom-database delivery artifact
# (releases/mister/quake.json.zip) via mkdb.py — a real ZIP-wrapped JSON the
# stock MiSTer Downloader syncs.  external_files.csv is folded into it here at
# build time (it is NOT a device file).  db_id must never change once published.
#
# Publishing model (edit these for the real release):
#   MISTER_DB_ID       downloader.ini [section] == db_id
#   MISTER_DB_BASE_URL where the STAGED tree is hosted; a local file at sd path
#                      <p> resolves to  $(MISTER_DB_BASE_URL)<p>  (base_files_url
#                      + path — the Downloader's own rule).  Host the game-dist
#                      tree under games/OpenfpgaOS/ at this base.
#   MISTER_DB_URL      where quake.json.zip itself is published (for the ini snippet)
MISTER_DB_ID       ?= openfpgaOS/openfpgaos-quake
# Version of THIS app's core.json — pins the DB's asset URLs to this
# release's own tag.  releases/latest/download is ONE pointer per REPO:
# with several release streams (doom/heretic/hexen) it resolves to
# whichever core released last, serving the wrong stream's assets.
MISTER_DB_VER  := $(shell python3 -c "import glob,json;print(json.load(open(sorted(glob.glob('$(DIST)/Cores/*/core.json'))[0]))['core']['metadata']['version'])" 2>/dev/null || echo 0.0.0)
MISTER_DB_BASE_URL ?= https://github.com/openfpgaOS/Quake/releases/download/quake-mister-v$(MISTER_DB_VER)/
# The DB itself is tiny — committed to the repo and served via raw, a
# stable per-game pointer immune to the shared 'latest' problem.
MISTER_DB_URL      ?= https://raw.githubusercontent.com/openfpgaOS/Quake/main/releases/mister/quake.json.zip
MISTER_DB          = $(ROOT)/releases/mister/$(APP).json.zip
MISTER_DB_INI      = $(ROOT)/releases/mister/$(APP).downloader.ini
ifeq ($(TARGET),mister)
package: game-dist
	@$(ROOT)/scripts/package.sh $(APP)
	@$(SDK_DIR)/platforms/mister/mkdb.py \
	    --staging "$(GAME_STAGE)" --sd-prefix "games/OpenfpgaOS" \
	    --menu-dir "_Computer/_OpenfpgaOS/_$(GAME)" \
	    --base-url "$(MISTER_DB_BASE_URL)" --url-mode flat --db-id "$(MISTER_DB_ID)" \
	    --external "$(GAME_CSV)" --db-url "$(MISTER_DB_URL)" \
	    --output "$(MISTER_DB)" --ini-out "$(MISTER_DB_INI)"
else
package: all
	@$(ROOT)/scripts/package.sh $(APP)
endif

# ── MiSTer per-game locked distribution (S0 boot shell + S1 saves + .mgl) ─
# The core (openfpgaOS.rbf + os.bin) is game-agnostic.  This game ships a pair
# of images: a READ-ONLY boot.vhd (S0 — /$(GAME)/common: bank.ofsf, PAKs added
# on-device; NO engine ELF) and a writable $(GAME).saves.vhd (S1 — one
# /$(GAME)/<Instance> subfolder of preallocated config/save slots per instance
# ini).  Splitting them keeps saves on a separate, Downloader-reserved volume
# so an update can replace boot.vhd without touching the player's saves.  The
# engine ELF ships as a LOOSE file ($(GAME)/$(GAME_ELF), next to boot.vhd): the
# firmware F-loads it at ioctl index 2 into ELF staging and serves it as slot 3,
# so an engine update is one loose-file swap (mkdb.py indexes it as a Downloader
# `files` entry) and boot.vhd never changes.  Also ships: a 4-line .mgl launcher
# per instance (S0 boot + S1 saves + F-load ELF index 2 + F-load ini index 1;
# ELF BEFORE ini — the ini F-load resets and triggers the app load), the loose
# per-instance inis, setup.sh (seeds the saves image + injects the user's PAKs)
# and external_files.csv (freeware data list).
# `make package TARGET=mister` builds and zips this bundle.
#
# GAME_INIS  : canonical per-instance inis (each with [os] GAME/INSTANCE/ELF)
# GAME_STAGE : games/OpenfpgaOS/-rooted staging that package.sh zips
# GAME_COMMON: throwaway dir feeding mkgame the bank.ofsf (kept OUT of the
#              staging so it is never zipped)
GAME        = Quake
GAME_INIS   = $(DIST)/Assets/quake/common
GAME_STAGE  = $(ROOT)/build/mister/$(APP)
GAME_COMMON = $(ROOT)/build/mister/.$(APP)-common
GAME_ELF    = quake.elf
GAME_CSV    = $(DIST)/mister/external_files.csv
# Explicit shell size: id1 paks + both mission packs are ~130 MB — far past
# the 48 MB FAT32 floor mkgame defaults to.  Ships gzipped, nearly free.
GAME_BOOT_MB = 256

game-dist: $(OBJ_DIR)/app.elf
	@rm -rf "$(GAME_STAGE)" "$(GAME_COMMON)"
	@mkdir -p "$(GAME_STAGE)/$(GAME)/wads" "$(GAME_COMMON)"
	@cp "$(RUNTIME)/bank.ofsf" "$(GAME_COMMON)/bank.ofsf"
	@$(SDK_DIR)/platforms/mister/mkgame.sh --no-elf --saves-out "$(GAME_STAGE)/$(GAME)/$(GAME).saves.vhd" "$(GAME)" "$(GAME_INIS)" "$(GAME_COMMON)" "$(GAME_STAGE)/$(GAME)/boot.vhd" $(GAME_BOOT_MB)
	@gzip -9nf "$(GAME_STAGE)/$(GAME)"/boot*.vhd
	@$(SDK_DIR)/platforms/mister/mkmgl.sh "$(GAME)" "$(GAME_INIS)" "$(GAME_STAGE)"
	@cp "$(OBJ_DIR)/app.elf" "$(GAME_STAGE)/$(GAME)/$(GAME_ELF)"
	@cp "$(GAME_INIS)"/*.ini "$(GAME_STAGE)/$(GAME)/"
	@sed 's|^GAME_DEFAULT=.*|GAME_DEFAULT="$(GAME)"|' "$(SDK_DIR)/platforms/mister/setup.sh" > "$(GAME_STAGE)/$(GAME)/setup.sh"
	@chmod +x "$(GAME_STAGE)/$(GAME)/setup.sh"
	@cp "$(GAME_CSV)" "$(GAME_STAGE)/$(GAME)/external_files.csv"
	@rm -rf "$(GAME_COMMON)"
	@echo "  game-dist staged -> $(GAME_STAGE)"

test: app_pc

clean: sdk-clean
	rm -rf $(OUT)

.PHONY: all release debug copy copy-app package test clean game-dist
