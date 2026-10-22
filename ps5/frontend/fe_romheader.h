// The internal header of a Mega Drive cartridge or a Sega CD disc: its titles and its region.
//
// A fan translation or a hack keeps the cartridge's own header, so the title in it names the original game when the
// file name does not (fe_artmatch.h ResolveHeader). Ported from Snes9x PS5's SNES header reader (thatboialex's design,
// pull request #3), for the Sega header instead:
//   $100  16 bytes  the system: "SEGA MEGA DRIVE", "SEGA GENESIS" (a cartridge, a disc's first sector alike)
//   $120  48 bytes  the domestic (Japanese) title
//   $150  48 bytes  the overseas title
//   $1F0   3 bytes  the regions: "JUE" letters, or one hex digit (1 Japan, 4 USA, 8 Europe, added up)
// A cartridge may sit behind a 512-byte copier header, and a .smd dump is also interleaved in 16 KiB blocks (each
// block's odd bytes, then its even ones): both are undone here. A disc (.iso, or the first track of a .cue) has the
// header at the start of its first sector, 16 bytes in for a raw 2352-byte track. The Master System, the Game Gear and
// the SG-1000 have no title in their headers: nothing is read for them.
//
// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

namespace fe { namespace romheader {
struct Info
{
    std::string title;         // the title to look up: overseas, or domestic for a Japan-only game; printable ASCII
    std::string other;         // the other one, when it is text and differs; "" otherwise
    uint8_t country = 0xFF;    // 0: Japan only; 1: any other region set
    bool japan() const { return country == 0; }
};
// Fills out with n bytes at offset; false if the image has fewer.
using Reader = std::function<bool(uint64_t offset, uint8_t* out, size_t n)>;

// 48 title bytes as text: printable ASCII kept, runs of spaces made one, anything else (Shift-JIS) a space. "" when
// fewer than 3 letters or digits are left.
inline std::string Title(const uint8_t* t, size_t n)
{
    std::string out;
    int letters = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const unsigned char c = t[i];
        const bool text = c >= 0x20 && c <= 0x7E;
        if (!text || c == ' ')
        {
            if (!out.empty() && out.back() != ' ') out += ' ';
            continue;
        }
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) ++letters;
        out += char(c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return letters >= 3 ? out : std::string();
}
// The 512 bytes from $000 of a header: true when $100 names a Sega system.
inline bool Parse(const uint8_t* h, Info* out)
{
    const char* sys = reinterpret_cast<const char*>(h + 0x100);
    if (std::memcmp(sys, "SEGA", 4) != 0 && std::memcmp(sys, " SEGA", 5) != 0) return false;
    const std::string domestic = Title(h + 0x120, 48), overseas = Title(h + 0x150, 48);
    // the regions: letters (J, U, E; "JUE" is any), or a hex digit of flags (bit 0 Japan, bit 2 USA, bit 3 Europe)
    bool j = false, other = false;
    for (int i = 0; i < 3; ++i)
    {
        const char c = char(h[0x1F0 + i]);
        int v = -1;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10; // 'E' is Europe as a letter and USA + Europe as a digit: either way not Japan
        if (c == 'J') j = true;
        else if (c == 'U') other = true;
        else if (v >= 0)
        {
            j = j || (v & 1);
            other = other || (v & ~1);
        }
    }
    out->country = (j && !other) ? 0 : 1;
    const bool want_domestic = out->japan() || overseas.empty();
    out->title = want_domestic && !domestic.empty() ? domestic : overseas;
    const std::string& second = out->title == domestic ? overseas : domestic;
    out->other = second != out->title ? second : std::string();
    return !out->title.empty();
}
// A cartridge image of `total` bytes (0 when not known): plain, behind a copier header, or a .smd (interleaved); each
// way is tried, as file names do not say which it is.
inline bool ReadCart(const Reader& rd, uint64_t total, Info* out)
{
    uint8_t h[512];
    if (rd(0, h, sizeof(h)) && Parse(h, out)) return true;
    if ((total == 0 || total >= 512 + 512) && rd(512, h, sizeof(h)) && Parse(h, out)) return true;
    // interleaved: the first 16 KiB block after the copier header; odd bytes in its first half, even ones in the second
    if (total != 0 && total < 512 + 16384) return false;
    uint8_t lo[256], hi[256];
    if (!rd(512, lo, sizeof(lo)) || !rd(512 + 8192, hi, sizeof(hi))) return false;
    uint8_t d[512];
    for (int i = 0; i < 256; ++i)
    {
        d[2 * i + 1] = lo[i];
        d[2 * i] = hi[i];
    }
    return Parse(d, out);
}
// The first sector of a disc's data track: 2048-byte sectors (an .iso) have it at 0, raw 2352-byte ones 16 bytes in.
inline bool ReadDisc(const Reader& rd, Info* out)
{
    uint8_t h[512];
    if (rd(0, h, sizeof(h)) && std::memcmp(h, "SEGADISCSYSTEM", 14) == 0 && Parse(h, out)) return true;
    return rd(16, h, sizeof(h)) && std::memcmp(h, "SEGADISCSYSTEM", 14) == 0 && Parse(h, out);
}
}} // namespace fe::romheader
