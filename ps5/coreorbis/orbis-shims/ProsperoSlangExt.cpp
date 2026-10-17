// Genesis Plus GX PS5: the GPU's shader list, the built-in presets (compiled at build time, ProsperoSlang.h) and the
// user's own .slangp / .slang files from <root>/shaders, compiled here on the console the first time they are used.
//
// What tools/slang_compile.py does at build time, done at run time: the preset's key = value lines (#reference
// followed), each pass's .slang with its #includes inlined, split into its vertex and fragment stages, its
// #pragma parameter / name / format lines read, both stages compiled to SPIR-V by glslang (third_party/glslang), the
// SPIR-V reflected (uniform block and push-constant members, sampled textures), the LUT images read. The result is a
// ps5slang::Preset like the built-in ones, so ProsperoGpu.cpp runs both the same way.
//
// A user preset is listed at start (its name is its path under shaders/, without the extension) and compiled by the
// GPU renderer's build thread when it is picked; a preset that does not compile says why in boot.log and stays the
// plain picture.
//
// SPDX-License-Identifier: MIT

#include "ProsperoSlang.h"

#if defined(GENPLUS_GPU) || !defined(__PROSPERO__)

#include "OrbisPaths.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "SPIRV/GlslangToSpv.h"
#include "glslang/Public/ResourceLimits.h"
#include "glslang/Public/ShaderLang.h"

namespace ps5slang
{
namespace
{
// ---- a compiled user preset: everything the Preset points to, owned here -------------------------------------------
struct Owned
{
	Preset preset = {};
	std::string name;
	std::vector<Pass> passes;
	std::vector<std::vector<uint32_t>> spirv;
	std::vector<std::vector<Member>> members;
	std::vector<std::vector<Sampler>> samplers;
	std::vector<Param> params;
	std::vector<Lut> luts;
	std::vector<std::vector<uint8_t>> lut_data;
	std::deque<std::string> strings; // the names the tables point to (a deque: its strings never move)

