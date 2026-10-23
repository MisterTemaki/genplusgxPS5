# lzma7z: the LZMA SDK's C 7z decoder

Genesis Plus GX PS5 2.2 reads cartridge games inside `.7z` archives with these files (`ps5/frontend/fe_7z.cpp`).

- **Source:** the official 7-Zip repository, tag **26.05** (commit `b88c6bc09339de32f7d18bbab900686b244f42ba`), folder `C/`:
  `7zArcIn.c 7zBuf.c 7zCrc.c 7zCrcOpt.c 7zDec.c 7zStream.c Bcj2.c Bra.c Bra86.c BraIA64.c Delta.c Lzma2Dec.c
  LzmaDec.c Ppmd7.c Ppmd7Dec.c` and the headers they include (`7z.h 7zBuf.h 7zCrc.h 7zTypes.h Bcj2.h Bra.h Compiler.h
  CpuArch.h Delta.h Lzma2Dec.h LzmaDec.h Ppmd.h Ppmd7.h Precomp.h RotateDefs.h`). The files are **unchanged**.
- **Licence:** each of these files states "Igor Pavlov : Public domain" in its first lines, and 7-Zip's
  `DOC/License.txt` says that files whose source states public-domain status are public domain (the rest of 7-Zip is
  LGPL, BSD or under the unRAR restriction; none of those files is here). So this folder is **public domain**.
- **Build** (ps5/Makefile): `-DZ7_ST` (no threads), `-DZ7_PPMD_SUPPORT`, `-DZ7_EXTRACT_ONLY`, no assembler (no
  `Z7_LZMA_DEC_OPT`), and no AES (`7zAes` is not here: an encrypted archive is refused). The archive is read through
  the app's own stdio calls (fe_7z.cpp's `ISeekInStream` over `FILE*`), so `7zFile.c` is not used.
- **Names:** `fe7z_names.h` (ours, MIT) renames every external name of these files to `fe7z_<name>`. It is
  force-included into each of them (`-include fe7z_names.h`) and included by fe_7z.cpp before the SDK's headers.
  libchdr carries an older LZMA SDK (`core/cd_hw/libchdr/deps/lzma-24.05`, with `LzmaDec_*`, `CpuArch.c`...); without
  the prefix the two copies would clash in the link or, worse, mix two versions. The Makefile's `lzma7z-names` step
  (`build/*/lzma7z/names.ok`) fails the build when an object defines a global name without the prefix (a name a newer
  SDK adds): add it to `fe7z_names.h` (the list is what `nm -g --defined-only` prints for these objects).

What is decoded: LZMA, LZMA2, PPMd (as 7-Zip writes it: 5 bytes of properties), BCJ, BCJ2, the ARM, ARM64, ARMT, PPC,
SPARC, IA64 and RISC-V filters, Delta, and stored files. Not: BZip2, Deflate, Deflate64, Zstandard and the other
methods of 7-Zip's C++ code, nor encrypted archives.
