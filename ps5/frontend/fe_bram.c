/* Genesis Plus GX PS5: the Sega CD backup RAM, saved while the game runs (fe_bram.h).
 *
 * The core (libretro/libretro.c) writes the Sega CD's internal backup RAM (scd_U.brm / scd_E.brm / scd_J.brm)
 * and the RAM cartridge (cart.brm) only when the game is unloaded, so a save made in a Sega CD game was lost when
 * the app was closed from the PS button, crashed or lost power. This file writes them to the same files every few
 * seconds when they changed, the way the frontend already does for cartridge battery RAM. It is compiled with the
 * core's defines and headers (it reads the core's own `scd` state and the paths libretro.c built), and changes
 * nothing in the core.
 *
 * SPDX-License-Identifier: MIT
 */
#include "shared.h"
#include "fe_bram.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <zlib.h>

/* libretro.c's paths (set by retro_load_game) and the console's region */
extern char CD_BRAM_JP[256];
extern char CD_BRAM_EU[256];
extern char CD_BRAM_US[256];
extern char CART_BRAM[256];

/* the last 0x20 bytes of a formatted backup RAM (libretro.c's brm_format + 0x20) */
static const uint8 kFormatTail[0x20] = {0x53, 0x45, 0x47, 0x41, 0x5f, 0x43, 0x44, 0x5f, 0x52, 0x4f, 0x4d, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x52, 0x41, 0x4d, 0x5f, 0x43, 0x41, 0x52, 0x54, 0x52, 0x49, 0x44, 0x47, 0x45, 0x5f, 0x5f, 0x5f};

static uint32 s_crc[2];
static int s_active;
static int s_backed[2]; /* the file was copied to <file>.bak before this session's first write */

static const char* InternalPath(void)
{
	switch (region_code)
	{
		case REGION_JAPAN_NTSC: return CD_BRAM_JP;
		case REGION_EUROPE: return CD_BRAM_EU;
		case REGION_USA: return CD_BRAM_US;
		default: return NULL;
	}
}

/* data -> path.part, flushed to the disk, then renamed over path: the old file or the new one, never half of one */
static int WriteAtomic(const char* path, const uint8* data, size_t size)
{
	char tmp[300];
	if (snprintf(tmp, sizeof(tmp), "%s.part", path) >= (int)sizeof(tmp))
		return 0;
	/* POSIX calls: the core's headers turn FILE/fopen into libretro streams */
	const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
		return 0;
	int ok = 1;
	size_t off = 0;
	while (ok && off < size)
	{
		const ssize_t n = write(fd, data + off, size - off);
		if (n <= 0)
			ok = 0;
		else
			off += (size_t)n;
	}
	ok = (fsync(fd) == 0) && ok;
	ok = (close(fd) == 0) && ok;
	if (!ok || rename(tmp, path) != 0)
	{
		unlink(tmp);
		return 0;
	}
	return 1;
}

/* Before the first write of a session, the file as it was is copied to <file>.bak (the Sega CD's backup RAM holds
 * the saves of every game of that region: one bad write must not cost them all). The session counts as protected
 * only once the copy is really there, or when there was no file to copy; returns whether the file may be written
 * over. While it may not, the new contents go beside it (<file>.new) and the copy is tried again next time. */
static int BackupOnce(const char* path, int which)
{
	char bak[300];
	struct stat st;
	uint8* buf;
	ssize_t n;
	int fd, ok = 0;
	if (s_backed[which])
		return 1;
	if (snprintf(bak, sizeof(bak), "%s.bak", path) >= (int)sizeof(bak))
		return 0;
	fd = open(path, O_RDONLY);
	if (fd < 0)
	{
		if (errno != ENOENT)
			return 0; /* there, but unreadable: don't write over it */
		s_backed[which] = 1; /* nothing to keep */
		return 1;
	}
	if (fstat(fd, &st) != 0 || st.st_size > 0x100000)
	{
		close(fd);
		return 0;
	}
	if (st.st_size == 0)
	{
		close(fd);
		s_backed[which] = 1;
		return 1;
	}
	buf = (uint8*)malloc((size_t)st.st_size);
	if (!buf)
	{
		close(fd);
		return 0;
	}
	n = read(fd, buf, (size_t)st.st_size);
	close(fd);
	if (n == st.st_size)
		ok = WriteAtomic(bak, buf, (size_t)n);
	free(buf);
	if (ok)
		s_backed[which] = 1;
	return ok;
}

