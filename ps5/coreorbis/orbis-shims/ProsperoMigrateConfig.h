// Genesis Plus GX PS5's title IDs (ProsperoMigrate.h): the current one and the ones it used before.
//
// SPDX-License-Identifier: MIT
#pragma once

#ifndef GENPLUS_TITLE_ID
#define GENPLUS_TITLE_ID "PPSA99921"
#endif

#define MIG_APP_NAME "Genesis Plus GX PS5"
#define MIG_NEW_TITLE GENPLUS_TITLE_ID
// 1.0-1.6 were PPSA99011, which the homebrew catalog lists for another app (PS5 FSR4 Showcase)
#define MIG_OLD_TITLES "PPSA99011"
// the label of our content ID (UP9000-<title>_00-<label>), the same under every title ID
#define MIG_CONTENT_LABEL "GENPLUSGXPS50001"
// the host tests' override: <prefix>PS5_HOMEBREW
#define MIG_ENV_PREFIX "GENPLUS_"
