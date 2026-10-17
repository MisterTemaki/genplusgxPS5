#!/usr/bin/env python3
"""Compiles RetroArch slang shader presets (.slangp, or a single .slang) for the PS5 port's GPU renderer.

    slang_compile.py <out.cpp> <name>=<preset>[@<param>=<value>,...] ...
    slang_compile.py <out.cpp> --list <presets.txt>

For each preset: every pass's .slang is split into its vertex and fragment stages (`#pragma stage`), its
`#include`s resolved, its `#pragma parameter`s read, both stages compiled to SPIR-V with glslangValidator, and the
SPIR-V reflected here (no SPIRV-Cross needed): the uniform block and push-constant members (by name, with their
offsets and types), the sampled textures (by name, with their bindings). The output is a C++ file of tables that
ProsperoSlang.cpp runs: no shader compiler on the console. The preset's look-up textures (PNG) are embedded too.

What a preset may use is what RetroArch's slang spec defines and ProsperoSlang.cpp implements: the passes' scale
(source / viewport / absolute), filter, wrap mode, 8-bit / sRGB / float framebuffers, aliases, frame_count_mod; the
semantics MVP, OutputSize, SourceSize, OriginalSize, FinalViewportSize, FrameCount, FrameDirection, Rotation,
<texture>Size; the textures Original, Source, OriginalHistory#, PassOutput#, PassFeedback#, aliases (and their
Feedback), LUTs; the parameters, with the preset's values.

SPDX-License-Identifier: MIT
"""
import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile

# ---------------------------------------------------------------------------------------------------- presets


def parse_kv(path):
    """key = value lines of a .slangp, with #reference followed (its keys first, ours over them)"""
    out = {}
    base = os.path.dirname(path)
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    for line in lines:
        s = line.strip()
        m = re.match(r'#reference\s+"?([^"]+)"?', s)
        if m:
            ref = os.path.normpath(os.path.join(base, m.group(1)))
            for k, v in parse_kv(ref).items():
                out.setdefault(k, v)
            continue
        if not s or s.startswith("#") or s.startswith("//") or "=" not in s:
            continue
        k, v = s.split("=", 1)
        k = k.strip()
        v = v.strip()
        if v.startswith('"'):
            v = v[1:].split('"', 1)[0]
        else:
            v = re.split(r"\s+#|//|\s+;", v)[0].strip()
        # paths are relative to the file that names them
        if (re.match(r"shader\d+$", k)) and not os.path.isabs(v):
            v = os.path.normpath(os.path.join(base, v))
        out[k] = v
    # LUT paths: relative to the file too
    if "textures" in out:
        for t in out["textures"].split(";"):
            t = t.strip()
            if t and t in out and not os.path.isabs(out[t]) and not out[t].startswith("/"):
                p = os.path.normpath(os.path.join(base, out[t]))
                if os.path.exists(p):
                    out[t] = p
    return out


def read_source(path, seen=None):
    """the .slang with its #includes inlined"""
    seen = seen or []
    if path in seen:
        raise SystemExit("include loop: " + path)
    out = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f.read().splitlines():
            m = re.match(r'\s*#include\s+"([^"]+)"', line)
            if m:
                out.append(read_source(os.path.normpath(os.path.join(os.path.dirname(path), m.group(1))),
                                       seen + [path]))
            else:
                out.append(line)
    return "\n".join(out)