/* The file, or <file>.new beside it while it can't be backed up. Returns 1 written over the file, 2 written beside
 * it, 0 failed. */
static int WriteProtected(const char* path, int which, const uint8* data, size_t size)
{
	char beside[300];
	if (BackupOnce(path, which))
		return WriteAtomic(path, data, size) ? 1 : 0;
	if (snprintf(beside, sizeof(beside), "%s.new", path) >= (int)sizeof(beside))
		return 0;
	return WriteAtomic(beside, data, size) ? 2 : 0;
}

void fe_bram_start(void)
{
	s_backed[0] = s_backed[1] = 0;
	s_active = system_hw == SYSTEM_MCD;
	if (!s_active)
		return;
	/* what the core loaded (or formatted): nothing to write until the game changes it */
	s_crc[0] = crc32(0, scd.bram, 0x2000);
	s_crc[1] = scd.cartridge.id ? crc32(0, scd.cartridge.area, scd.cartridge.mask + 1) : 0;
}

int fe_bram_flush(void)
{
	int written = 0;
	if (!s_active)
		return 0;
	{
		const uint32 crc = crc32(0, scd.bram, 0x2000);
		const char* path = InternalPath();
		if (crc != s_crc[0] && path && path[0] && !memcmp(scd.bram + 0x2000 - 0x20, kFormatTail, 0x20))
		{
			const int w = WriteProtected(path, 0, scd.bram, 0x2000);
			if (w)
			{
				s_crc[0] = crc;
				written |= w == 1 ? 1 : (1 | 8);
			}
			else
				written |= 4;
		}
	}
	if (scd.cartridge.id)
	{
		const size_t size = (size_t)scd.cartridge.mask + 1;
		const uint32 crc = crc32(0, scd.cartridge.area, size);
		if (crc != s_crc[1] && CART_BRAM[0] && !memcmp(scd.cartridge.area + size - 0x20, kFormatTail, 0x20))
		{
			const int w = WriteProtected(CART_BRAM, 1, scd.cartridge.area, size);
			if (w)
			{
				s_crc[1] = crc;
				written |= w == 1 ? 2 : (2 | 8);
			}
			else
				written |= 4;
		}
	}
	return written;
}

void fe_bram_stop(void)
{
	/* the core writes the backup RAM once more when the game is unloaded (libretro.c, bram_save: plain fopen and
	 * fwrite, no backup). fe_bram_flush has just written it the protected way: the core's paths are cleared so its
	 * own write opens nothing. check_variables sets them again for the next game. */
	if (s_active)
	{
		CD_BRAM_JP[0] = CD_BRAM_EU[0] = CD_BRAM_US[0] = 0;
		CART_BRAM[0] = 0;
	}
	s_active = 0;
}

/* For the performance log (fe_emu.cpp): what the CD drive and the PCM chip are doing now. */
int fe_cd_activity(void)
{
	int bits = 0;
	if (!cdd.loaded)
		return 0;
	if (cdd.status == CD_PLAY && cdd.index >= 0 && cdd.index < cdd.toc.last)
		bits |= cdd.toc.tracks[cdd.index].type ? 4 : 1; /* data read : CD audio */
	if (system_hw == SYSTEM_MCD && scd.pcm_hw.enabled && (uint8)~scd.pcm_hw.status)
		bits |= 2;
	return bits;
}
