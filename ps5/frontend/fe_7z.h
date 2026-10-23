// Genesis Plus GX PS5 frontend: reading .7z archives (2.2), with the LZMA SDK's C decoder (ps5/third_party/lzma7z, public
// domain, every name prefixed fe7z_ so it can't meet libchdr's older LZMA SDK).
//
// The archive is read through the ordinary stdio calls (fopen, fread, fseeko, ftello), so a .7z on a network share goes
// through netfs like any file. Decoded: LZMA, LZMA2, PPMd, BCJ, BCJ2, the ARM/ARM64/ARMT/PPC/SPARC/IA64/RISC-V filters,
// Delta and stored data. Not: encrypted archives (no AES: Status::Encrypted), BZip2, Deflate, Zstandard... (Unsupported).
//
// Memory: a solid block is decoded whole, so the block that holds a file must fit max_block (it is refused before
// anything is decoded); every allocation of the decoder (the archive's header, PPMd's model, the block) comes from a
// budget of kMemoryBudget bytes, beyond which the archive is refused (Status::TooBig) instead of eating the console's memory.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fe
{
namespace sevenzip
{
enum class Status
{
	Ok,
	CantOpen, // the file could not be opened
	NotArchive, // not a 7z (no signature)
	Damaged, // a broken or cut-short archive, a CRC that does not match
	Unsupported, // a compression method this decoder lacks
	Encrypted, // a password-protected archive (its data, or its whole list of files)
	TooBig, // the file is over max_entry, or the decoder would need more than kMemoryBudget
	BlockTooBig, // the solid block that holds the file is over max_block
	ReadError, // the file could not be read (a share that dropped...)
	Cancelled, // the cancel flag turned true
};
const char* StatusText(Status s); // "damaged", "encrypted"... (for the log)

// What the decoder may allocate for one archive, the decoded block included.
constexpr uint64_t kMemoryBudget = 136ull << 20;

struct Entry
{
	std::string name; // UTF-8, as stored (a path inside the archive keeps its folders)
	uint64_t size = 0;
	bool dir = false;
	bool has_crc = false;
	uint32_t crc = 0; // the CRC32 the archive stores for the file (no decoding needed)
};

class Archive
{
public:
	Archive();
	~Archive();
	Archive(const Archive&) = delete;
	Archive& operator=(const Archive&) = delete;

	// Opens the archive and reads its list of files. cancel (when given): reading stops when it turns true.
	Status Open(const std::string& path, const std::atomic<bool>* cancel = nullptr);
	const std::vector<Entry>& Entries() const;
	// The size of the solid block that holds entry i (what has to be decoded to get it); 0 for an empty file.
	uint64_t BlockSize(size_t i) const;
	// Entry i's bytes. Refused before decoding: a file over max_entry (TooBig), a block over max_block (BlockTooBig),
	// an encrypted block (Encrypted).
	Status Extract(size_t i, std::vector<uint8_t>* out, uint64_t max_entry, uint64_t max_block);

private:
	struct Impl;
	std::unique_ptr<Impl> m_i;
};
} // namespace sevenzip
} // namespace fe