def num(text):
    """a #pragma parameter number; a malformed one ("0.0.001", seen in the Mega Bezel) reads as far as it can"""
    m = re.match(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", text)
    return float(m.group(0)) if m else 0.0


def split_stages(src):
    params = []
    name = None
    fmt = None
    common, vert, frag = [], [], []
    cur = None
    for line in src.splitlines():
        s = line.strip()
        m = re.match(r'#pragma\s+parameter\s+(\w+)\s+"([^"]*)"\s+([-\d.eE+]+)\s+([-\d.eE+]+)\s+([-\d.eE+]+)(?:\s+([-\d.eE+]+))?', s)
        if m:
            params.append((m.group(1), m.group(2), num(m.group(3)), num(m.group(4)), num(m.group(5)),
                           num(m.group(6) or "0")))
            continue
        m = re.match(r"#pragma\s+name\s+(\w+)", s)
        if m:
            name = m.group(1)
            continue
        m = re.match(r"#pragma\s+format\s+(\w+)", s)
        if m:
            fmt = m.group(1)
            continue
        m = re.match(r"#pragma\s+stage\s+(\w+)", s)
        if m:
            cur = vert if m.group(1) == "vertex" else frag
            continue
        (cur if cur is not None else common).append(line)
    # the #version line must stay first
    head = [l for l in common if l.strip().startswith("#version")] or ["#version 450"]
    body = [l for l in common if not l.strip().startswith("#version")]
    v = "\n".join(head + body + vert) + "\n"
    f = "\n".join(head + body + frag) + "\n"
    return v, f, params, name, fmt


def compile_stage(source, stage):
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "s." + stage)
        out = os.path.join(d, "s.spv")
        open(src, "w").write(source)
        r = subprocess.run(["glslangValidator", "-V", "--target-env", "vulkan1.0", "-o", out, src],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit("glslang failed (%s):\n%s" % (stage, r.stdout + r.stderr))
        return open(out, "rb").read()

# ---------------------------------------------------------------------------------------------------- SPIR-V


def reflect(spv):
    words = struct.unpack("<%dI" % (len(spv) // 4), spv)
    assert words[0] == 0x07230203
    names, member_names, decos, member_decos = {}, {}, {}, {}
    types, pointers, variables = {}, {}, []
    i = 5
    while i < len(words):
        wc, op = words[i] >> 16, words[i] & 0xFFFF
        ops = words[i + 1:i + wc]
        if op == 5:  # OpName
            names[ops[0]] = lit_str(ops[1:])
        elif op == 6:  # OpMemberName
            member_names[(ops[0], ops[1])] = lit_str(ops[2:])
        elif op == 71:  # OpDecorate
            decos.setdefault(ops[0], {})[ops[1]] = ops[2:]
        elif op == 72:  # OpMemberDecorate
            member_decos.setdefault((ops[0], ops[1]), {})[ops[2]] = ops[3:]
        elif op in (19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 30):
            types[ops[0]] = (op, ops[1:])
        elif op == 32:  # OpTypePointer
            pointers[ops[0]] = (ops[1], ops[2])
        elif op == 59:  # OpVariable
            variables.append((ops[0], ops[1], ops[2]))
        i += wc

    def type_kind(t):
        op, a = types[t]
        if op == 22:
            return "float"
        if op == 21:
            return "int" if a[1] else "uint"
        if op == 23:
            return types[a[0]][0] == 22 and "vec%d" % a[1] or "ivec%d" % a[1]
        if op == 24:
            return "mat%d" % a[1]
        return "other"

    result = {"ubo": None, "push": None, "samplers": {}}
    for ptype, var, storage in variables:
        if ptype not in pointers:
            continue
        _, pointee = pointers[ptype]
        if storage in (2, 9) and pointee in types and types[pointee][0] == 30:
            members = []
            for m, mt in enumerate(types[pointee][1]):
                off = member_decos.get((pointee, m), {}).get(35, [0])[0]
                members.append((member_names.get((pointee, m), ""), off, type_kind(mt)))
            size = 0
            for n, off, k in members:
                size = max(size, off + {"float": 4, "int": 4, "uint": 4, "vec2": 8, "vec3": 12, "vec4": 16,
                                        "mat4": 64}.get(k, 16))
            block = {"members": members, "size": (size + 15) & ~15,
                     "binding": decos.get(var, {}).get(33, [0])[0]}
            result["ubo" if storage == 2 else "push"] = block
        elif storage == 0 and pointee in types and types[pointee][0] == 27:
            result["samplers"][names.get(var, "")] = decos.get(var, {}).get(33, [0])[0]
    return result


def lit_str(ws):
    b = b"".join(struct.pack("<I", w) for w in ws)
    return b.split(b"\0")[0].decode()

# ---------------------------------------------------------------------------------------------------- output


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def emit_words(out, name, spv):
    words = struct.unpack("<%dI" % (len(spv) // 4), spv)
    out.append("static const uint32_t %s[%d] = {" % (name, len(words)))
    for i in range(0, len(words), 16):
        out.append(",".join("0x%x" % w for w in words[i:i + 16]) + ",")
    out.append("};")


KINDS = {"float": 0, "int": 1, "uint": 2, "vec2": 3, "vec3": 4, "vec4": 5, "mat4": 6}
SCALE = {"source": 0, "viewport": 1, "absolute": 2}
WRAP = {"clamp_to_border": 0, "clamp_to_edge": 1, "repeat": 2, "mirrored_repeat": 3}


def main():
    out_path = sys.argv[1]
    out = ["// Generated by ps5/tools/slang_compile.py: RetroArch slang presets compiled for ProsperoSlang.cpp.",
           "// The shaders keep their own licences (ps5/THIRD_PARTY_SHADERS.md). Do not edit.",
           "#include \"ProsperoSlang.h\"", "", "namespace ps5slang", "{", "namespace", "{"]
    presets = []
    args = sys.argv[2:]
    if args and args[0] == "--list":
        # a list file: "name = preset" lines, paths relative to the list
        base = os.path.dirname(os.path.abspath(args[1]))
        args = []
        for line in open(sys.argv[3], encoding="utf-8"):
            line = line.split("#")[0].strip()
            if "=" in line:
                k, v = [x.strip() for x in line.split("=", 1)]
                args.append("%s=%s" % (k, os.path.join(base, v)))
    for arg in args:
        name, rest = arg.split("=", 1)
        fullscreen = False
        if "|" in rest:
            rest, flags = rest.split("|", 1)
            fullscreen = "fullscreen" in flags
        overrides = {}
        if "@" in rest:
            rest, ov = rest.split("@", 1)
            for kv in ov.split(","):
                k, v = kv.split("=")
                overrides[k] = v
        if rest.endswith(".slang"):
            kv = {"shaders": "1", "shader0": os.path.abspath(rest), "filter_linear0": "false", "scale_type0": "viewport"}
        else:
            kv = parse_kv(rest)
        kv.update(overrides)
        npass = int(kv["shaders"])
        passes = []
        params = {}
        param_order = []
        for p in range(npass):
            src = read_source(kv["shader%d" % p])
            v, f, prm, pname, fmt = split_stages(src)
            vs, fs = compile_stage(v, "vert"), compile_stage(f, "frag")
            rv, rf = reflect(vs), reflect(fs)
            ubo = rv["ubo"] or rf["ubo"]
            push = rv["push"] or rf["push"]
            # a member used in only one stage: the union of both
            for which in ("ubo", "push"):
                a, b = rv[which], rf[which]
                if a and b:
                    names = {m[0] for m in a["members"]}
                    a["members"] += [m for m in b["members"] if m[0] not in names]
                    a["size"] = max(a["size"], b["size"])
            samplers = dict(rv["samplers"])
            samplers.update(rf["samplers"])
            for pr in prm:
                if pr[0] not in params:
                    params[pr[0]] = pr
                    param_order.append(pr[0])
            alias = kv.get("alias%d" % p, "") or (pname or "")
            fb = 0
            if kv.get("float_framebuffer%d" % p, "false") == "true" or (fmt and "SFLOAT" in fmt):
                fb = 2
            elif kv.get("srgb_framebuffer%d" % p, "false") == "true" or (fmt and "SRGB" in fmt):
                fb = 1
            st = kv.get("scale_type%d" % p)
            stx = kv.get("scale_type_x%d" % p, st or ("viewport" if p == npass - 1 else "source"))
            sty = kv.get("scale_type_y%d" % p, st or ("viewport" if p == npass - 1 else "source"))
            sc = kv.get("scale%d" % p)
            sx = float(kv.get("scale_x%d" % p, sc or "1.0"))
            sy = float(kv.get("scale_y%d" % p, sc or "1.0"))
            lin = kv.get("filter_linear%d" % p, "false") == "true"
            wrap = WRAP.get(kv.get("wrap_mode%d" % p, "clamp_to_edge"), 1)
            fcm = int(kv.get("frame_count_mod%d" % p, "0"))
            last_unscaled = p == npass - 1 and (st is None and "scale_type_x%d" % p not in kv)
            passes.append(dict(vs=vs, fs=fs, ubo=ubo, push=push, samplers=samplers, alias=alias, fb=fb,
                               stx=SCALE[stx], sty=SCALE[sty], sx=sx, sy=sy, lin=lin, wrap=wrap, fcm=fcm,
                               mip=kv.get("mipmap_input%d" % p, "false") == "true"))
        # the preset's own parameter values
        for k in list(params):
            if k in kv:
                try:
                    pr = params[k]
                    params[k] = (pr[0], pr[1], float(kv[k]), pr[3], pr[4], pr[5])
                except ValueError:
                    pass
        luts = []
        for t in [t.strip() for t in kv.get("textures", "").split(";") if t.strip()]:
            data = open(kv[t], "rb").read()
            luts.append(dict(name=t, data=data, lin=kv.get(t + "_linear", "false") == "true",
                             wrap=WRAP.get(kv.get(t + "_wrap_mode", "clamp_to_edge"), 1),
                             mip=kv.get(t + "_mipmap", "false") == "true"))
        presets.append((name, passes, [params[k] for k in param_order], luts, fullscreen))

    lut_arrays = {}  # the same texture in several presets is stored once
    for pi, (name, passes, params, luts, fullscreen) in enumerate(presets):
        for i, p in enumerate(passes):
            emit_words(out, "p%d_vs%d" % (pi, i), p["vs"])
            emit_words(out, "p%d_fs%d" % (pi, i), p["fs"])
            for which in ("ubo", "push"):
                blk = p[which]
                if blk:
                    out.append("static const Member p%d_%s%d[] = {%s};" % (pi, which, i, ", ".join(
                        "{%s, %d, %d}" % (c_str(n), off, KINDS.get(k, 7)) for n, off, k in blk["members"])))
            out.append("static const Sampler p%d_smp%d[] = {%s};" % (pi, i, ", ".join(
                "{%s, %d}" % (c_str(n), b) for n, b in sorted(p["samplers"].items(), key=lambda x: x[1])) or
                "{nullptr, 0}"))
        for li, l in enumerate(luts):
            d = l["data"]
            key = hashlib.sha256(d).hexdigest()
            if key not in lut_arrays:
                arr = "lut_%d" % len(lut_arrays)
                lut_arrays[key] = arr
                out.append("static const uint8_t %s[%d] = {" % (arr, len(d)))
                for j in range(0, len(d), 40):
                    out.append(",".join(str(b) for b in d[j:j + 40]) + ",")
                out.append("};")
            l["array"] = lut_arrays[key]
        out.append("static const Pass p%d_passes[] = {" % pi)
        for i, p in enumerate(passes):
            u, pu = p["ubo"], p["push"]
            out.append("\t{p%d_vs%d, sizeof(p%d_vs%d), p%d_fs%d, sizeof(p%d_fs%d)," % (pi, i, pi, i, pi, i, pi, i))
            out.append("\t\t%s, %d, %d, %d, %s, %d, %d," % (
                "p%d_ubo%d" % (pi, i) if u else "nullptr", len(u["members"]) if u else 0, u["size"] if u else 0,
                u["binding"] if u else -1, "p%d_push%d" % (pi, i) if pu else "nullptr",
                len(pu["members"]) if pu else 0, pu["size"] if pu else 0))
            out.append("\t\tp%d_smp%d, %d, %s, %d, %d, %d, %ff, %ff, %s, %d, %d, %s}," % (
                pi, i, len(p["samplers"]), c_str(p["alias"]), p["fb"], p["stx"], p["sty"], p["sx"], p["sy"],
                "true" if p["lin"] else "false", p["wrap"], p["fcm"], "true" if p["mip"] else "false"))
        out.append("};")
        out.append("static const Param p%d_params[] = {%s};" % (pi, ", ".join(
            "{%s, %s, %ff, %ff, %ff, %ff}" % (c_str(n), c_str(d), v, lo, hi, st) for n, d, v, lo, hi, st in params)
            or "{nullptr, nullptr, 0, 0, 0, 0}"))
        out.append("static const Lut p%d_luts[] = {%s};" % (pi, ", ".join(
            "{%s, %s, sizeof(%s), %s, %d, %s}" % (c_str(l["name"]), l["array"], l["array"],
                                                                 "true" if l["lin"] else "false", l["wrap"],
                                                                 "true" if l["mip"] else "false")
            for li, l in enumerate(luts)) or "{nullptr, nullptr, 0, false, 0, false}"))
    out.append("} // namespace")
    out.append("")
    out.append("const Preset kBuiltinPresets[] = {")
    for pi, (name, passes, params, luts, fullscreen) in enumerate(presets):
        out.append("\t{%s, p%d_passes, %d, p%d_params, %d, p%d_luts, %d, %s}," % (
            c_str(name), pi, len(passes), pi, len(params), pi, len(luts), "true" if fullscreen else "false"))
    out.append("};")
    out.append("const int kBuiltinPresetCount = %d;" % len(presets))
    out.append("} // namespace ps5slang")
    with open(out_path, "w") as f:
        f.write("\n".join(out) + "\n")
    print("%s: %d presets, %d passes" % (out_path, len(presets), sum(len(p[1]) for p in presets)))


if __name__ == "__main__":
    main()