	const char* Str(const std::string& s)
	{
		strings.push_back(s);
		return strings.back().c_str();
	}
};

struct External
{
	std::string name; // "crt/crt-royale" (its path under shaders/, no extension)
	std::string path;
	Preset stub = {};                         // the name only, until compiled
	std::atomic<const Preset*> cur{nullptr}; // &stub, then the compiled preset
	std::unique_ptr<Owned> owned;
	bool tried = false;
};

std::mutex g_mx;         // the scan
std::mutex g_compile_mx; // one compile at a time (the build thread), and each External's owned / tried
std::deque<External> g_ext; // filled once by ScanExternal, before the list is read: never changed after
std::atomic<int> g_ext_count{0};
bool g_scanned = false;

// ---- the preset file ----------------------------------------------------------------------------------------------
std::string Trim(const std::string& s)
{
	size_t a = 0, b = s.size();
	while (a < b && isspace(static_cast<unsigned char>(s[a])))
		a++;
	while (b > a && isspace(static_cast<unsigned char>(s[b - 1])))
		b--;
	return s.substr(a, b - a);
}

std::string Dir(const std::string& path)
{
	const size_t s = path.rfind('/');
	return s == std::string::npos ? "." : path.substr(0, s);
}

// a/b/../c -> a/c
std::string Normalize(const std::string& path)
{
	std::vector<std::string> parts;
	size_t pos = 0;
	while (pos <= path.size())
	{
		size_t s = path.find('/', pos);
		if (s == std::string::npos)
			s = path.size();
		const std::string p = path.substr(pos, s - pos);
		if (p == "..")
		{
			if (!parts.empty() && parts.back() != "..")
				parts.pop_back();
			else
				parts.push_back(p);
		}
		else if (!p.empty() && p != ".")
			parts.push_back(p);
		pos = s + 1;
	}
	std::string out = path.size() && path[0] == '/' ? "/" : "";
	for (size_t i = 0; i < parts.size(); i++)
		out += (i ? "/" : "") + parts[i];
	return out;
}

std::string Join(const std::string& base, const std::string& rel)
{
	return Normalize(!rel.empty() && rel[0] == '/' ? rel : base + "/" + rel);
}

bool ReadFile(const std::string& path, std::string& out)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	char buf[65536];
	size_t n;
	out.clear();
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		out.append(buf, n);
	fclose(f);
	return true;
}

std::vector<std::string> Lines(const std::string& text)
{
	std::vector<std::string> out;
	size_t pos = 0;
	while (pos <= text.size())
	{
		size_t e = text.find('\n', pos);
		if (e == std::string::npos)
			e = text.size();
		std::string l = text.substr(pos, e - pos);
		if (!l.empty() && l.back() == '\r')
			l.pop_back();
		out.push_back(l);
		pos = e + 1;
	}
	return out;
}

bool ParseKv(const std::string& path, std::map<std::string, std::string>& out, std::string& err, int depth = 0)
{
	if (depth > 16)
	{
		err = "#reference loop at " + path;
		return false;
	}
	std::string text;
	if (!ReadFile(path, text))
	{
		err = "can't read " + path;
		return false;
	}
	const std::string base = Dir(path);
	for (const std::string& line : Lines(text))
	{
		const std::string s = Trim(line);
		if (s.compare(0, 10, "#reference") == 0)
		{
			std::string ref = Trim(s.substr(10));
			if (!ref.empty() && ref[0] == '"')
				ref = ref.substr(1, ref.find('"', 1) == std::string::npos ? std::string::npos : ref.find('"', 1) - 1);
			std::map<std::string, std::string> sub;
			if (!ParseKv(Join(base, ref), sub, err, depth + 1))
				return false;
			for (const auto& kv : sub)
				out.insert(kv); // ours over the referenced file's
			continue;
		}
		const size_t eq = s.find('=');
		if (s.empty() || s[0] == '#' || s.compare(0, 2, "//") == 0 || eq == std::string::npos)
			continue;
		const std::string k = Trim(s.substr(0, eq));
		std::string v = Trim(s.substr(eq + 1));
		if (!v.empty() && v[0] == '"')
		{
			const size_t q = v.find('"', 1);
			v = v.substr(1, q == std::string::npos ? std::string::npos : q - 1);
		}
		else
		{
			size_t cut = std::string::npos;
			for (const char* c : {" #", "\t#", "//", " ;", "\t;"})
				cut = std::min(cut, v.find(c));
			v = Trim(v.substr(0, cut));
		}
		// a pass's shader is relative to the file that names it
		if (k.compare(0, 6, "shader") == 0 && k.size() > 6 && isdigit(static_cast<unsigned char>(k[6])))
			v = Join(base, v);
		out[k] = v;
	}
	// the LUTs' paths too
	auto t = out.find("textures");
	if (t != out.end())
	{
		std::string list = t->second;
		size_t pos = 0;
		while (pos <= list.size())
		{
			size_t e = list.find(';', pos);
			if (e == std::string::npos)
				e = list.size();
			const std::string name = Trim(list.substr(pos, e - pos));
			auto it = out.find(name);
			if (!name.empty() && it != out.end() && !it->second.empty() && it->second[0] != '/')
			{
				const std::string p = Join(base, it->second);
				struct stat st;
				if (stat(p.c_str(), &st) == 0)
					it->second = p;
			}
			pos = e + 1;
		}
	}
	return true;
}

bool ReadSource(const std::string& path, std::string& out, std::string& err, int depth = 0)
{
	if (depth > 32)
	{
		err = "#include loop at " + path;
		return false;
	}
	std::string text;
	if (!ReadFile(path, text))
	{
		err = "can't read " + path;
		return false;
	}
	for (const std::string& line : Lines(text))
	{
		const std::string s = Trim(line);
		if (s.compare(0, 8, "#include") == 0)
		{
			const size_t a = s.find('"'), b = a == std::string::npos ? a : s.find('"', a + 1);
			if (b != std::string::npos)
			{
				std::string inc;
				if (!ReadSource(Join(Dir(path), s.substr(a + 1, b - a - 1)), inc, err, depth + 1))
					return false;
				out += inc + "\n";
				continue;
			}
		}
		out += line + "\n";
	}
	return true;
}

// a #pragma parameter number; a malformed one ("0.0.001", seen in the Mega Bezel) reads as far as it can
float Num(const std::string& s)
{
	return strtof(s.c_str(), nullptr);
}

struct Stages
{
	std::string vs, fs, name, format;
	std::vector<Param> params;
	std::vector<std::string> param_names, param_descs;
};

void SplitStages(const std::string& src, Stages& st)
{
	std::vector<std::string> common, vert, frag, head;
	std::vector<std::string>* cur = nullptr;
	for (const std::string& line : Lines(src))
	{
		const std::string s = Trim(line);
		if (s.compare(0, 7, "#pragma") == 0)
		{
			char what[32] = {}, a[256] = {};
			if (sscanf(s.c_str(), "#pragma %31s %255s", what, a) >= 1)
			{
				const std::string w = what;
				if (w == "parameter")
				{
					// #pragma parameter NAME "Description" default min max [step]
					const size_t q1 = s.find('"'), q2 = q1 == std::string::npos ? q1 : s.find('"', q1 + 1);
					if (q2 != std::string::npos)
					{
						char n[128] = {}, v[4][64] = {};
						sscanf(s.c_str(), "#pragma parameter %127s", n);
						const int got = sscanf(s.c_str() + q2 + 1, "%63s %63s %63s %63s", v[0], v[1], v[2], v[3]);
						if (got >= 3)
						{
							st.param_names.push_back(n);
							st.param_descs.push_back(s.substr(q1 + 1, q2 - q1 - 1));
							st.params.push_back({nullptr, nullptr, Num(v[0]), Num(v[1]), Num(v[2]), got >= 4 ? Num(v[3]) : 0.0f});
						}
					}
					continue;
				}
				if (w == "name")
				{
					st.name = a;
					continue;
				}
				if (w == "format")
				{
					st.format = a;
					continue;
				}
				if (w == "stage")
				{
					cur = !strcmp(a, "vertex") ? &vert : &frag;
					continue;
				}
			}
		}
		if (s.compare(0, 8, "#version") == 0 && !cur)
		{
			head.push_back(line);
			continue;
		}
		(cur ? *cur : common).push_back(line);
	}
	if (head.empty())
		head.push_back("#version 450");
	auto join = [](const std::vector<std::string>& a, const std::vector<std::string>& b, const std::vector<std::string>& c) {
		std::string o;
		for (const auto* v : {&a, &b, &c})
			for (const std::string& l : *v)
				o += l + "\n";
		return o;
	};
	st.vs = join(head, common, vert);
	st.fs = join(head, common, frag);
}

// ---- glslang ------------------------------------------------------------------------------------------------------
bool Compile(const std::string& src, bool vertex, std::vector<uint32_t>& spv, std::string& err)
{
	static std::once_flag once;
	std::call_once(once, [] { glslang::InitializeProcess(); });
	const EShLanguage stage = vertex ? EShLangVertex : EShLangFragment;
	glslang::TShader sh(stage);
	const char* s = src.c_str();
	sh.setStrings(&s, 1);
	sh.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
	sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
	sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
	const EShMessages msg = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
	if (!sh.parse(GetDefaultResources(), 450, false, msg))
	{
		err = std::string(vertex ? "vertex" : "fragment") + " stage: " + sh.getInfoLog();
		return false;
	}
	glslang::TProgram prog;
	prog.addShader(&sh);
	if (!prog.link(msg))
	{
		err = std::string("link: ") + prog.getInfoLog();
		return false;
	}
	spv.clear();
	glslang::SpvOptions opt;
	opt.disableOptimizer = true;
	glslang::GlslangToSpv(*prog.getIntermediate(stage), spv, &opt);
	return !spv.empty();
}

// ---- SPIR-V reflection (tools/slang_compile.py's reflect) ---------------------------------------------------------
struct Block
{
	bool present = false;
	std::vector<std::pair<std::string, std::pair<int, int>>> members; // name, (offset, kind)
	int size = 0, binding = 0;
};
struct Reflection
{
	Block ubo, push;
	std::map<std::string, int> samplers;
};

std::string LitStr(const uint32_t* w, size_t n)
{
	std::string s;
	for (size_t i = 0; i < n; i++)
		for (int b = 0; b < 4; b++)
		{
			const char c = char((w[i] >> (8 * b)) & 0xff);
			if (!c)
				return s;
			s += c;
		}
	return s;
}

bool Reflect(const std::vector<uint32_t>& spv, Reflection& r)
{
	if (spv.size() < 5 || spv[0] != 0x07230203)
		return false;
	std::map<uint32_t, std::string> names;
	std::map<std::pair<uint32_t, uint32_t>, std::string> member_names;
	std::map<uint32_t, std::map<uint32_t, uint32_t>> decos;
	std::map<std::pair<uint32_t, uint32_t>, std::map<uint32_t, uint32_t>> member_decos;
	std::map<uint32_t, std::pair<uint32_t, std::vector<uint32_t>>> types;
	std::map<uint32_t, std::pair<uint32_t, uint32_t>> pointers;
	std::vector<std::array<uint32_t, 3>> variables;
	for (size_t i = 5; i < spv.size();)
	{
		const uint32_t wc = spv[i] >> 16, op = spv[i] & 0xffff;
		if (wc == 0 || i + wc > spv.size())
			return false;
		const uint32_t* o = &spv[i + 1];
		const size_t n = wc - 1;
		switch (op)
		{
			case 5: if (n >= 1) names[o[0]] = LitStr(o + 1, n - 1); break;
			case 6: if (n >= 2) member_names[{o[0], o[1]}] = LitStr(o + 2, n - 2); break;
			case 71: if (n >= 2) decos[o[0]][o[1]] = n >= 3 ? o[2] : 0; break;
			case 72: if (n >= 3) member_decos[{o[0], o[1]}][o[2]] = n >= 4 ? o[3] : 0; break;
			case 19: case 20: case 21: case 22: case 23: case 24: case 25: case 26: case 27: case 28: case 30:
				if (n >= 1)
					types[o[0]] = {op, std::vector<uint32_t>(o + 1, o + n)};
				break;
			case 32: if (n >= 3) pointers[o[0]] = {o[1], o[2]}; break;
			case 59: if (n >= 3) variables.push_back({o[0], o[1], o[2]}); break;
			default: break;
		}
		i += wc;
	}
	auto kind = [&](uint32_t t) -> int {
		auto it = types.find(t);
		if (it == types.end())
			return 7;
		const uint32_t op = it->second.first;
		const std::vector<uint32_t>& a = it->second.second;
		if (op == 22)
			return 0;
		if (op == 21)
			return a.size() >= 2 && a[1] ? 1 : 2;
		if (op == 23 && a.size() >= 2)
		{
			auto e = types.find(a[0]);
			if (e != types.end() && e->second.first == 22 && a[1] >= 2 && a[1] <= 4)
				return 3 + int(a[1]) - 2;
			return 7;
		}
		if (op == 24 && a.size() >= 2 && a[1] == 4)
			return 6;
		return 7;
	};
	static const int kSize[] = {4, 4, 4, 8, 12, 16, 64, 16};
	for (const auto& v : variables)
	{
		const uint32_t ptype = v[0], var = v[1], storage = v[2];
		auto p = pointers.find(ptype);
		if (p == pointers.end())
			continue;
		const uint32_t pointee = p->second.second;
		auto t = types.find(pointee);
		if (t == types.end())
			continue;
		if ((storage == 2 || storage == 9) && t->second.first == 30)
		{
			Block b;
			b.present = true;
			int size = 0;
			for (uint32_t m = 0; m < t->second.second.size(); m++)
			{
				const auto md = member_decos.find({pointee, m});
				int off = 0;
				if (md != member_decos.end() && md->second.count(35))
					off = int(md->second.at(35));
				const int k = kind(t->second.second[m]);
				const auto mn = member_names.find({pointee, m});
				b.members.push_back({mn != member_names.end() ? mn->second : "", {off, k}});
				size = std::max(size, off + kSize[k]);
			}
			b.size = (size + 15) & ~15;
			const auto d = decos.find(var);
			b.binding = d != decos.end() && d->second.count(33) ? int(d->second.at(33)) : 0;
			(storage == 2 ? r.ubo : r.push) = b;
		}
		else if (storage == 0 && t->second.first == 27)
		{
			const auto d = decos.find(var);
			r.samplers[names.count(var) ? names[var] : ""] = d != decos.end() && d->second.count(33) ? int(d->second.at(33)) : 0;
		}
	}
	return true;
}

int Scale(const std::string& s)
{
	return s == "viewport" ? 1 : s == "absolute" ? 2 : 0;
}

int Wrap(const std::string& s)
{
	if (s == "clamp_to_border")
		return 0;
	if (s == "repeat")
		return 2;
	if (s == "mirrored_repeat")
		return 3;
	return 1;
}

std::string Get(const std::map<std::string, std::string>& kv, const std::string& k, const std::string& def = "")
{
	auto it = kv.find(k);
	return it == kv.end() ? def : it->second;
}

bool Has(const std::map<std::string, std::string>& kv, const std::string& k)
{
	return kv.count(k) != 0;
}

std::unique_ptr<Owned> Load(const std::string& name, const std::string& path, std::string& err)
{
	std::map<std::string, std::string> kv;
	const bool single = path.size() > 6 && path.compare(path.size() - 6, 6, ".slang") == 0;
	if (single)
		kv = {{"shaders", "1"}, {"shader0", path}, {"filter_linear0", "false"}, {"scale_type0", "viewport"}};
	else if (!ParseKv(path, kv, err))
		return nullptr;
	const int npass = atoi(Get(kv, "shaders", "0").c_str());
	if (npass <= 0 || npass > 64)
	{
		err = "no passes (shaders = " + Get(kv, "shaders") + ")";
		return nullptr;
	}
	auto o = std::make_unique<Owned>();
	o->name = name;
	o->passes.resize(size_t(npass));
	o->spirv.resize(size_t(npass) * 2);
	o->members.resize(size_t(npass) * 2);
	o->samplers.resize(size_t(npass));
	std::vector<std::string> pnames, pdescs;
	std::vector<Param> params;
	for (int p = 0; p < npass; p++)
	{
		const std::string ps = std::to_string(p);
		std::string src;
		if (!ReadSource(Get(kv, "shader" + ps), src, err))
			return nullptr;
		Stages st;
		SplitStages(src, st);
		std::vector<uint32_t>& vs = o->spirv[size_t(p) * 2];
		std::vector<uint32_t>& fs = o->spirv[size_t(p) * 2 + 1];
		if (!Compile(st.vs, true, vs, err) || !Compile(st.fs, false, fs, err))
		{
			err = "pass " + ps + " (" + Get(kv, "shader" + ps) + "): " + err;
			return nullptr;
		}
		Reflection rv, rf;
		if (!Reflect(vs, rv) || !Reflect(fs, rf))
		{
			err = "pass " + ps + ": unreadable SPIR-V";
			return nullptr;
		}
		Block ubo = rv.ubo.present ? rv.ubo : rf.ubo;
		Block push = rv.push.present ? rv.push : rf.push;
		// a member used in only one stage: the union of both
		for (int which = 0; which < 2; which++)
		{
			Block& a = which ? push : ubo;
			const Block& b = which ? rf.push : rf.ubo;
			const Block& av = which ? rv.push : rv.ubo;
			if (av.present && b.present)
			{
				for (const auto& m : b.members)
				{
					bool dup = false;
					for (const auto& x : a.members)
						dup |= x.first == m.first;
					if (!dup)
						a.members.push_back(m);
				}
				a.size = std::max(a.size, b.size);
			}
		}
		std::map<std::string, int> smp = rv.samplers;
		for (const auto& s : rf.samplers)
			smp[s.first] = s.second;
		for (size_t i = 0; i < st.params.size(); i++)
			if (std::find(pnames.begin(), pnames.end(), st.param_names[i]) == pnames.end())
			{
				pnames.push_back(st.param_names[i]);
				pdescs.push_back(st.param_descs[i]);
				params.push_back(st.params[i]);
			}

		Pass& d = o->passes[size_t(p)];
		d = {};
		d.vs = vs.data();
		d.vs_bytes = unsigned(vs.size() * 4);
		d.fs = fs.data();
		d.fs_bytes = unsigned(fs.size() * 4);
		for (int which = 0; which < 2; which++)
		{
			const Block& b = which ? push : ubo;
			std::vector<Member>& mv = o->members[size_t(p) * 2 + size_t(which)];
			for (const auto& m : b.members)
				mv.push_back({o->Str(m.first), m.second.first, m.second.second});
			if (which == 0)
			{
				d.ubo = b.present ? mv.data() : nullptr;
				d.ubo_count = b.present ? int(mv.size()) : 0;
				d.ubo_size = b.present ? b.size : 0;
				d.ubo_binding = b.present ? b.binding : -1;
			}
			else
			{
				d.push = b.present ? mv.data() : nullptr;
				d.push_count = b.present ? int(mv.size()) : 0;
				d.push_size = b.present ? b.size : 0;
			}
		}
		std::vector<std::pair<int, std::string>> bysort;
		for (const auto& s : smp)
			bysort.push_back({s.second, s.first});
		std::sort(bysort.begin(), bysort.end());
		for (const auto& s : bysort)
			o->samplers[size_t(p)].push_back({o->Str(s.second), s.first});
		d.samplers = o->samplers[size_t(p)].data();
		d.sampler_count = int(o->samplers[size_t(p)].size());
		std::string alias = Get(kv, "alias" + ps);
		if (alias.empty())
			alias = st.name;
		d.alias = o->Str(alias);
		d.framebuffer = 0;
		if (Get(kv, "float_framebuffer" + ps) == "true" || st.format.find("SFLOAT") != std::string::npos)
			d.framebuffer = 2;
		else if (Get(kv, "srgb_framebuffer" + ps) == "true" || st.format.find("SRGB") != std::string::npos)
			d.framebuffer = 1;
		const bool last = p == npass - 1;
		const std::string stt = Get(kv, "scale_type" + ps);
		const std::string def_type = !stt.empty() ? stt : (last ? "viewport" : "source");
		d.scale_type_x = Scale(Get(kv, "scale_type_x" + ps, def_type));
		d.scale_type_y = Scale(Get(kv, "scale_type_y" + ps, def_type));
		const std::string sc = Get(kv, "scale" + ps, "1.0");
		d.scale_x = Num(Get(kv, "scale_x" + ps, sc));
		d.scale_y = Num(Get(kv, "scale_y" + ps, sc));
		d.linear = Get(kv, "filter_linear" + ps) == "true";
		d.wrap = Wrap(Get(kv, "wrap_mode" + ps, "clamp_to_edge"));
		d.frame_count_mod = atoi(Get(kv, "frame_count_mod" + ps, "0").c_str());
		d.mipmap_input = Get(kv, "mipmap_input" + ps) == "true";
	}
	// the preset's own parameter values
	for (size_t i = 0; i < params.size(); i++)
	{
		if (Has(kv, pnames[i]))
		{
			const std::string& v = kv[pnames[i]];
			char* end = nullptr;
			const float f = strtof(v.c_str(), &end);
			if (end != v.c_str())
				params[i].value = f;
		}
		params[i].name = o->Str(pnames[i]);
		params[i].desc = o->Str(pdescs[i]);
	}
	o->params = params;
	// the look-up textures (PNG files, decoded when the chain is built)
	std::string list = Get(kv, "textures");
	size_t pos = 0;
	std::vector<std::string> lut_names;
	while (pos <= list.size())
	{
		size_t e = list.find(';', pos);
		if (e == std::string::npos)
			e = list.size();
		const std::string t = Trim(list.substr(pos, e - pos));
		if (!t.empty())
			lut_names.push_back(t);
		pos = e + 1;
	}
	o->lut_data.resize(lut_names.size());
	for (size_t i = 0; i < lut_names.size(); i++)
	{
		std::string data;
		if (!ReadFile(Get(kv, lut_names[i]), data))
		{
			err = "can't read the texture " + lut_names[i] + " (" + Get(kv, lut_names[i]) + ")";
			return nullptr;
		}
		o->lut_data[i].assign(data.begin(), data.end());
		o->luts.push_back({o->Str(lut_names[i]), o->lut_data[i].data(), unsigned(o->lut_data[i].size()),
			Get(kv, lut_names[i] + "_linear") == "true", Wrap(Get(kv, lut_names[i] + "_wrap_mode", "clamp_to_edge")),
			Get(kv, lut_names[i] + "_mipmap") == "true"});
	}
	Preset& pr = o->preset;
	pr.name = o->Str(name);
	pr.passes = o->passes.data();
	pr.pass_count = npass;
	pr.params = o->params.empty() ? nullptr : o->params.data();
	pr.param_count = int(o->params.size());
	pr.luts = o->luts.empty() ? nullptr : o->luts.data();
	pr.lut_count = int(o->luts.size());
	pr.fullscreen = false;
	return o;
}

// ---- the folder ---------------------------------------------------------------------------------------------------
constexpr int kMaxExternal = 1024;
constexpr int kMaxDepth = 5;

void Scan(const std::string& root, const std::string& rel, int depth, std::vector<std::pair<std::string, std::string>>& out)
{
	if (depth > kMaxDepth || int(out.size()) >= kMaxExternal * 4)
		return;
	const std::string dir = rel.empty() ? root : root + "/" + rel;
	DIR* d = opendir(dir.c_str());
	if (!d)
		return;
	std::vector<std::string> names;
	while (dirent* e = readdir(d))
		if (e->d_name[0] != '.')
			names.push_back(e->d_name);
	closedir(d);
	std::sort(names.begin(), names.end());
	for (const std::string& n : names)
	{
		const std::string full = dir + "/" + n, r = rel.empty() ? n : rel + "/" + n;
		struct stat st;
		if (stat(full.c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
			Scan(root, r, depth + 1, out);
		else if (n.size() > 7 && n.compare(n.size() - 7, 7, ".slangp") == 0)
			out.push_back({r.substr(0, r.size() - 7), full});
		// a lone .slang at the top only: below, .slang files are the presets' passes
		else if (rel.empty() && n.size() > 6 && n.compare(n.size() - 6, 6, ".slang") == 0)
			out.push_back({r.substr(0, r.size() - 6), full});
	}
}
} // namespace

bool CompileGlsl(const std::string& src, bool vertex, std::vector<uint32_t>& spv, std::string& err)
{
	std::lock_guard<std::mutex> lk(g_compile_mx);
	return Compile(src, vertex, spv, err);
}

std::string ExternalDir()
{
	return OrbisRoot() + "/shaders";
}

void ScanExternal()
{
	std::lock_guard<std::mutex> lk(g_mx);
	if (g_scanned)
		return;
	g_scanned = true;
	std::vector<std::pair<std::string, std::string>> found;
	Scan(ExternalDir(), "", 0, found);
	if (int(found.size()) > kMaxExternal)
	{
		OrbisLog("[gpu] %s: %d presets, the first %d listed", ExternalDir().c_str(), int(found.size()), kMaxExternal);
		found.resize(kMaxExternal);
	}
	for (const auto& f : found)
	{
		g_ext.emplace_back();
		External& e = g_ext.back();
		e.name = f.first;
		e.path = f.second;
		e.stub.name = e.name.c_str();
		e.cur = &e.stub;
	}
	g_ext_count = int(g_ext.size());
	if (!found.empty())
		OrbisLog("[gpu] %d preset(s) of your own in %s (compiled when picked)", int(found.size()), ExternalDir().c_str());
}

#ifndef __PROSPERO__
// host tests: GENPLUS_HOST_SLANG_CHECK=1 compiles every preset found, now (boot.log says which ones failed)
void CheckAllExternal()
{
	if (!getenv("GENPLUS_HOST_SLANG_CHECK"))
		return;
	int ok = 0;
	for (int i = 0; i < g_ext_count.load(); i++)
		ok += EnsureCompiled(kBuiltinPresetCount + i, nullptr) ? 1 : 0;
	OrbisLog("[gpu] slang check: %d of %d preset(s) of your own compiled", ok, g_ext_count.load());
}
#endif

int PresetCount()
{
	return kBuiltinPresetCount + g_ext_count.load();
}

const Preset& PresetAt(int i)
{
	if (i < kBuiltinPresetCount)
		return kBuiltinPresets[i < 0 ? 0 : i];
	const size_t e = size_t(i - kBuiltinPresetCount);
	if (int(e) >= g_ext_count.load())
		return kBuiltinPresets[0];
	return *g_ext[e].cur.load();
}

bool IsExternal(int i)
{
	return i >= kBuiltinPresetCount;
}

bool EnsureCompiled(int i, std::string* err)
{
	if (i < kBuiltinPresetCount)
		return true;
	std::lock_guard<std::mutex> lk(g_compile_mx);
	const size_t idx = size_t(i - kBuiltinPresetCount);
	if (int(idx) >= g_ext_count.load())
		return false;
	External& e = g_ext[idx];
	if (e.owned)
		return true;
	if (e.tried)
	{
		if (err)
			*err = "it did not compile (see above)";
		return false;
	}
	e.tried = true;
	std::string why;
	auto o = Load(e.name, e.path, why);
	if (!o)
	{
		OrbisLog("[gpu] your preset %s (%s) could not be used: %s", e.name.c_str(), e.path.c_str(), why.c_str());
		if (err)
			*err = why;
		return false;
	}
	e.owned = std::move(o);
	e.cur = &e.owned->preset;
	OrbisLog("[gpu] your preset %s: %d pass(es), %d parameter(s), %d texture(s), compiled", e.name.c_str(),
		e.owned->preset.pass_count, e.owned->preset.param_count, e.owned->preset.lut_count);
	return true;
}
} // namespace ps5slang

#endif
