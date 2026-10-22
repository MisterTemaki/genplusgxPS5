# Genesis Plus GX PS5
#   make ps5    -> build/ps5/GenesisPlusGXPS5.elf   the payload to send with the PS5 Payload Manager / ELF loader:
#                  it installs or updates the dashboard app /data/homebrew/$(TITLE_ID)/ (Genesis Plus GX itself,
#                  icon included) and stays running as the helper that lets the app reach /data
#   make app    -> build/app/$(TITLE_ID)/     the app folder on its own (eboot.bin, sce_module/, sce_sys/)
#   make host   -> build/host/                Linux builds of the same code (PS5 calls faked by host/sce_host.cpp)
#   make test   -> host builds + tests/run-tests.sh
#   make dist   -> build/dist/GenesisPlusGXPS5-v$(VERSION).elf + genplus-ps5-v$(VERSION)-src.zip (release names)
#   make send   -> sends GenesisPlusGXPS5.elf to $(PS5_HOST):$(PS5_PORT) with the SDK's prospero-deploy
#
# The layout is PS5SX2's (and Snes9x PS5's, Mesen2 PS5's): eboot.bin is the emulator, a native PS5 title (the
# console gives the controller to the title in front only), and the payload is its installer and helper:
#   1. build/ps5/GenesisPlusGXPS5-helper.elf   the helper alone (payload); the app sends it to the ELF loader
#   2. build/app/$(TITLE_ID)/                   eboot.bin = Genesis Plus GX's core (through its libretro port,
#                                               ../libretro/libretro.c) + the PS5 frontend + the helper from
#                                               step 1 (native title, signed); sce_module/libc.prx; sce_sys/
#   3. build/ps5/GenesisPlusGXPS5.elf          installer + helper, with the app folder from step 2 built in
#
# Genesis Plus GX's core (../core, ../libretro) is compiled as it is, with the flags of its own
# Makefile.libretro (unix): every one of its sources builds for the PS5 unchanged.
#
# Dependencies: the ps5-payload-dev SDK (v0.42 or newer, https://github.com/ps5-payload-dev/sdk) in
# $(PS5_PAYLOAD_SDK), with clang/lld 18 on the host.

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
PS5_HOST ?= ps5
PS5_PORT ?= 9021
VERSION ?= 2.0.2
CORE_VERSION := 1.7.4
# the GPU's shader tables, generated from shaders/presets.txt (see "shaders:" below)
SLANG_CPP := build/gen/slang_presets.cpp
TITLE_ID ?= PPSA99921
# where the app looks for its updates (Settings, SYSTEM, Updates): this repository's GitHub releases
UPDATE_REPO ?= MisterTemaki/genplusgxPS5
APP_OUT := build/app/$(TITLE_ID)

ROOT := ..

# Genesis Plus GX: its libretro build's sources (libretro/Makefile.common, HAVE_CHD=1), Tremor for the Ogg CD
# tracks, libchdr (with its zlib, LZMA and Zstandard) for .chd discs.
CHD := core/cd_hw/libchdr
CORE_DIRS := core core/z80 core/m68k core/ntsc core/sound core/sound/minimp3 core/input_hw core/cd_hw core/cart_hw \
	core/cart_hw/svp core/sound/tremor
CORE_C_SRC := $(sort $(foreach d,$(CORE_DIRS),$(patsubst $(ROOT)/%,%,$(wildcard $(ROOT)/$(d)/*.c)))) \
	$(CHD)/src/libchdr_bitstream.c $(CHD)/src/libchdr_cdrom.c $(CHD)/src/libchdr_chd.c $(CHD)/src/libchdr_flac.c \
	$(CHD)/src/libchdr_huffman.c \
	$(CHD)/deps/lzma-24.05/src/LzFind.c $(CHD)/deps/lzma-24.05/src/LzmaEnc.c $(CHD)/deps/lzma-24.05/src/LzmaDec.c \
	$(CHD)/deps/lzma-24.05/src/CpuArch.c \
	$(addprefix $(CHD)/deps/zstd-1.5.6/lib/,decompress/zstd_decompress.c decompress/huf_decompress.c \
		decompress/zstd_decompress_block.c decompress/zstd_ddict.c common/entropy_common.c common/error_private.c \
		common/fse_decompress.c common/xxhash.c common/zstd_common.c) \
	$(addprefix $(CHD)/deps/zlib-1.3.1/,adler32.c inffast.c inflate.c inftrees.c zutil.c) \
	$(addprefix libretro/libretro-common/,streams/file_stream.c streams/file_stream_transforms.c \
		compat/fopen_utf8.c compat/compat_strl.c encodings/encoding_utf.c vfs/vfs_implementation.c) \
	libretro/libretro.c libretro/scrc32.c
CORE_SRC :=
CORE_DEFS := -DZSTD_TRACE=0 -DLSB_FIRST -DBYTE_ORDER=LITTLE_ENDIAN -DHAVE_ZLIB -DMAXROMSIZE=33554432 -DUSE_LIBTREMOR -DUSE_LIBCHDR \
	-DHAVE_LROUND -DHAVE_STDINT_H -DZ7_ST -DZSTD_DISABLE_ASM -DHAVE_SYS_PARAM_H -DUSE_16BPP_RENDERING \
	-DFRONTEND_SUPPORTS_RGB565 -D__LIBRETRO__ -DUSE_LIBRETRO_VFS -DM68K_OVERCLOCK_SHIFT=20 -DZ80_OVERCLOCK_SHIFT=20 \
	-DHAVE_YM3438_CORE -DHAVE_OPLL_CORE '-DINLINE=static inline'

# netfs: network shares as folders (SMB and NFS), from Snes9x PS5 (thatboialex's design; netfs/netfs_backend.h).
# backend_smb.cpp / backend_nfs.cpp are the protocols; the stubs (which fail with "not built in") stand in for them in
# the netfs unit test.
NETFS_CORE_SRC := netfs/netfs_share.cpp netfs/netfs_core.cpp netfs/netfs_cache.cpp netfs/netfs_vfs.cpp netfs/netfs_wrap.cpp netfs/netfs_edit.cpp netfs/netfs_shims_ps5.cpp
# the libc calls netfs answers for /smb/... and /nfs/... paths (netfs/netfs_wrap.cpp has a __wrap_ function for each).
# They go into both links as --wrap, so a call anywhere in the app (the core and libretro's file streams, libchdr,
# minizip, libc++'s file streams) lands in netfs first and is passed on to the real call when the path or handle is
# not netfs's. A Sega CD disc on a share is read this way while the game runs (netfs_cache.h: blocks, read-ahead).
NETFS_WRAPPED := fopen fclose fread fseek fseeko ftell ftello rewind feof ferror clearerr \
	fileno fgetc getc ungetc fgets fflush setvbuf flockfile funlockfile open close read pread lseek fstat stat lstat \
	access realpath opendir readdir closedir
# glibc only: the console's libc has no 64-bit variants (off_t is 64 bits), and minizip maps its fopen64 calls to
# fopen on FreeBSD targets (third_party/ioapi.h), so a --wrap of these would leave __real_* undefined in the console link.
NETFS_WRAPPED_LFS64 := fopen64 fseeko64 ftello64 stat64 lstat64 fstat64 readdir64 open64 pread64 lseek64
# console only (netfs/netfs_shims_ps5.cpp): sockets that take SO_NBIO instead of fcntl, and a getaddrinfo that works in a native title
NETFS_WRAPPED_PS5 := fcntl getaddrinfo freeaddrinfo
NETFS_WRAP_LLD := $(foreach s,$(NETFS_WRAPPED) $(NETFS_WRAPPED_PS5),--wrap=$(s))
NETFS_WRAP_GCC := $(foreach s,$(NETFS_WRAPPED) $(NETFS_WRAPPED_LFS64),-Wl,--wrap=$(s))
NETFS_SRC := $(NETFS_CORE_SRC) netfs/backend_smb.cpp netfs/backend_nfs.cpp
# libsmb2 (57fea04) and libnfs (2d368ad), LGPL-2.1-or-later, vendored with their notices as Snes9x PS5 has them
# (third_party/libsmb2/README.snes9xps5, third_party/libnfs/PATCHES.md). libsmb2 is built from a config.h of its folder
# (no CMake). It needs no PS4_PLATFORM: prospero-clang defines __FreeBSD__, which selects libsmb2's FreeBSD headers.
# Their fcntl and getaddrinfo calls reach the console shims through the --wrap list above.
LIBSMB2 := third_party/libsmb2
LIBNFS := third_party/libnfs
LIBSMB2_SRC := $(sort $(wildcard $(LIBSMB2)/lib/*.c))
LIBNFS_SRC := $(sort $(foreach d,lib mount nfs nfs4 portmap nlm nsm rquota,$(wildcard $(LIBNFS)/$(d)/*.c)))
LIBSMB2_INC := -I$(LIBSMB2)/include -I$(LIBSMB2)/include/smb2 -I$(LIBSMB2)/lib
LIBSMB2_FLAGS := -I$(LIBSMB2) $(LIBSMB2_INC) -DHAVE_CONFIG_H -D_FILE_OFFSET_BITS=64 '-D_U_=__attribute__((unused))'
LIBNFS_FLAGS := -DHAVE_CONFIG_H -I$(LIBNFS) -I$(LIBNFS)/include -I$(LIBNFS)/include/nfsc \
	$(addprefix -I$(LIBNFS)/,mount nfs nfs4 nlm nsm portmap rquota)
# netfs/backend_smb.cpp includes libsmb2's headers as system headers (they are not ours to keep -Werror clean)
%/port/netfs/backend_smb.o: EXTRA_FLAGS = $(patsubst -I%,-isystem %,$(LIBSMB2_INC))
%/port/netfs/backend_nfs.o: EXTRA_FLAGS = $(LIBNFS_FLAGS)

# the app (eboot.bin): the PS5 layer and the frontend
EMU_SRC := coreorbis/main-boot.cpp \
	coreorbis/orbis-shims/orbis_paths.cpp coreorbis/orbis-shims/ProsperoNotify.cpp \
	coreorbis/orbis-shims/ProsperoVideo.cpp coreorbis/orbis-shims/ProsperoAudio.cpp \
	coreorbis/orbis-shims/ProsperoInput.cpp coreorbis/orbis-shims/ProsperoJailbreak.cpp \
	coreorbis/orbis-shims/ProsperoCrash.cpp coreorbis/orbis-shims/ProsperoCrt.cpp coreorbis/orbis-shims/ProsperoGpu.cpp \
	coreorbis/orbis-shims/ProsperoUpdate.cpp coreorbis/orbis-shims/ProsperoSlangExt.cpp \
	frontend/fe_emu.cpp frontend/fe_menu.cpp frontend/fe_settings.cpp frontend/fe_i18n.cpp frontend/fe_chdcache.cpp frontend/fe_text.cpp \
	frontend/fe_games.cpp frontend/fe_http.cpp frontend/fe_covers.cpp frontend/fe_shelf.cpp \
	frontend/fe_prefetch.cpp frontend/fe_coverfetch.cpp frontend/fe_coverworker.cpp \
	frontend/fe_netscreen.cpp frontend/fe_osk.cpp frontend/fe_ui.cpp $(NETFS_SRC)
# the cheats (.cht files, the cheat manager): fe_cheatcode.cpp is under Genesis Plus GX's licence (libretro.c's decoding)
EMU_SRC += frontend/fe_cheats.cpp frontend/fe_cheatcode.cpp frontend/fe_chtfile.cpp
EMU_SRC += frontend/fe_border.cpp frontend/fe_borderimg.cpp frontend/fe_borderfetch.cpp
# minizip (zlib licence) for games inside a .zip, built as C with the core's zlib
EMU_C_SRC := frontend/third_party/unzip.c frontend/third_party/ioapi.c
# a C bridge to the core's own state (the Sega CD backup RAM), built with the core's defines and headers
BRIDGE_C_SRC := frontend/fe_bram.c
# the payloads: the helper, and the installer (= helper + install)
# (the helper also downloads the covers in the background: fe_coverworker.cpp, with the app's HTTPS code)
HELPER_SRC := coreorbis/orbis-shims/orbis_paths.cpp coreorbis/orbis-shims/ProsperoNotify.cpp \
	coreorbis/orbis-shims/ProsperoHelper.cpp coreorbis/orbis-shims/ProsperoCrash.cpp coreorbis/orbis-shims/ProsperoMigrate.cpp \
	coreorbis/orbis-shims/ProsperoUpdate.cpp \
	frontend/fe_http.cpp frontend/fe_coverfetch.cpp frontend/fe_coverworker.cpp frontend/fe_tlshttp.cpp
# the helper's own HTTPS (fe_tlshttp.cpp): Mbed TLS 3.6 (Apache-2.0), with fe_mbedtls_config.h on its default settings
MBEDTLS := frontend/third_party/mbedtls
MBEDTLS_SRC := $(sort $(wildcard $(MBEDTLS)/library/*.c))
MBEDTLS_FLAGS := -I$(MBEDTLS)/include -Ifrontend '-DMBEDTLS_USER_CONFIG_FILE="fe_mbedtls_config.h"'
INSTALLER_SRC := installer/installer_main.cpp $(HELPER_SRC) coreorbis/orbis-shims/ProsperoInstall.cpp

CORE_INCS := $(addprefix -I$(ROOT)/,$(CORE_DIRS)) -I$(ROOT)/$(CHD)/src -I$(ROOT)/$(CHD)/include \
	-I$(ROOT)/$(CHD)/deps/zstd-1.5.6/lib -I$(ROOT)/$(CHD)/deps/zstd-1.5.6/common -I$(ROOT)/$(CHD)/deps/lzma-24.05/include \
	-I$(ROOT)/$(CHD)/deps/zlib-1.3.1 -I$(ROOT)/libretro -I$(ROOT)/libretro/libretro-common/include
INCS := -Inetfs -Icoreorbis/include-orbis -Icoreorbis/orbis-shims -Ifrontend -isystem $(ROOT)/libretro/libretro-common/include \
	-isystem $(ROOT)/$(CHD)/deps/zlib-1.3.1
DEFS := -DGENPLUS_PS5_VERSION='"$(VERSION)"' -DGENPLUS_CORE_VERSION='"$(CORE_VERSION)"' -DGENPLUS_TITLE_ID='"$(TITLE_ID)"' -DGENPLUS_UPDATE_REPO='"$(UPDATE_REPO)"'
MINIZIP_FLAGS := -DNOUNCRYPT -I$(ROOT)/$(CHD)/deps/zlib-1.3.1 -w
PORT_WARN := -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers

# the game-name table built into fe_games.o (tools/make_gamedb.py), the palettes in fe_config.o
# (2.1: with the dumps No-Intro lacks and the games an online database has art of, tools/make_extra_crc.py, make_db_art.py)
GAMEDB_DEFS := -DGAMEDB_TSV='"$(abspath frontend/data/nointro.tsv)"' -DEXTRACRC_TSV='"$(abspath frontend/data/extra-crc.tsv)"' \
	-DDBART_TSV='"$(abspath frontend/data/db-art.tsv)"'
%/port/frontend/fe_games.o: EXTRA_FLAGS = $(GAMEDB_DEFS)
%/port/frontend/fe_games.o: frontend/data/nointro.tsv frontend/data/extra-crc.tsv frontend/data/db-art.tsv
# stb_image in fe_covers.o has warnings of its own; libretro-thumbnails' box art names are built in (tools/make_cover_index.py)
%/port/frontend/fe_covers.o: EXTRA_FLAGS = -Wno-error -Wno-sign-compare -Wno-unused-but-set-variable -Wno-implicit-fallthrough \
	-DCOVER_INDEX_TXT='"$(abspath frontend/data/cover-index.txt)"'
%/port/frontend/fe_covers.o: frontend/data/cover-index.txt
# the borders (2.1): the built-in frame is embedded in fe_border.o (tools/make_default_border.py), the names of the borders The
# Bezel Project has in fe_borderfetch.o (tools/make_border_index.py; the pictures are downloaded); stb_image in fe_borderimg.o
%/port/frontend/fe_border.o: EXTRA_FLAGS = -DBORDER_DEFAULT_PNG='"$(abspath frontend/assets/borders/default.png)"'
%/port/frontend/fe_border.o: frontend/assets/borders/default.png
%/port/frontend/fe_borderfetch.o: EXTRA_FLAGS = -DBORDER_INDEX_TXT='"$(abspath frontend/data/border-index.txt)"'
%/port/frontend/fe_borderfetch.o: frontend/data/border-index.txt
%/port/frontend/fe_borderimg.o: EXTRA_FLAGS = -Wno-error -Wno-sign-compare -Wno-unused-but-set-variable -Wno-implicit-fallthrough
# the UI fonts (PS5SX2's: PCSX2's Roboto Regular and PromptFont; Font Awesome's brands) built into fe_text.o
%/port/frontend/fe_text.o: EXTRA_FLAGS = -DFE_FONT_DIR='"$(abspath frontend/assets/fonts)"' -Wno-error -Wno-sign-compare -Wno-unused-function
%/port/frontend/fe_text.o: frontend/assets/fonts/Roboto-Regular.ttf frontend/assets/fonts/promptfont.otf \
	frontend/assets/fonts/fa-brands-400.otf
# the helper's HTTPS client: Mbed TLS's headers, and Mozilla's CA list built in
%/port/frontend/fe_tlshttp.o: EXTRA_FLAGS = $(MBEDTLS_FLAGS) -DCACERT_PEM='"$(abspath frontend/data/cacert.pem)"'
%/port/frontend/fe_tlshttp.o: frontend/data/cacert.pem frontend/fe_mbedtls_config.h
# the CHD read-ahead (fe_chdcache.cpp) sees libchdr's header; the link wraps chd_read and chd_close with it
%/port/frontend/fe_chdcache.o: EXTRA_FLAGS = -I$(ROOT)/$(CHD)/include
CHD_WRAP := --wrap=chd_read --wrap=chd_close
# the helper payload is installer_main.cpp without the install step
%/port/installer/helper_main.o: EXTRA_FLAGS = -DGENPLUS_HELPER_ONLY

# glslang (third_party/glslang, 15.1.0, BSD-3-Clause and others: its LICENSE.txt): the user's own .slangp presets
# compiled on the console (ProsperoSlangExt.cpp). Built without HLSL and without SPIRV-Tools.
GLSLANG := third_party/glslang
GLSLANG_SRC := $(wildcard $(GLSLANG)/glslang/MachineIndependent/*.cpp $(GLSLANG)/glslang/MachineIndependent/preprocessor/*.cpp \
	$(GLSLANG)/glslang/GenericCodeGen/*.cpp) $(GLSLANG)/glslang/OSDependent/Unix/ossource.cpp \
	$(GLSLANG)/glslang/ResourceLimits/ResourceLimits.cpp $(addprefix $(GLSLANG)/SPIRV/,GlslangToSpv.cpp SpvBuilder.cpp \
	SpvPostProcess.cpp InReadableOrder.cpp Logger.cpp SpvTools.cpp disassemble.cpp doc.cpp)
GLSLANG_FLAGS := -std=c++17 -I$(GLSLANG) -w
%/port/coreorbis/orbis-shims/ProsperoSlangExt.o: EXTRA_FLAGS = -I$(GLSLANG) $(PS5_GPU_DEFS)

# ---------------- the GPU (optional) ----------------
# PS5_VULKAN=<a PS5_Vulkan checkout> (github.com/mihawk-99/PS5_Vulkan, with tools/setup-native-dependencies.sh and
# tools/build-radv.sh release run in it) builds the app with RADV inside: the GPU draws the picture and runs the
# slang shaders. Without it the app is the CPU-only build of 1.8. Use that checkout's payload SDK fork too
# (PS5_PAYLOAD_SDK=$(PS5_VULKAN)/.deps/native/ps5-payload-sdk): RADV is built against it.
PS5_VULKAN ?=
ifneq ($(PS5_VULKAN),)
RADV_DIR ?= $(PS5_VULKAN)/.deps/native/radv-release
RADV_ARCHIVE ?= $(RADV_DIR)/lib/libvulkan_radeon.ps5.a
PS5_GPU_DEFS := -DGENPLUS_GPU -isystem $(RADV_DIR)/include
endif

# ---------------- PS5 ----------------
PS5_CC  := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
PS5_CXX := $(PS5_PAYLOAD_SDK)/bin/prospero-clang++
PS5_OUT := build/ps5
# The PS5's CPU is Zen 2 (PS5SX2 builds with the same -march).
PS5_OPT := -O3 -fPIC -march=znver2 -fno-strict-aliasing
# the payloads: notifications, and the kernel access the helper needs (libkernel_sys: sceKernelGetAppInfo)
# the payload SDK's network libraries (its http2_get sample's), for the helper's cover downloads
PS5_PAYLOAD_LIBS := -lkernel_sys -lSceNet -lSceNetCtl -lSceSsl -lSceHttp2 -lpthread

PS5_CORE_OBJ := $(patsubst %.cpp,$(PS5_OUT)/core/%.o,$(CORE_SRC)) $(patsubst %.c,$(PS5_OUT)/core/%.o,$(CORE_C_SRC))
PS5_EMU_OBJ := $(patsubst %.cpp,$(PS5_OUT)/port/%.o,$(EMU_SRC)) $(patsubst %.c,$(PS5_OUT)/port/%.o,$(EMU_C_SRC)) \
	$(patsubst %.c,$(PS5_OUT)/bridge/%.o,$(BRIDGE_C_SRC))
PS5_MBEDTLS_OBJ := $(patsubst $(MBEDTLS)/library/%.c,$(PS5_OUT)/mbedtls/%.o,$(MBEDTLS_SRC))
PS5_HELPER_OBJ := $(PS5_OUT)/port/installer/helper_main.o $(patsubst %.cpp,$(PS5_OUT)/port/%.o,$(HELPER_SRC)) $(PS5_MBEDTLS_OBJ)
PS5_INSTALLER_OBJ := $(patsubst %.cpp,$(PS5_OUT)/port/%.o,$(INSTALLER_SRC)) $(PS5_MBEDTLS_OBJ)
PS5_LIBSMB2_OBJ := $(patsubst $(LIBSMB2)/%.c,$(PS5_OUT)/libsmb2/%.o,$(LIBSMB2_SRC))
PS5_LIBNFS_OBJ := $(patsubst $(LIBNFS)/%.c,$(PS5_OUT)/libnfs/%.o,$(LIBNFS_SRC))

ps5: $(PS5_OUT)/GenesisPlusGXPS5.elf
# the core alone
ps5-core: $(PS5_CORE_OBJ)

# the core's own flags (Makefile.libretro), its warnings left to it
$(PS5_OUT)/core/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(PS5_CC) $(PS5_OPT) $(CORE_DEFS) $(CORE_INCS) -w -c -o $@ $<

$(PS5_OUT)/mbedtls/%.o: $(MBEDTLS)/library/%.c frontend/fe_mbedtls_config.h
	@mkdir -p $(dir $@)
	$(PS5_CC) -Os -fPIC -march=znver2 $(MBEDTLS_FLAGS) -c -o $@ $<

$(PS5_OUT)/port/%.o: %.c
	@mkdir -p $(dir $@)
	$(PS5_CC) $(PS5_OPT) $(MINIZIP_FLAGS) -c -o $@ $<

$(PS5_OUT)/bridge/%.o: %.c frontend/fe_bram.h
	@mkdir -p $(dir $@)
	$(PS5_CC) $(PS5_OPT) $(CORE_DEFS) $(CORE_INCS) -Ifrontend -I$(ROOT)/$(CHD)/deps/zlib-1.3.1 -Wall -Werror -c -o $@ $<

$(PS5_OUT)/port/%.o: %.cpp coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h frontend/*.h netfs/*.h
	@mkdir -p $(dir $@)
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

$(PS5_OUT)/libsmb2/%.o: $(LIBSMB2)/%.c
	@mkdir -p $(dir $@)
	$(PS5_CC) -O2 -fPIC -march=znver2 $(LIBSMB2_FLAGS) -w -c -o $@ $<

$(PS5_OUT)/libnfs/%.o: $(LIBNFS)/%.c
	@mkdir -p $(dir $@)
	$(PS5_CC) -O2 -fPIC -march=znver2 $(LIBNFS_FLAGS) -w -c -o $@ $<
$(PS5_OUT)/port/coreorbis/orbis-shims/ProsperoGpu.o: EXTRA_FLAGS = $(PS5_GPU_DEFS)

$(PS5_OUT)/port/installer/helper_main.o: installer/installer_main.cpp coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h
	@mkdir -p $(dir $@)
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

# 1. the helper payload (built into the app, which sends it to the ELF loader when no helper answers)
$(PS5_OUT)/GenesisPlusGXPS5-helper.elf: $(PS5_HELPER_OBJ)
	$(PS5_CXX) -o $@ $^ $(PS5_PAYLOAD_LIBS)

$(PS5_OUT)/helper_data.o: coreorbis/orbis-shims/helper_data.cpp coreorbis/orbis-shims/ProsperoJailbreak.h $(PS5_OUT)/GenesisPlusGXPS5-helper.elf
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PORT_WARN) -DHELPER_ELF='"$(abspath $(PS5_OUT)/GenesisPlusGXPS5-helper.elf)"' -c -o $@ $<

# 3. the installer + helper, with the app folder built in
APP_FILES = $(APP_OUT)/eboot.bin $(APP_OUT)/sce_sys/param.json $(APP_OUT)/sce_sys/icon0.png $(APP_OUT)/sce_sys/pic0.dds \
	$(APP_OUT)/sce_sys/pic1.dds \
	$(APP_OUT)/sce_module/libc.prx
APP_DATA_DEFS = -DAPP_EBOOT='"$(abspath $(APP_OUT)/eboot.bin)"' -DAPP_PARAM='"$(abspath $(APP_OUT)/sce_sys/param.json)"' \
	-DAPP_ICON0='"$(abspath $(APP_OUT)/sce_sys/icon0.png)"' -DAPP_PIC0='"$(abspath $(APP_OUT)/sce_sys/pic0.dds)"' \
	-DAPP_PIC1='"$(abspath $(APP_OUT)/sce_sys/pic1.dds)"' \
	-DAPP_LIBC='"$(abspath $(APP_OUT)/sce_module/libc.prx)"' \
	$(if $(shell cmp -s app/sce_sys/pic0.dds app/sce_sys/pic1.dds && echo same),-DAPP_PIC1_IS_PIC0)

$(PS5_OUT)/install_data.o: coreorbis/orbis-shims/install_data.cpp coreorbis/orbis-shims/ProsperoInstall.h $(APP_FILES)
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PORT_WARN) $(APP_DATA_DEFS) -c -o $@ $<

$(PS5_OUT)/GenesisPlusGXPS5.elf: $(PS5_INSTALLER_OBJ) $(PS5_OUT)/install_data.o
	$(PS5_CXX) -o $@ $^ $(PS5_PAYLOAD_LIBS)
	@echo "built $@ ($$(stat -c %s $@) bytes)"

$(PS5_OUT)/GenesisPlusGXPS5-stripped.elf: $(PS5_OUT)/GenesisPlusGXPS5.elf
	$(PS5_PAYLOAD_SDK)/bin/prospero-strip -o $@ $<

strip: $(PS5_OUT)/GenesisPlusGXPS5-stripped.elf

# the release files, named with the version: build/dist/GenesisPlusGXPS5-v$(VERSION).elf and the sources zip
DIST := build/dist
# + the app folder as a stored zip with its manifest (GenesisPlusGXPS5-v$(VERSION)-$(TITLE_ID).zip): attach it to the
# GitHub release tagged v$(VERSION), so the app can update itself (Settings, SYSTEM, Updates)
dist: $(PS5_OUT)/GenesisPlusGXPS5.elf $(APP_FILES)
	@mkdir -p $(DIST)
	cp $< $(DIST)/GenesisPlusGXPS5-v$(VERSION).elf
	python3 tools/make_release_zip.py $(APP_OUT) $(TITLE_ID) $(DIST)/GenesisPlusGXPS5-v$(VERSION)-$(TITLE_ID).zip
	cd ../.. && rm -f $(abspath $(DIST))/genplus-ps5-v$(VERSION)-src.zip && \
		zip -qr $(abspath $(DIST))/genplus-ps5-v$(VERSION)-src.zip $(notdir $(abspath ..)) \
		-x '*/ps5/build/*' '*/proto/native/build/*' '*.o' '*/.git/*'
	@ls -l $(DIST)

send: $(PS5_OUT)/GenesisPlusGXPS5.elf
	$(PS5_PAYLOAD_SDK)/bin/prospero-deploy -h $(PS5_HOST) -p $(PS5_PORT) $<

# ---------------- the app (eboot.bin = Genesis Plus GX) ----------------
# 2. A native PS5 title linked the way PS5SX2 links its eboot (ps5/coreorbis/link-vk.sh): the boilerplate's
# app_crt.cpp (_start -> _init_env -> main) and app_cpp_runtime.cpp, the emulator's objects (the same ones,
# compiled once), a PIE laid out by ps5-pie.ld (+ PS5SX2's ehframe.ld) against the SDK's system-module stubs
# and its static libc++/libc++abi/libunwind/libc, every defined symbol local (PS5SX2's genmap.py result:
# localize.map); then ps5-native-tool link (SCE dynamic tables, process parameters) and self --sign.
# The folder carries sce_module/libc.prx, the C runtime module a native title needs (without it the console
# stops the app with PRX_SCE_MODULE_LOAD_ERROR, 0xa0020102).
NATIVE := proto/native
NATIVE_TOOL := $(NATIVE)/build/host/ps5-native-tool
LIBC_BUILDER := $(NATIVE)/build/host/libc-builder
LLD ?= ld.lld
NATIVE_CLANG = PS5_PAYLOAD_SDK=$(PS5_PAYLOAD_SDK) sh $(NATIVE)/tooling/prospero-clang18
SDK_LIB := $(PS5_PAYLOAD_SDK)/target/lib
# every system-module stub but the two PS5SX2 leaves out (link-vk.sh)
APP_STUBS = $(filter-out %/libScePosixForWebKit.so %/libSceGLSlimVSH.so,$(sort $(wildcard $(SDK_LIB)/*.so)))

$(NATIVE_TOOL): $(NATIVE)/tooling/native/native_app_builder.cpp $(NATIVE)/tooling/native/elf_object.cpp \
		$(NATIVE)/tooling/native/self_container.cpp $(NATIVE)/tooling/native/sce_module_writer.cpp
	@mkdir -p $(dir $@)
	$(HOST_CXX_NATIVE) -std=c++20 -O2 -o $@ $^ -lz
HOST_CXX_NATIVE ?= clang++

$(LIBC_BUILDER): $(NATIVE)/tooling/native/libc_builder.cpp
	@mkdir -p $(dir $@)
	$(HOST_CXX_NATIVE) -std=c++20 -O2 -Wall -Wextra -Werror -o $@ $<

# sce_module/libc.prx, deterministic: it must match the published digest (proto/native/libc.prx.sha256)
build/app/libc.prx: $(LIBC_BUILDER) $(NATIVE_TOOL) $(wildcard $(NATIVE)/tooling/native/runtime/*.txt)
	@mkdir -p $(dir $@)
	$(LIBC_BUILDER) $(NATIVE)/tooling/native/runtime/api-surface.txt $(NATIVE)/tooling/native/runtime/imports.txt build/app/libc.raw.elf
	$(NATIVE_TOOL) self --sign --in build/app/libc.raw.elf --out $@
	cd build/app && sha256sum --check --strict $(abspath $(NATIVE)/libc.prx.sha256)

$(APP_OUT)/sce_module/libc.prx: build/app/libc.prx
	@mkdir -p $(dir $@)
	cp $< $@

app: $(APP_FILES)

build/app/app_crt.o build/app/app_cpp_runtime.o: build/app/%.o: $(NATIVE)/tooling/native/%.cpp
	@mkdir -p $(dir $@)
	$(NATIVE_CLANG) -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
		-c -o $@ $<

EBOOT_OBJS = build/app/app_crt.o build/app/app_cpp_runtime.o $(PS5_CORE_OBJ) $(PS5_EMU_OBJ) $(PS5_LIBSMB2_OBJ) $(PS5_LIBNFS_OBJ) \
	$(PS5_OUT)/helper_data.o
ifeq ($(PS5_VULKAN),)
build/app/genplus-pie.elf: $(EBOOT_OBJS) $(NATIVE)/tooling/native/ps5-pie.ld $(NATIVE)/tooling/native/ehframe.ld \
		$(NATIVE)/tooling/native/app-symbols.map $(NATIVE)/tooling/native/localize.map
	$(LLD) -m elf_x86_64 -pie -z max-page-size=0x4000 --hash-style=gnu \
		-T $(NATIVE)/tooling/native/ps5-pie.ld -T $(NATIVE)/tooling/native/ehframe.ld --eh-frame-hdr \
		--version-script $(NATIVE)/tooling/native/app-symbols.map --version-script $(NATIVE)/tooling/native/localize.map \
		-e _start --no-dynamic-linker $(CHD_WRAP) $(NETFS_WRAP_LLD) -o $@ $(EBOOT_OBJS) \
		--as-needed $(APP_STUBS) -L $(SDK_LIB) \
		$(SDK_LIB)/libc++.a $(SDK_LIB)/libc++abi.a $(SDK_LIB)/libunwind.a $(SDK_LIB)/libc.a -lpthread
EBOOT_EXTRA_STUBS =
else
# With RADV: PS5_Vulkan's recipe (tools/radv_link_args.sh), its AGC import stubs, and the shader tables. RADV's
# archive carries zlib 1.3.1 (its shader cache's), the same release as libchdr's copy: that copy is left out, and
# so is libretro's scrc32.c (its crc32 is zlib's, which RADV's archive defines).
EBOOT_OBJS := $(filter-out $(PS5_OUT)/core/$(CHD)/deps/zlib-1.3.1/% $(PS5_OUT)/core/libretro/scrc32.o,$(EBOOT_OBJS)) \
	$(PS5_OUT)/slang_presets.o $(patsubst %.cpp,$(PS5_OUT)/glslang/%.o,$(GLSLANG_SRC))
$(PS5_OUT)/glslang/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(PS5_CXX) $(GLSLANG_FLAGS) -O2 -fPIC -march=znver2 -c -o $@ $<
AGC_STUBS := build/app/stubs/libSceAgc.so build/app/stubs/libSceAgcDriver.so
build/app/stubs/libSceAgc.so: $(PS5_VULKAN)/vendor/ps5/sdk/stubs/agc_canary_link_stub.c
build/app/stubs/libSceAgcDriver.so: $(PS5_VULKAN)/vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c
$(AGC_STUBS):
	@mkdir -p $(dir $@)
	$(PS5_CC) -std=c11 -O2 -fPIC -c -o $@.o $<
	$(PS5_PAYLOAD_SDK)/bin/prospero-lld --shared -soname $(basename $(notdir $@)).prx -o $@ $@.o
build/app/radv-link.args: tools/radv_link_args.sh $(RADV_ARCHIVE)
	@mkdir -p $(dir $@)
	bash tools/radv_link_args.sh $(abspath $(PS5_VULKAN)) $(abspath $(PS5_PAYLOAD_SDK)) $(abspath $(RADV_ARCHIVE)) > $@
$(PS5_OUT)/slang_presets.o: $(SLANG_CPP) coreorbis/orbis-shims/ProsperoSlang.h
	@mkdir -p $(dir $@)
	$(PS5_CXX) -std=c++17 -O0 -fPIC -Icoreorbis/orbis-shims -c -o $@ $<
build/app/genplus-pie.elf: $(EBOOT_OBJS) $(AGC_STUBS) build/app/radv-link.args $(NATIVE)/tooling/native/app-symbols.map \
		$(NATIVE)/tooling/native/localize.map
	$(PS5_PAYLOAD_SDK)/bin/prospero-lld --eh-frame-hdr --no-dynamic-linker -z nodynamic-undefined-weak --version-script $(NATIVE)/tooling/native/app-symbols.map \
		--version-script $(NATIVE)/tooling/native/localize.map --exclude-libs=ALL $(CHD_WRAP) $(NETFS_WRAP_LLD) \
		-e _start -o $@ $(EBOOT_OBJS) $(AGC_STUBS) $$(cat build/app/radv-link.args) \
		--as-needed $(APP_STUBS)
EBOOT_EXTRA_STUBS = $(foreach s,$(AGC_STUBS),--stub $(s))
endif

build/app/eboot.elf: build/app/genplus-pie.elf $(NATIVE_TOOL)
	$(NATIVE_TOOL) link --in $< --out $@ --stub-dir $(SDK_LIB) $(EBOOT_EXTRA_STUBS) --module-sdk 0x02000009 \
		--companion-sdk 0x08050001 --file-name eboot.elf
	python3 tools/prepare_console_elf.py --input $@ --output build/app/eboot-console-ready.elf

$(APP_OUT)/eboot.bin: build/app/eboot.elf $(NATIVE_TOOL)
	@mkdir -p $(dir $@)
	$(NATIVE_TOOL) self --sign --in $< --out $@ --magic 0x1D3D154F
	$(NATIVE_TOOL) self --inspect --file $@

$(APP_OUT)/sce_sys/param.json: app/sce_sys/param.json
	@mkdir -p $(dir $@)
	sed 's/PPSA99921/$(TITLE_ID)/g' $< > $@

$(APP_OUT)/sce_sys/%.png: app/sce_sys/%.png
	@mkdir -p $(dir $@)
	cp $< $@

# pic0.dds (the home screen's background while the icon is selected) and pic1.dds (the launch background):
# 3840x2160 BC7_UNORM DX10 DDS without mipmaps, made from app/sce_sys/background-source.png with the
# ps5-native-app-boilerplate's tools/prepare-assets.sh --background (pinned bc7enc_rdo).
$(APP_OUT)/sce_sys/%.dds: app/sce_sys/%.dds
	@mkdir -p $(dir $@)
	cp $< $@

# ---------------- host ----------------
HOST_CC ?= gcc
HOST_CXX ?= g++
HOST_OUT := build/host
HOST_SAN ?= -fsanitize=address,undefined -fno-omit-frame-pointer
# The core under AddressSanitizer only (UBSan would report its deliberate shifts and unaligned reads).
HOST_CORE_SAN ?= -fsanitize=address -fno-omit-frame-pointer
# no _FORTIFY_SOURCE: it turns fread/fgets/read into __fread_chk/..., which --wrap (netfs) does not see
HOST_OPT := -O1 -g -mavx2 -fno-strict-aliasing -U_FORTIFY_SOURCE
HOST_CORE_OPT := -O2 -g1 -mavx2 -U_FORTIFY_SOURCE

HOST_CORE_OBJ := $(patsubst %.cpp,$(HOST_OUT)/core/%.o,$(CORE_SRC)) $(patsubst %.c,$(HOST_OUT)/core/%.o,$(CORE_C_SRC))
HOST_EMU_OBJ := $(patsubst %.cpp,$(HOST_OUT)/port/%.o,$(EMU_SRC) host/sce_host.cpp host/netfs_dirbackend.cpp) $(patsubst %.c,$(HOST_OUT)/port/%.o,$(EMU_C_SRC)) \
	$(patsubst %.c,$(HOST_OUT)/bridge/%.o,$(BRIDGE_C_SRC))
HOST_MBEDTLS_OBJ := $(patsubst $(MBEDTLS)/library/%.c,$(HOST_OUT)/mbedtls/%.o,$(MBEDTLS_SRC))
HOST_HELPER_OBJ := $(HOST_OUT)/port/installer/helper_main.o $(patsubst %.cpp,$(HOST_OUT)/port/%.o,$(HELPER_SRC) host/sce_host.cpp) \
	$(HOST_MBEDTLS_OBJ)
HOST_INSTALLER_OBJ := $(patsubst %.cpp,$(HOST_OUT)/port/%.o,$(INSTALLER_SRC) host/sce_host.cpp) $(HOST_MBEDTLS_OBJ)
HOST_LIBSMB2_OBJ := $(patsubst $(LIBSMB2)/%.c,$(HOST_OUT)/libsmb2/%.o,$(LIBSMB2_SRC))
HOST_LIBNFS_OBJ := $(patsubst $(LIBNFS)/%.c,$(HOST_OUT)/libnfs/%.o,$(LIBNFS_SRC))

host-core: $(HOST_CORE_OBJ)
host: $(HOST_OUT)/genplus-ps5-host $(HOST_OUT)/genplus-ps5-installer $(HOST_OUT)/genplus-ps5-helper

$(HOST_OUT)/core/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -fPIC $(HOST_CORE_OPT) $(HOST_CORE_SAN) $(CORE_DEFS) $(CORE_INCS) -w -c -o $@ $<

$(HOST_OUT)/mbedtls/%.o: $(MBEDTLS)/library/%.c frontend/fe_mbedtls_config.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -O2 -g1 $(MBEDTLS_FLAGS) -c -o $@ $<

$(HOST_OUT)/port/%.o: %.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_OPT) $(MINIZIP_FLAGS) -c -o $@ $<

$(HOST_OUT)/bridge/%.o: %.c frontend/fe_bram.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -fPIC $(HOST_CORE_OPT) $(HOST_CORE_SAN) $(CORE_DEFS) $(CORE_INCS) -Ifrontend -I$(ROOT)/$(CHD)/deps/zlib-1.3.1 -Wall -Werror -c -o $@ $<

$(HOST_OUT)/port/%.o: %.cpp coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h frontend/*.h netfs/*.h
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(DEFS) $(INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

$(HOST_OUT)/libsmb2/%.o: $(LIBSMB2)/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -O2 -g1 -D_GNU_SOURCE $(LIBSMB2_FLAGS) -w -c -o $@ $<

$(HOST_OUT)/libnfs/%.o: $(LIBNFS)/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -O2 -g1 $(LIBNFS_FLAGS) -w -c -o $@ $<

$(HOST_OUT)/port/installer/helper_main.o: installer/installer_main.cpp coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(DEFS) $(INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

# The host installer carries a stand-in app folder (tests/fake-eboot.bin, tests/fake-libc.prx + the real
# param/icon/pic); the host app carries the host helper, which it hands to a fake ELF loader in the tests.
HOST_APP_DEFS = -DAPP_EBOOT='"$(abspath tests/fake-eboot.bin)"' -DAPP_PARAM='"$(abspath app/sce_sys/param.json)"' \
	-DAPP_ICON0='"$(abspath app/sce_sys/icon0.png)"' -DAPP_PIC0='"$(abspath app/sce_sys/pic0.dds)"' \
	-DAPP_PIC1='"$(abspath app/sce_sys/pic1.dds)"' \
	-DAPP_LIBC='"$(abspath tests/fake-libc.prx)"'

$(HOST_OUT)/install_data.o: coreorbis/orbis-shims/install_data.cpp coreorbis/orbis-shims/ProsperoInstall.h \
		tests/fake-eboot.bin tests/fake-libc.prx app/sce_sys/param.json app/sce_sys/icon0.png app/sce_sys/pic0.dds app/sce_sys/pic1.dds
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(DEFS) $(INCS) $(PORT_WARN) $(HOST_APP_DEFS) -c -o $@ $<

$(HOST_OUT)/helper_data.o: coreorbis/orbis-shims/helper_data.cpp coreorbis/orbis-shims/ProsperoJailbreak.h $(HOST_OUT)/genplus-ps5-helper
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(DEFS) $(INCS) $(PORT_WARN) -DHELPER_ELF='"$(abspath $(HOST_OUT)/genplus-ps5-helper)"' -c -o $@ $<

$(HOST_OUT)/genplus-ps5-helper: $(HOST_HELPER_OBJ)
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ -lpthread

$(HOST_OUT)/genplus-ps5-installer: $(HOST_INSTALLER_OBJ) $(HOST_OUT)/install_data.o
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ -lpthread

# the shader tables: data only, no sanitizer, no optimisation (a large file)
$(HOST_OUT)/slang_presets.o: $(SLANG_CPP) coreorbis/orbis-shims/ProsperoSlang.h
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 -O0 -Icoreorbis/orbis-shims -c -o $@ $<

HOST_GLSLANG_OBJ := $(patsubst %.cpp,$(HOST_OUT)/glslang/%.o,$(GLSLANG_SRC))
$(HOST_OUT)/glslang/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(GLSLANG_FLAGS) -O2 -g1 -c -o $@ $<

$(HOST_OUT)/genplus-ps5-host: $(HOST_CORE_OBJ) $(HOST_EMU_OBJ) $(HOST_LIBSMB2_OBJ) $(HOST_LIBNFS_OBJ) $(HOST_OUT)/helper_data.o \
		$(HOST_OUT)/slang_presets.o $(HOST_GLSLANG_OBJ)
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ -Wl,--wrap=chd_read -Wl,--wrap=chd_close $(NETFS_WRAP_GCC) -lpthread -lm -lvulkan

# netfs's own test: the netfs code, the stub backends and an in-memory share, linked with the same --wrap list
HOST_NETFS_TEST_OBJ := $(patsubst %.cpp,$(HOST_OUT)/port/%.o,$(NETFS_CORE_SRC) netfs/backend_smb_stub.cpp netfs/backend_nfs_stub.cpp \
	tests/test_netfs.cpp)
# minizip and a whole zlib (the core's copy, libchdr's zlib 1.3.1, gz files included) go into it too: the test opens a
# .zip and a .gz on the fake share
TEST_ZLIB := $(ROOT)/$(CHD)/deps/zlib-1.3.1
HOST_TEST_ZLIB_OBJ := $(patsubst %,$(HOST_OUT)/testzlib/%.o,adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback \
	inffast inflate inftrees trees uncompr zutil)
$(HOST_OUT)/testzlib/%.o: $(TEST_ZLIB)/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -O2 -g1 -U_FORTIFY_SOURCE -DHAVE_UNISTD_H -I$(TEST_ZLIB) -w -c -o $@ $<
%/port/tests/test_netfs.o: EXTRA_FLAGS = -Inetfs -Ifrontend/third_party -I$(TEST_ZLIB)
HOST_NETFS_TEST_EXTRA := $(HOST_OUT)/port/frontend/third_party/unzip.o $(HOST_OUT)/port/frontend/third_party/ioapi.o $(HOST_TEST_ZLIB_OBJ)
$(HOST_OUT)/test-netfs: $(HOST_NETFS_TEST_OBJ) $(HOST_NETFS_TEST_EXTRA) tests/netfs_fake.h
	$(HOST_CXX) $(HOST_SAN) -o $@ $(HOST_NETFS_TEST_OBJ) $(HOST_NETFS_TEST_EXTRA) $(NETFS_WRAP_GCC) -lpthread

test: host $(HOST_OUT)/test-netfs
	bash tests/run-tests.sh

# NFS backend unit test: backend_nfs.cpp against an in-memory libnfs (tests/libnfs_mock.cpp), no network
NFS_TEST := $(HOST_OUT)/nfs-backend-test
$(NFS_TEST): tests/nfs_backend_test.cpp tests/libnfs_mock.cpp tests/libnfs_mock.h netfs/backend_nfs.cpp netfs/backend_nfs.h netfs/netfs_backend.h
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(PORT_WARN) $(LIBNFS_FLAGS) -Inetfs -Itests -o $@ \
		tests/nfs_backend_test.cpp tests/libnfs_mock.cpp netfs/backend_nfs.cpp -lpthread

# the same backend on real libnfs (the objects of the host build), against loopback sockets that misbehave
NFS_LOOP_TEST := $(HOST_OUT)/nfs-loopback-test
$(NFS_LOOP_TEST): tests/nfs_loopback_test.cpp netfs/backend_nfs.cpp netfs/backend_nfs.h netfs/netfs_backend.h $(HOST_LIBNFS_OBJ)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(PORT_WARN) $(LIBNFS_FLAGS) -Inetfs -o $@ \
		tests/nfs_loopback_test.cpp netfs/backend_nfs.cpp $(HOST_LIBNFS_OBJ) -lpthread

# the same backend against a real NFSv3 server (unfs3): not part of `make test`, needs rpcbind and unfsd
NFS_SERVER_TEST := $(HOST_OUT)/nfs-server-test
$(NFS_SERVER_TEST): tests/nfs_server_test.cpp netfs/backend_nfs.cpp netfs/backend_nfs.h netfs/netfs_backend.h $(HOST_LIBNFS_OBJ)
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(PORT_WARN) $(LIBNFS_FLAGS) -Inetfs -o $@ \
		tests/nfs_server_test.cpp netfs/backend_nfs.cpp $(HOST_LIBNFS_OBJ) -lpthread

test-nfs-server: $(NFS_SERVER_TEST)
	bash tests/nfs_server_test.sh $(NFS_SERVER_TEST)

test-nfs: $(NFS_TEST) $(NFS_LOOP_TEST)
	ASAN_OPTIONS=detect_leaks=1 $(NFS_TEST)
	ASAN_OPTIONS=detect_leaks=1 $(NFS_LOOP_TEST)

# the shelf's storage tabs (frontend/fe_tabs.h): header only, so no app code is linked
TABS_TEST := $(HOST_OUT)/tabs-test
$(TABS_TEST): tests/test_tabs.cpp frontend/fe_tabs.h frontend/fe_games.h
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(PORT_WARN) -Ifrontend -o $@ tests/test_tabs.cpp

test-tabs: $(TABS_TEST)
	$(TABS_TEST)

test: test-nfs test-tabs

test: unit-tests

# 2.1's unit tests (run by run-tests.sh, groups 60-63), ported from Snes9x PS5 2.7:
# the cover and border matcher over the built-in tables (headers only)
TITLEMATCH_TEST := $(HOST_OUT)/titlematch-test
$(TITLEMATCH_TEST): tests/test_titlematch.cpp frontend/fe_titlematch.h frontend/fe_artmatch.h frontend/fe_dbart.h frontend/fe_romheader.h
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 -O2 -g $(HOST_SAN) $(PORT_WARN) -Ifrontend -o $@ tests/test_titlematch.cpp
# the border pack code (fe_border.cpp, fe_borderimg.cpp, fe_borderfetch.cpp) with the renderer, the cover matcher and the game
# naming stood in for by the test: generated PNGs through the lookup order, the window, the backdrop and the rim
BORDER_TEST := $(HOST_OUT)/border-pack-test
BORDER_TEST_OBJ := $(patsubst %.cpp,$(HOST_OUT)/port/%.o,tests/test_border_pack.cpp frontend/fe_border.cpp frontend/fe_borderimg.cpp \
	frontend/fe_borderfetch.cpp frontend/fe_coverfetch.cpp frontend/fe_settings.cpp coreorbis/orbis-shims/orbis_paths.cpp)
$(BORDER_TEST): $(BORDER_TEST_OBJ)
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ -lpthread -lz
# the border in the renderer (ProsperoVideo.cpp) on the CPU and for the GPU (stood in for), against the host's video output
BORDER_RENDER_TEST := $(HOST_OUT)/border-render-test
BORDER_RENDER_OBJ := $(patsubst %.cpp,$(HOST_OUT)/port/%.o,tests/test_border_render.cpp coreorbis/orbis-shims/ProsperoVideo.cpp \
	coreorbis/orbis-shims/ProsperoCrt.cpp coreorbis/orbis-shims/ProsperoCrash.cpp coreorbis/orbis-shims/orbis_paths.cpp host/sce_host.cpp)
$(BORDER_RENDER_TEST): $(BORDER_RENDER_OBJ)
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ -lpthread
# the helper's download loop with its covers and borders lanes (fe_coverworker.cpp), against a fake HTTP client
DOWNLOAD_WORKER_TEST := $(HOST_OUT)/download-worker-test
DOWNLOAD_WORKER_OBJ := $(patsubst %.cpp,$(HOST_OUT)/port/%.o,tests/test_download_worker.cpp frontend/fe_coverworker.cpp frontend/fe_coverfetch.cpp \
	frontend/fe_http.cpp coreorbis/orbis-shims/orbis_paths.cpp coreorbis/orbis-shims/ProsperoCrash.cpp host/sce_host.cpp)
$(DOWNLOAD_WORKER_TEST): $(DOWNLOAD_WORKER_OBJ)
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ -lpthread
unit-tests: $(TITLEMATCH_TEST) $(BORDER_TEST) $(BORDER_RENDER_TEST) $(DOWNLOAD_WORKER_TEST)

# The GPU's shaders: shaders/presets.txt (RetroArch slang presets, in shaders/slang/) compiled to SPIR-V tables
# with their reflection and textures (tools/slang_compile.py, which needs glslangValidator).
$(SLANG_CPP): shaders/presets.txt tools/slang_compile.py shaders/plain.slang shaders/ui.slang
	@mkdir -p $(dir $@)
	python3 tools/slang_compile.py $@ --list shaders/presets.txt
shaders: $(SLANG_CPP)

# the CRT shaders' drawing time on this computer, built with the PS5's flags (tools/shader_bench.cpp)
shader-bench: $(HOST_OUT)/shader-bench
	$(HOST_OUT)/shader-bench 300
	$(HOST_OUT)/shader-bench 300 720

$(HOST_OUT)/shader-bench: tools/shader_bench.cpp coreorbis/orbis-shims/ProsperoCrt.cpp coreorbis/orbis-shims/ProsperoCrt.h
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 -O3 -march=znver2 -fno-strict-aliasing $(INCS) -o $@ tools/shader_bench.cpp \
		coreorbis/orbis-shims/ProsperoCrt.cpp -lpthread

# the cheats' unit test: the frontend's code check (fe_cheatcode.cpp) and .cht files (fe_chtfile.cpp) against
# libretro.c's own cheat code, copied out of it as it is (tests/extract_libretro_cheats.py) and run on fake ROM and RAM
CHEATS_TEST := $(HOST_OUT)/cheats-test
$(HOST_OUT)/gen/libretro_cheats.inc: $(ROOT)/libretro/libretro.c tests/extract_libretro_cheats.py
	@mkdir -p $(dir $@)
	python3 tests/extract_libretro_cheats.py $< $@
$(HOST_OUT)/gen/libretro_cheats_shim.o: tests/libretro_cheats_shim.c $(HOST_OUT)/gen/libretro_cheats.inc
	$(HOST_CC) -std=gnu99 $(HOST_CORE_OPT) $(HOST_CORE_SAN) -DLSB_FIRST -I$(HOST_OUT)/gen -w -c -o $@ $<
$(CHEATS_TEST): tests/test_cheats.cpp frontend/fe_cheatcode.cpp frontend/fe_cheatcode.h frontend/fe_chtfile.cpp \
		frontend/fe_chtfile.h $(HOST_OUT)/gen/libretro_cheats_shim.o
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(PORT_WARN) -Ifrontend -o $@ tests/test_cheats.cpp frontend/fe_cheatcode.cpp \
		frontend/fe_chtfile.cpp $(HOST_OUT)/gen/libretro_cheats_shim.o
test-cheats: $(CHEATS_TEST)
	$(CHEATS_TEST)
host: $(CHEATS_TEST)
.PHONY: test-cheats

clean:
	rm -rf build

# the SMB backend against a real Samba (adds `smb-test`, run before the other tests; skipped without smbd)
include tests/smb/smb.mk

.PHONY: shaders shader-bench ps5 ps5-core host-core host test test-nfs test-nfs-server test-tabs unit-tests send strip app dist clean
.SECONDARY:
