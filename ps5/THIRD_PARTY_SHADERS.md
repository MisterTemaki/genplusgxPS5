# Third-party CRT shaders

Genesis Plus GX PS5 draws its CRT shaders on the CPU (`coreorbis/orbis-shims/ProsperoCrt.cpp`). Apart from
"CRT Easymode style", which is original code written for this port, they are C++ rewrites of shaders from
libretro's slang-shaders collection (https://github.com/libretro/slang-shaders, folder `crt/`), kept with their
default parameters. Their authors and licences follow.

| Shader | Source in slang-shaders | Author | Licence |
|---|---|---|---|
| crt-lottes | `crt/shaders/crt-lottes.slang` | Timothy Lottes | public domain |
| crt-lottes-fast | `crt/shaders/crt-lottes-fast.slang` ([CRTS] 20180120b, adapted for RetroArch by hunterk) | Timothy Lottes | Unlicense (public domain) |
| crt-1tap | `crt/shaders/crt-Ntap/crt-1tap.slang` (v1.4) | fishku | public domain (CC0) |
| crt-2tap | `crt/shaders/crt-Ntap/crt-2tap.slang` (v1.0) | fishku | public domain (CC0) |
| monoCRT | `crt/shaders/monoCRT.slang` | hunterk | public domain |
| newpixie-mini | `crt/shaders/newpixie-mini/newpixie-mini.slang` (adapted for slang by hunterk) | Mattias Gustavsson | Unlicense or MIT (used here under the Unlicense) |
| crt-hyllian-fast | `crt/shaders/hyllian/crt-hyllian-fast.slang` (ported to GLSL/slang by DariusG & hunterk) | Hyllian | MIT |
| crt-nobody | `crt/shaders/crt-nobody.slang` | Hyllian | MIT |
| crt-blurPi | `crt/shaders/crt-blurPi.slang` | Oriol Ferrer Mesià (armadillu) | MIT |

"CRT Easymode style" follows the look of EasyMode's crt-easymode (`crt/shaders/crt-easymode.slang`, GPL) but
contains none of its code: the GPL can't be combined with the Genesis Plus GX licence.

## Unlicense (crt-lottes-fast, newpixie-mini)

This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or distribute this software, either in source code
form or as a compiled binary, for any purpose, commercial or non-commercial, and by any means.

In jurisdictions that recognize copyright laws, the author or authors of this software dedicate any and all
copyright interest in the software to the public domain. We make this dedication for the benefit of the public at
large and to the detriment of our heirs and successors. We intend this dedication to be an overt act of
relinquishment in perpetuity of all present and future rights to this software under copyright law.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## MIT (crt-hyllian-fast, crt-nobody, crt-blurPi)

crt-hyllian-fast: Copyright (C) 2011-2015 Hyllian - sergiogdb@gmail.com
crt-nobody: Copyright (C) 2011-2025 Hyllian - sergiogdb@gmail.com
crt-blurPi: Made by Oriol Ferrer Mesià (armadillu), http://uri.cat

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of
the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## CC0 (crt-1tap, crt-2tap) and public domain (crt-lottes, monoCRT)

crt-1tap and crt-2tap: "Copyright (C) 2023-2026, Public domain license (CC0)" -- fishku. crt-lottes: "PUBLIC
DOMAIN CRT STYLED SCAN-LINE SHADER by Timothy Lottes". monoCRT: "by hunterk, license: public domain".
