// Genesis Plus GX PS5: the /data/genplus folder layout and the boot log (orbis-shims/orbis_paths.cpp).
//
//   /data/genplus/roms         games (.md .gen .smd .bin .sms .gg .sg, also in .zip; Sega CD .cue .chd .iso);
//                             sub-folders are fine
//   /data/genplus/bios         BIOS files: bios_CD_U.bin / bios_CD_E.bin / bios_CD_J.bin (Sega CD, needed),
//                             bios_U.sms, bios.gg, bios_MD.bin (optional boot screens)
//   /data/genplus/saves        battery saves (<game>.srm) and the Sega CD backup RAM (scd_U.brm ...)
//   /data/genplus/states       save states, <game>.state1 .. <game>.state10
//   /data/genplus/covers       box art (downloaded, or your own <ROM file name>.png), crc-cache.txt, wanted.txt
//   /data/genplus/logs         boot.log (this run) and boot.prev.log (the run before); installer.log, helper.log
//   /data/genplus/genplus-ps5.ini  the frontend's settings
//
// USB drives are searched too: /mnt/usbN/genplus/roms (N = 0..7) and /mnt/extN/genplus/roms.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

#ifndef ORBIS_ROOT_DEFAULT
#define ORBIS_ROOT_DEFAULT "/data/genplus"
#endif

// Root folder (ORBIS_ROOT_DEFAULT; the host tests point it elsewhere with GENPLUS_PS5_ROOT).
const std::string& OrbisRoot();
// <root>/<sub>, created at boot by OrbisPathsInit.
std::string OrbisDir(const char* sub);
// Creates the folder tree. Returns false when the root itself can't be created (no /data access).
bool OrbisPathsInit();
// Folders that hold ROMs and exist right now (internal first, then USB drives).
std::vector<std::string> OrbisRomRoots();

bool OrbisIsDir(const std::string& path);
bool OrbisIsFile(const std::string& path);
bool OrbisMkdirs(const std::string& path);

// <root>/logs/<name>.log (the run before kept as <name>.prev.log): boot.log for the emulator, installer.log
// for the installer/helper payload. Every line also goes to stdout. Lines logged before the file is open
// (before the jailbreak shows /data to the app) are kept and written first.
void OrbisLogOpen(const char* name = "boot");
void OrbisLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void OrbisLogClose();
// The open log's file descriptor, for the crash handler's signal-safe write(); -1 before it is open.
int OrbisLogFd();
