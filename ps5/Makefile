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
VERSION ?= 1.5
CORE_VERSION := 1.7.4
TITLE_ID ?= PPSA99011
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
CORE_DEFS := -DLSB_FIRST -DBYTE_ORDER=LITTLE_ENDIAN -DHAVE_ZLIB -DMAXROMSIZE=33554432 -DUSE_LIBTREMOR -DUSE_LIBCHDR \
	-DHAVE_LROUND -DHAVE_STDINT_H -DZ7_ST -DZSTD_DISABLE_ASM -DHAVE_SYS_PARAM_H -DUSE_16BPP_RENDERING \
	-DFRONTEND_SUPPORTS_RGB565 -D__LIBRETRO__ -DUSE_LIBRETRO_VFS -DM68K_OVERCLOCK_SHIFT=20 -DZ80_OVERCLOCK_SHIFT=20 \
	-DHAVE_YM3438_CORE -DHAVE_OPLL_CORE '-DINLINE=static inline'

# the app (eboot.bin): the PS5 layer and the frontend
EMU_SRC := coreorbis/main-boot.cpp \
	coreorbis/orbis-shims/orbis_paths.cpp coreorbis/orbis-shims/ProsperoNotify.cpp \
	coreorbis/orbis-shims/ProsperoVideo.cpp coreorbis/orbis-shims/ProsperoAudio.cpp \
	coreorbis/orbis-shims/ProsperoInput.cpp coreorbis/orbis-shims/ProsperoJailbreak.cpp \
	coreorbis/orbis-shims/ProsperoCrash.cpp coreorbis/orbis-shims/ProsperoCrt.cpp \
	frontend/fe_emu.cpp frontend/fe_menu.cpp frontend/fe_settings.cpp frontend/fe_text.cpp \
	frontend/fe_games.cpp frontend/fe_http.cpp frontend/fe_covers.cpp frontend/fe_shelf.cpp \
	frontend/fe_prefetch.cpp frontend/fe_coverfetch.cpp frontend/fe_coverworker.cpp
# minizip (zlib licence) for games inside a .zip, built as C with the core's zlib
EMU_C_SRC := frontend/third_party/unzip.c frontend/third_party/ioapi.c
# a C bridge to the core's own state (the Sega CD backup RAM), built with the core's defines and headers
BRIDGE_C_SRC := frontend/fe_bram.c
# the payloads: the helper, and the installer (= helper + install)
# (the helper also downloads the covers in the background: fe_coverworker.cpp, with the app's HTTPS code)
HELPER_SRC := coreorbis/orbis-shims/orbis_paths.cpp coreorbis/orbis-shims/ProsperoNotify.cpp \
	coreorbis/orbis-shims/ProsperoHelper.cpp coreorbis/orbis-shims/ProsperoCrash.cpp \
	frontend/fe_http.cpp frontend/fe_coverfetch.cpp frontend/fe_coverworker.cpp
INSTALLER_SRC := installer/installer_main.cpp $(HELPER_SRC) coreorbis/orbis-shims/ProsperoInstall.cpp

CORE_INCS := $(addprefix -I$(ROOT)/,$(CORE_DIRS)) -I$(ROOT)/$(CHD)/src -I$(ROOT)/$(CHD)/include \
	-I$(ROOT)/$(CHD)/deps/zstd-1.5.6/lib -I$(ROOT)/$(CHD)/deps/zstd-1.5.6/common -I$(ROOT)/$(CHD)/deps/lzma-24.05/include \
	-I$(ROOT)/$(CHD)/deps/zlib-1.3.1 -I$(ROOT)/libretro -I$(ROOT)/libretro/libretro-common/include
INCS := -Icoreorbis/include-orbis -Icoreorbis/orbis-shims -Ifrontend -isystem $(ROOT)/libretro/libretro-common/include \
	-isystem $(ROOT)/$(CHD)/deps/zlib-1.3.1
DEFS := -DGENPLUS_PS5_VERSION='"$(VERSION)"' -DGENPLUS_CORE_VERSION='"$(CORE_VERSION)"' -DGENPLUS_TITLE_ID='"$(TITLE_ID)"'
MINIZIP_FLAGS := -DNOUNCRYPT -I$(ROOT)/$(CHD)/deps/zlib-1.3.1 -w
PORT_WARN := -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers

