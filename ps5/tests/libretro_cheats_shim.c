/* Genesis Plus GX PS5 tests: libretro.c's cheat code (copied out by extract_libretro_cheats.py) on fake ROM and RAM.
 *
 * The core's state the cheat code touches -- system_hw, the cartridge ROM, the Work-RAM, the Z80's read map, the Sega
 * CD's PRG-RAM and Word-RAM -- stands here as plain arrays; test_cheats.cpp sets them up and calls the core's functions
 * through the up_* functions below. Built with the core's own -DLSB_FIRST, as on the PS5.
 *
 * SPDX-License-Identifier: MIT (this file; the code it includes keeps libretro.c's licence)
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* core/system.h */
#define SYSTEM_SG 0x01
#define SYSTEM_SMS 0x20
#define SYSTEM_GG 0x40
#define SYSTEM_MD 0x80
#define SYSTEM_PBC 0x81
#define SYSTEM_MCD 0x84

uint8_t system_hw;
static struct
{
	uint8_t* rom;
	uint32_t romsize;
} cart;
static struct
{
	struct
	{
		uint8_t boot;
	} cartridge;
	uint8_t prg_ram[0x80000];
	uint8_t word_ram_2M[0x40000];
} scd;
static uint8_t work_ram[0x10000];
static uint8_t* z80_readmap[64];
static uint8_t rom_data[0x40000];

#include "libretro_cheats.inc"

void up_setup(uint8_t hw, uint32_t romsize)
{
	retro_cheat_reset();
	system_hw = hw;
	memset(rom_data, 0, sizeof(rom_data));
	memset(work_ram, 0, sizeof(work_ram));
	memset(scd.prg_ram, 0, sizeof(scd.prg_ram));
	memset(scd.word_ram_2M, 0, sizeof(scd.word_ram_2M));
	scd.cartridge.boot = 0;
	cart.rom = rom_data;
	cart.romsize = romsize;
	/* the 8-bit systems: the first 64 KB of the ROM straight in the Z80's map (no bank switched) */
	for (int i = 0; i < 64; i++)
		z80_readmap[i] = rom_data + (i << 10);
}

uint8_t* up_rom(void) { return rom_data; }
uint8_t* up_work_ram(void) { return work_ram; }
uint8_t* up_prg_ram(void) { return scd.prg_ram; }
uint8_t* up_word_ram(void) { return scd.word_ram_2M; }
/* a bank switch: the Z80's page `page` (1 KB) now shows `rom_offset` of the ROM */
void up_map(int page, uint32_t rom_offset) { z80_readmap[page] = rom_data + rom_offset; }

/* decode_cheat on its own: 0 = not a code, else its length; *address / *data / *ref as the core reads them */
uint32_t up_decode(const char* code, uint32_t* address, uint16_t* data, uint16_t* ref)
{
	char buf[256];
	memset(buf, 0, sizeof(buf));
	strncpy(buf, code, sizeof(buf) - 1);
	memset(&cheatlist[0], 0, sizeof(cheatlist[0]));
	const uint32_t len = decode_cheat(buf, 0);
	*address = cheatlist[0].address;
	*data = cheatlist[0].data;
	*ref = cheatlist[0].old;
	return len;
}

void up_cheat_reset(void) { retro_cheat_reset(); }
void up_cheat_set(unsigned index, bool enabled, const char* code) { retro_cheat_set(index, enabled, code); }
void up_frame(void) { RAMCheatUpdate(); } /* osd_input_update: once a frame */
void up_bank_switch(void) { ROMCheatUpdate(); } /* the mappers' CHEATS_UPDATE() */
int up_count(void) { return maxcheats; }
