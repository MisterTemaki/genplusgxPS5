// Genesis Plus GX PS5: GenesisPlusGXPS5-helper.elf built into the app (ProsperoJailbreak.h). Only the native eboot links
// this file; the Makefile passes HELPER_ELF, the helper payload's path.
//
// SPDX-License-Identifier: MIT

#include "ProsperoJailbreak.h"

__asm__(".section .rodata\n"
		".balign 16\n"
		".global genplus_helper_elf_begin\n"
		"genplus_helper_elf_begin:\n"
		".incbin \"" HELPER_ELF "\"\n"
		".global genplus_helper_elf_end\n"
		"genplus_helper_elf_end:\n"
		".previous\n");
extern "C" const unsigned char genplus_helper_elf_begin[];
extern "C" const unsigned char genplus_helper_elf_end[];

namespace jailbreak
{
Blob EmbeddedHelper()
{
	return {genplus_helper_elf_begin, size_t(genplus_helper_elf_end - genplus_helper_elf_begin)};
}
} // namespace jailbreak