# the game-name table built into fe_games.o (tools/make_gamedb.py), the palettes in fe_config.o
GAMEDB_DEFS := -DGAMEDB_TSV='"$(abspath frontend/data/nointro.tsv)"'
%/port/frontend/fe_games.o: EXTRA_FLAGS = $(GAMEDB_DEFS)
%/port/frontend/fe_games.o: frontend/data/nointro.tsv
# stb_image in fe_covers.o has warnings of its own
%/port/frontend/fe_covers.o: EXTRA_FLAGS = -Wno-error -Wno-sign-compare -Wno-unused-but-set-variable -Wno-implicit-fallthrough
# the UI fonts (PS5SX2's: PCSX2's Roboto Regular and PromptFont; Font Awesome's brands) built into fe_text.o
%/port/frontend/fe_text.o: EXTRA_FLAGS = -DFE_FONT_DIR='"$(abspath frontend/assets/fonts)"' -Wno-error -Wno-sign-compare -Wno-unused-function
%/port/frontend/fe_text.o: frontend/assets/fonts/Roboto-Regular.ttf frontend/assets/fonts/promptfont.otf \
	frontend/assets/fonts/fa-brands-400.otf
# the helper payload is installer_main.cpp without the install step
%/port/installer/helper_main.o: EXTRA_FLAGS = -DGENPLUS_HELPER_ONLY

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
PS5_HELPER_OBJ := $(PS5_OUT)/port/installer/helper_main.o $(patsubst %.cpp,$(PS5_OUT)/port/%.o,$(HELPER_SRC))
PS5_INSTALLER_OBJ := $(patsubst %.cpp,$(PS5_OUT)/port/%.o,$(INSTALLER_SRC))

ps5: $(PS5_OUT)/GenesisPlusGXPS5.elf
# the core alone
ps5-core: $(PS5_CORE_OBJ)

# the core's own flags (Makefile.libretro), its warnings left to it
$(PS5_OUT)/core/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(PS5_CC) $(PS5_OPT) $(CORE_DEFS) $(CORE_INCS) -w -c -o $@ $<

$(PS5_OUT)/port/%.o: %.c
	@mkdir -p $(dir $@)
	$(PS5_CC) $(PS5_OPT) $(MINIZIP_FLAGS) -c -o $@ $<

$(PS5_OUT)/bridge/%.o: %.c frontend/fe_bram.h
	@mkdir -p $(dir $@)
	$(PS5_CC) $(PS5_OPT) $(CORE_DEFS) $(CORE_INCS) -Ifrontend -I$(ROOT)/$(CHD)/deps/zlib-1.3.1 -Wall -Werror -c -o $@ $<

