// Genesis Plus GX PS5 frontend: reading .7z archives (fe_7z.h).
// SPDX-License-Identifier: MIT

#include "fe_7z.h"

// the vendored SDK's names, prefixed (third_party/lzma7z/fe7z_names.h), before its headers
#include "fe7z_names.h"
#include "7z.h"
#include "7zCrc.h"

#include <sys/types.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace fe
{
namespace sevenzip
{
namespace
{
// ISeekInStream over a FILE*: fread/fseeko/ftello, which netfs answers for a file on a share (the link's --wrap list).
struct FileStream
{
	ISeekInStream vt; // first: the SDK hands back a pointer to it
	FILE* f = nullptr;
	const std::atomic<bool>* cancel = nullptr;
	bool read_error = false;
	bool cancelled = false;
};

FileStream* Self(ISeekInStreamPtr p)
{
	return const_cast<FileStream*>(reinterpret_cast<const FileStream*>(p));
}

SRes StreamRead(ISeekInStreamPtr p, void* buf, size_t* size)
{
	FileStream* s = Self(p);
	if (s->cancel && s->cancel->load())
	{
		s->cancelled = true;
		*size = 0;
		return SZ_ERROR_READ;
	}
	if (*size == 0)
		return SZ_OK;
	const size_t got = fread(buf, 1, *size, s->f);
	*size = got;
	if (got == 0 && ferror(s->f))
	{
		s->read_error = true;
		return SZ_ERROR_READ;
	}
	return SZ_OK; // 0 bytes without an error: the end of the file
}

SRes StreamSeek(ISeekInStreamPtr p, Int64* pos, ESzSeek origin)
{
	FileStream* s = Self(p);
	const int whence = origin == SZ_SEEK_SET ? SEEK_SET : origin == SZ_SEEK_CUR ? SEEK_CUR : SEEK_END;
	if (fseeko(s->f, off_t(*pos), whence) != 0)
	{
		s->read_error = true;
		return SZ_ERROR_READ;
	}
	const off_t at = ftello(s->f);
	if (at < 0)
	{
		s->read_error = true;
		return SZ_ERROR_READ;
	}
	*pos = Int64(at);
	return SZ_OK;
}

// The decoder's allocations, against a budget (fe_7z.h): a size in front of each block, so Free knows what comes back.
struct Budget
{
	ISzAlloc vt; // first, as above
	uint64_t used = 0;
	uint64_t cap = kMemoryBudget;
	bool refused = false;
};

constexpr size_t kHead = 16; // keeps the blocks 16-byte aligned

void* BudgetAlloc(ISzAllocPtr p, size_t size)
{
	Budget* b = const_cast<Budget*>(reinterpret_cast<const Budget*>(p));
	if (size == 0)
		return nullptr; // as the SDK's own SzAlloc
	if (uint64_t(size) > b->cap - b->used)
	{
		b->refused = true;
		return nullptr;
	}
	char* m = static_cast<char*>(malloc(size + kHead));
	if (!m)
		return nullptr;
	memcpy(m, &size, sizeof(size));
	b->used += size;
	return m + kHead;
}

void BudgetFree(ISzAllocPtr p, void* address)
{
	if (!address)
		return;
	Budget* b = const_cast<Budget*>(reinterpret_cast<const Budget*>(p));
	char* m = static_cast<char*>(address) - kHead;
	size_t size = 0;
	memcpy(&size, m, sizeof(size));
	b->used -= size;
	free(m);
}

std::once_flag g_crc_table;

constexpr UInt32 kAesMethod = 0x06F10701; // 7zAES: what a password-protected 7z is packed with
constexpr size_t kLookBuf = 1 << 18; // as the SDK's 7zMain

void AppendUtf8(std::string& out, uint32_t c)
{
	if (c < 0x80)
		out += char(c);
	else if (c < 0x800)
	{
		out += char(0xC0 | (c >> 6));
		out += char(0x80 | (c & 0x3F));
	}
	else if (c < 0x10000)
	{
		out += char(0xE0 | (c >> 12));
		out += char(0x80 | ((c >> 6) & 0x3F));
		out += char(0x80 | (c & 0x3F));
	}
	else
	{
		out += char(0xF0 | (c >> 18));
		out += char(0x80 | ((c >> 12) & 0x3F));
		out += char(0x80 | ((c >> 6) & 0x3F));
		out += char(0x80 | (c & 0x3F));
	}
}

std::string Utf16ToUtf8(const UInt16* s, size_t n)
{
	std::string out;
	for (size_t i = 0; i < n && s[i]; i++)
	{
		uint32_t c = s[i];
		if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
		{
			c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
			i++;
		}
		else if (c >= 0xD800 && c < 0xE000)
			c = 0xFFFD; // a lone surrogate
		AppendUtf8(out, c);
	}
	return out;
}

uint64_t Le64(const uint8_t* p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

// An archive whose list of files is encrypted (7z -mhe) fails to open as "unsupported": its packed header names the AES
// coder (06 F1 07 01) in the clear, which tells it from a method the decoder lacks.
bool HeaderIsEncrypted(FILE* f)
{
	uint8_t start[32];
	if (fseeko(f, 0, SEEK_SET) != 0 || fread(start, 1, sizeof(start), f) != sizeof(start))
		return false;
	static const uint8_t kSig[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
	if (memcmp(start, kSig, sizeof(kSig)) != 0)
		return false;
	const uint64_t offset = Le64(start + 12), size = Le64(start + 20);
	if (size == 0 || size > (1u << 20) || offset > (uint64_t(1) << 40))
		return false;
	std::vector<uint8_t> h(size_t(size), 0);
	if (fseeko(f, off_t(32 + offset), SEEK_SET) != 0 || fread(h.data(), 1, h.size(), f) != h.size())
		return false;
	// as a coder's method ID: after the coder's flags byte, whose low 4 bits say a 4-byte ID (the packed header's own
	// coder list is not encrypted)
	static const uint8_t kAes[4] = {0x06, 0xF1, 0x07, 0x01};
	for (size_t i = 1; i + 4 <= h.size(); i++)
		if ((h[i - 1] & 0x0F) == 4 && memcmp(h.data() + i, kAes, 4) == 0)
			return true;
	return false;
}
} // namespace

const char* StatusText(Status s)
{
	switch (s)
	{
		case Status::Ok: return "ok";
		case Status::CantOpen: return "can't be opened";
		case Status::NotArchive: return "not a 7z archive";
		case Status::Damaged: return "damaged";
		case Status::Unsupported: return "a compression method that is not supported";
		case Status::Encrypted: return "encrypted";
		case Status::TooBig: return "too big";
		case Status::BlockTooBig: return "solid block too big";
		case Status::ReadError: return "read error";
		case Status::Cancelled: return "cancelled";
	}
	return "?";
}

struct Archive::Impl
{
	FileStream stream;
	CLookToRead2 look;
	std::vector<uint8_t> look_buf;
	Budget budget;
	CSzArEx db;
	bool open = false;
	std::vector<Entry> entries;

	Impl()
	{
		stream.vt.Read = StreamRead;
		stream.vt.Seek = StreamSeek;
		budget.vt.Alloc = BudgetAlloc;
		budget.vt.Free = BudgetFree;
		memset(&look, 0, sizeof(look));
		SzArEx_Init(&db);
	}
	~Impl()
	{
		if (open)
			SzArEx_Free(&db, &budget.vt);
		if (stream.f)
			fclose(stream.f);
	}
	// What a result of the SDK means here.
	Status Map(SRes r) const
	{
		if (r == SZ_OK)
			return Status::Ok;
		if (stream.cancelled)
			return Status::Cancelled;
		if (stream.read_error || r == SZ_ERROR_READ)
			return Status::ReadError;
		if (r == SZ_ERROR_MEM)
			return Status::TooBig; // over the budget (or the console's memory)
		if (r == SZ_ERROR_UNSUPPORTED)
			return Status::Unsupported;
		if (r == SZ_ERROR_NO_ARCHIVE)
			return Status::NotArchive;
		return Status::Damaged; // DATA, CRC, ARCHIVE, INPUT_EOF, FAIL...
	}
	bool FolderEncrypted(UInt32 folder) const
	{
		const CSzAr& ar = db.db;
		CSzFolder f;
		CSzData sd;
		sd.Data = ar.CodersData + ar.FoCodersOffsets[folder];
		sd.Size = ar.FoCodersOffsets[folder + 1] - ar.FoCodersOffsets[folder];
		if (SzGetNextFolderItem(&f, &sd) != SZ_OK)
			return false;
		for (UInt32 c = 0; c < f.NumCoders; c++)
			if (f.Coders[c].MethodID == kAesMethod)
				return true;
		return false;
	}
};

Archive::Archive() : m_i(new Impl) {}
Archive::~Archive() = default;

const std::vector<Entry>& Archive::Entries() const
{
	return m_i->entries;
}

Status Archive::Open(const std::string& path, const std::atomic<bool>* cancel, uint32_t max_files)
{
	Impl& a = *m_i;
	if (a.stream.f)
		return Status::CantOpen; // one archive per object
	std::call_once(g_crc_table, [] { CrcGenerateTable(); });
	a.stream.f = fopen(path.c_str(), "rb");
	if (!a.stream.f)
		return Status::CantOpen;
	a.stream.cancel = cancel;
	a.look_buf.resize(kLookBuf);
	LookToRead2_CreateVTable(&a.look, False);
	a.look.buf = a.look_buf.data();
	a.look.bufSize = a.look_buf.size();
	a.look.realStream = &a.stream.vt;
	LookToRead2_INIT(&a.look);
	const SRes r = SzArEx_Open(&a.db, &a.look.vt, &a.budget.vt, &a.budget.vt);
	if (r != SZ_OK)
	{
		Status s = a.Map(r);
		SzArEx_Free(&a.db, &a.budget.vt);
		if (s == Status::Unsupported)
		{
			clearerr(a.stream.f);
			if (HeaderIsEncrypted(a.stream.f))
				s = Status::Encrypted;
		}
		return s;
	}
	a.open = true;
	if (a.db.NumFiles > max_files)
		return Status::Damaged; // a list this long is no game's (and its entries would be outside the budget)
	std::vector<UInt16> name;
	a.entries.resize(a.db.NumFiles);
	for (UInt32 i = 0; i < a.db.NumFiles; i++)
	{
		Entry& e = a.entries[i];
		const size_t len = SzArEx_GetFileNameUtf16(&a.db, i, nullptr);
		name.assign(len + 1, 0);
		SzArEx_GetFileNameUtf16(&a.db, i, name.data());
		e.name = Utf16ToUtf8(name.data(), len);
		e.dir = SzArEx_IsDir(&a.db, i);
		e.size = SzArEx_GetFileSize(&a.db, i);
		e.has_crc = SzBitWithVals_Check(&a.db.CRCs, i);
		e.crc = e.has_crc ? a.db.CRCs.Vals[i] : 0;
	}
	return Status::Ok;
}

uint64_t Archive::BlockSize(size_t i) const
{
	const Impl& a = *m_i;
	if (!a.open || i >= a.entries.size())
		return 0;
	const UInt32 folder = a.db.FileToFolder[i];
	return folder == UInt32(-1) ? 0 : SzAr_GetFolderUnpackSize(&a.db.db, folder);
}

Status Archive::Extract(size_t i, std::vector<uint8_t>* out, uint64_t max_entry, uint64_t max_block)
{
	Impl& a = *m_i;
	out->clear();
	if (!a.open || i >= a.entries.size() || a.entries[i].dir)
		return Status::Damaged;
	const UInt32 folder = a.db.FileToFolder[i];
	if (folder != UInt32(-1) && a.FolderEncrypted(folder))
		return Status::Encrypted; // first: whatever its size, it can't be read
	if (a.entries[i].size > max_entry)
		return Status::TooBig;
	if (folder == UInt32(-1))
		return Status::Ok; // an empty file
	if (SzAr_GetFolderUnpackSize(&a.db.db, folder) > max_block)
		return Status::BlockTooBig;
	clearerr(a.stream.f);
	a.stream.read_error = false;
	UInt32 block = 0xFFFFFFFF;
	Byte* buf = nullptr;
	size_t buf_size = 0, offset = 0, got = 0;
	const SRes r = SzArEx_Extract(&a.db, &a.look.vt, UInt32(i), &block, &buf, &buf_size, &offset, &got, &a.budget.vt, &a.budget.vt);
	if (r == SZ_OK && got == a.entries[i].size)
		out->assign(buf + offset, buf + offset + got);
	a.budget.vt.Free(&a.budget.vt, buf);
	if (r != SZ_OK)
		return a.Map(r);
	return got == a.entries[i].size ? Status::Ok : Status::Damaged;
}
} // namespace sevenzip
} // namespace fe
