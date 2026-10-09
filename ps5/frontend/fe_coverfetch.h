// Genesis Plus GX PS5: downloading covers, shared by the app (the shelf, the prefetch) and the helper payload, which
// downloads them in the background while the app runs (fe_coverworker.h).
//
// SPDX-License-Identifier: MIT
#pragma once

#include "fe_http.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fe
{
// A cover the library still needs: its file in /data/genplus/covers and where to get it (one or more addresses,
// tab-separated, tried in order). The list of them is covers/wanted.txt ("file<TAB>url" lines).
struct WantedCover
{
	std::string file; // "MegaDrive/Sonic The Hedgehog (USA, Europe).png"
	std::string url;
};


// A PNG or a JPEG (by its first bytes).
bool IsImage(const std::vector<uint8_t>& d);

// One GET per address until one has an image, following libretro-thumbnails' git symlinks (an "image" that is
// the real file's name). The HTTP status (404 only when no address has it); `data` is the image when it is 200.
int FetchCoverUrl(HttpClient& http, std::string urls, const std::string& label, std::vector<uint8_t>& data);

// "file<TAB>url[<TAB>url...]" lines -> the covers, keeping only plain names in a system's cover folder ("MegaDrive/x.png")
// and http(s) addresses; a last line without its end (a list being written) is skipped.
std::vector<WantedCover> ParseWantedList(const std::string& text);
bool SafeCoverFile(const std::string& file);

// What a download leaves in `covers_dir`: the image (written atomically, and Square's "<name>.refetch" request
// removed), or for a 404 "<name>.missing" (not asked again for 30 days). 1 saved, 0 not on the server, -1 else.
int SaveCoverResult(const std::string& covers_dir, const WantedCover& w, int status, const std::vector<uint8_t>& data);

bool WriteFileAtomicTo(const std::string& path, const std::vector<uint8_t>& data);
} // namespace fe