$(PS5_OUT)/port/%.o: %.cpp coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h frontend/*.h
	@mkdir -p $(dir $@)
	$(PS5_CXX) -std=c++17 $(PS5_OPT) $(DEFS) $(INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

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
	-DAPP_LIBC='"$(abspath $(APP_OUT)/sce_module/libc.prx)"'

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
dist: $(PS5_OUT)/GenesisPlusGXPS5.elf
	@mkdir -p $(DIST)
	cp $< $(DIST)/GenesisPlusGXPS5-v$(VERSION).elf
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

EBOOT_OBJS = build/app/app_crt.o build/app/app_cpp_runtime.o $(PS5_CORE_OBJ) $(PS5_EMU_OBJ) $(PS5_OUT)/helper_data.o
build/app/genplus-pie.elf: $(EBOOT_OBJS) $(NATIVE)/tooling/native/ps5-pie.ld $(NATIVE)/tooling/native/ehframe.ld \
		$(NATIVE)/tooling/native/app-symbols.map $(NATIVE)/tooling/native/localize.map
	$(LLD) -m elf_x86_64 -pie -z max-page-size=0x4000 --hash-style=gnu \
		-T $(NATIVE)/tooling/native/ps5-pie.ld -T $(NATIVE)/tooling/native/ehframe.ld --eh-frame-hdr \
		--version-script $(NATIVE)/tooling/native/app-symbols.map --version-script $(NATIVE)/tooling/native/localize.map \
		-e _start --no-dynamic-linker -o $@ $(EBOOT_OBJS) \
		--as-needed $(APP_STUBS) -L $(SDK_LIB) \
		$(SDK_LIB)/libc++.a $(SDK_LIB)/libc++abi.a $(SDK_LIB)/libunwind.a $(SDK_LIB)/libc.a -lpthread

build/app/eboot.elf: build/app/genplus-pie.elf $(NATIVE_TOOL)
	$(NATIVE_TOOL) link --in $< --out $@ --stub-dir $(SDK_LIB) --module-sdk 0x02000009 \
		--companion-sdk 0x08050001 --file-name eboot.elf
	python3 tools/prepare_console_elf.py --input $@ --output build/app/eboot-console-ready.elf

$(APP_OUT)/eboot.bin: build/app/eboot.elf $(NATIVE_TOOL)
	@mkdir -p $(dir $@)
	$(NATIVE_TOOL) self --sign --in $< --out $@ --magic 0x1D3D154F
	$(NATIVE_TOOL) self --inspect --file $@

$(APP_OUT)/sce_sys/param.json: app/sce_sys/param.json
	@mkdir -p $(dir $@)
	sed 's/PPSA99011/$(TITLE_ID)/g' $< > $@

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
HOST_OPT := -O1 -g -mavx2 -fno-strict-aliasing
HOST_CORE_OPT := -O2 -g1 -mavx2

HOST_CORE_OBJ := $(patsubst %.cpp,$(HOST_OUT)/core/%.o,$(CORE_SRC)) $(patsubst %.c,$(HOST_OUT)/core/%.o,$(CORE_C_SRC))
HOST_EMU_OBJ := $(patsubst %.cpp,$(HOST_OUT)/port/%.o,$(EMU_SRC) host/sce_host.cpp) $(patsubst %.c,$(HOST_OUT)/port/%.o,$(EMU_C_SRC)) \
	$(patsubst %.c,$(HOST_OUT)/bridge/%.o,$(BRIDGE_C_SRC))
HOST_HELPER_OBJ := $(HOST_OUT)/port/installer/helper_main.o $(patsubst %.cpp,$(HOST_OUT)/port/%.o,$(HELPER_SRC) host/sce_host.cpp)
HOST_INSTALLER_OBJ := $(patsubst %.cpp,$(HOST_OUT)/port/%.o,$(INSTALLER_SRC) host/sce_host.cpp)

host-core: $(HOST_CORE_OBJ)
host: $(HOST_OUT)/genplus-ps5-host $(HOST_OUT)/genplus-ps5-installer $(HOST_OUT)/genplus-ps5-helper

$(HOST_OUT)/core/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -fPIC $(HOST_CORE_OPT) $(HOST_CORE_SAN) $(CORE_DEFS) $(CORE_INCS) -w -c -o $@ $<

$(HOST_OUT)/port/%.o: %.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_OPT) $(MINIZIP_FLAGS) -c -o $@ $<

$(HOST_OUT)/bridge/%.o: %.c frontend/fe_bram.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -fPIC $(HOST_CORE_OPT) $(HOST_CORE_SAN) $(CORE_DEFS) $(CORE_INCS) -Ifrontend -I$(ROOT)/$(CHD)/deps/zlib-1.3.1 -Wall -Werror -c -o $@ $<

$(HOST_OUT)/port/%.o: %.cpp coreorbis/include-orbis/*.h coreorbis/orbis-shims/*.h frontend/*.h
	@mkdir -p $(dir $@)
	$(HOST_CXX) -std=c++17 $(HOST_OPT) $(HOST_SAN) $(DEFS) $(INCS) $(PORT_WARN) $(EXTRA_FLAGS) -c -o $@ $<

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

$(HOST_OUT)/genplus-ps5-host: $(HOST_CORE_OBJ) $(HOST_EMU_OBJ) $(HOST_OUT)/helper_data.o
	$(HOST_CXX) $(HOST_SAN) -o $@ $^ -lpthread -lm

test: host
	bash tests/run-tests.sh

clean:
	rm -rf build

.PHONY: ps5 ps5-core host-core host test send strip app dist clean
.SECONDARY:
