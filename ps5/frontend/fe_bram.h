/* Genesis Plus GX PS5: the Sega CD backup RAM, saved while the game runs (fe_bram.c).
 * SPDX-License-Identifier: MIT
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* After a game loaded: remembers the backup RAM as the core loaded it (nothing to do for other systems). */
void fe_bram_start(void);
/* Writes the internal backup RAM and the RAM cartridge, when changed, to the files the core uses (atomically).
 * Returns a bit set: 1 internal written, 2 cartridge written, 4 a write failed; 0 when nothing changed. */
int fe_bram_flush(void);
/* Before the game is unloaded (the core then writes them itself one last time). */
void fe_bram_stop(void);

#ifdef __cplusplus
}
#endif
