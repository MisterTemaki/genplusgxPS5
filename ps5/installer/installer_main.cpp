// Genesis Plus GX PS5: the payloads (GenesisPlusGXPS5.elf and GenesisPlusGXPS5-helper.elf).
//
// GenesisPlusGXPS5.elf, sent with the PS5 Payload Manager / the ELF loader (port 9021), is what PS5SX2's installer and
// helper are together:
//   1. it installs or updates the dashboard app in /data/homebrew/<TITLE_ID> (eboot.bin = Genesis Plus GX itself,
//      sce_module/libc.prx, sce_sys/param.json, icon0.png, pic0.dds, pic1.dds), built into it (install_data.cpp);
//   2. then it stays running as the helper that lets the app out of its sandbox (ProsperoJailbreak.h), until
//      the console is turned off. A second copy sent while one runs only installs.
// GenesisPlusGXPS5-helper.elf (GENPLUS_HELPER_ONLY) is step 2 alone: the app carries it and sends it to the ELF
// loader itself when no helper answers (after a reboot, for instance).
//
// Before that it brings an install made under the old title ID (PPSA99011, 1.0-1.6) over to the new one
// (ProsperoMigrate.h): our old folder is renamed, its home-screen registration removed through ShadowMountPlus.
//
// Nothing here writes outside /data/genplus, /data/homebrew/<TITLE_ID> and our own old /data/homebrew/PPSA99011.
//
// SPDX-License-Identifier: MIT

#include "OrbisPaths.h"
#include "ProsperoJailbreak.h"
#include "ProsperoMigrate.h"
#include "ProsperoNotify.h"
#ifndef GENPLUS_HELPER_ONLY
#include "ProsperoInstall.h"
#endif

#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

#ifndef GENPLUS_PS5_VERSION
#define GENPLUS_PS5_VERSION "dev"
#endif

namespace
{
std::string g_ready_message;

void OnReady()
{
	if (!g_ready_message.empty())
		ProsperoNotify("%s", g_ready_message.c_str());
	ProsperoNotifyFlush();
}
} // namespace

int main()
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	const bool have_data = OrbisPathsInit();
#ifdef GENPLUS_HELPER_ONLY
	OrbisLogOpen("helper");
	OrbisLog("[helper] Genesis Plus GX PS5 helper %s (built %s %s), pid %d%s", GENPLUS_PS5_VERSION, __DATE__, __TIME__,
		int(getpid()), have_data ? "" : " (no /data: no log file)");
	// the app started this helper: it may have left its old registration for us (ProsperoMigrate.h)
	migrate::Run(true);
#else
	OrbisLogOpen("installer");
	OrbisLog("[installer] Genesis Plus GX PS5 %s (built %s %s), pid %d", GENPLUS_PS5_VERSION, __DATE__, __TIME__, int(getpid()));
	if (!have_data)
		OrbisLog("[installer] can't create %s", OrbisRoot().c_str());

	// an install under the old title ID first: renamed to the new one, so the install below only updates it
	const migrate::Result moved = migrate::Run(true);

	const char* open_hint = "Open it from the Genesis Plus GX PS5 icon on the home screen.";
	char msg[1024];
	switch (InstallApp())
	{
		case InstallResult::Installed:
			snprintf(msg, sizeof(msg), "Genesis Plus GX PS5 %s installed. %s", GENPLUS_PS5_VERSION, open_hint);
			break;
		case InstallResult::Updated:
			snprintf(msg, sizeof(msg), "Genesis Plus GX PS5 updated to %s. %s", GENPLUS_PS5_VERSION, open_hint);
			break;
		case InstallResult::UpToDate:
			snprintf(msg, sizeof(msg), "Genesis Plus GX PS5 %s is ready. %s", GENPLUS_PS5_VERSION, open_hint);
			break;
		case InstallResult::Failed:
			snprintf(msg, sizeof(msg), "Genesis Plus GX PS5: could not install the app in %s (see %s/logs/installer.log)",
				AppInstallDir().c_str(), OrbisRoot().c_str());
			break;
		default:
			snprintf(msg, sizeof(msg), "Genesis Plus GX PS5: nothing to install");
			break;
	}
	if (SyncAppMeta() > 0)
	{
		const size_t len = strlen(msg);
		snprintf(msg + len, sizeof(msg) - len, " Home screen art updated (restart the PS5 if it doesn't show).");
	}
	if (moved.moved || moved.removed)
	{
		const size_t len = strlen(msg);
		snprintf(msg + len, sizeof(msg) - len, " Its title ID is now %s (was %s); games, saves and settings are kept.",
			GENPLUS_TITLE_ID, moved.old_ids.c_str());
	}
	if (moved.pending > 0)
	{
		const size_t len = strlen(msg);
		if (moved.no_api)
			snprintf(msg + len, sizeof(msg) - len,
				" Remove the old icon: select it on the home screen, OPTIONS, Delete.");
		else
			snprintf(msg + len, sizeof(msg) - len, " The old icon is removed in the background.");
	}
	OrbisLog("[installer] %s", msg);
	g_ready_message = msg;
#endif
	migrate::StartRetryThread();

	// the helper: runs until the console is turned off, or returns at once when one already runs
	if (!jailbreak::ServeHelper(OnReady))
	{
#ifndef GENPLUS_HELPER_ONLY
		ProsperoNotify("%s", g_ready_message.c_str());
#endif
		OrbisLog("[helper] another helper is running: leaving");
	}
	ProsperoNotifyFlush();
	OrbisLogClose();
	return 0;
}
