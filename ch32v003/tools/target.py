#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: GPL-3.0-or-later
"""target: what the shared C core can be on a chip, measured with that chip's own compiler.

A target is a capability file (targets/<name>.toml): the compiler and its flags, the memory, the audio out and
in, the storage, the screen, the controls, the network. This tool

  new   --cc CC [--cflags=..] NAME   probes a compiler: the ISA from its macros, and from a few test functions
                                     what the hardware does (multiply, 64-bit, float, double) and what becomes a
                                     runtime call; writes targets/NAME.toml with the board's part left to fill in
  probe NAME                         compiles every shared module (and the port's HAL, if it has one) with the
                                     target's compiler: code, data and bss, the state each module keeps (sizeof
                                     on the target's ABI), the HAL functions it calls, the runtime helpers it
                                     needs, and any of those the target's runtime doesn't have
  plan  NAME                         picks the features the capabilities allow and the budget holds, says why for
                                     each one left out, and writes build/targets/NAME/{config.h,target.mk}
  list                               the targets and their plans in one table
  check                              every target's plan against its [expect] (engine, build, fits): the
                                     regression check when a module grows or a toolchain changes

Sizes are per object before linking (an upper bound: --gc-sections takes out what nothing calls).
Toolchains: $ARM_GCC, $XPACK_RISCV, $JIELI_TOOLCHAIN, $FM1_DOCK, $PY32_SDK (found in ~/.local/opt,
~/sandbox/thirdparty if unset). llvm-size and llvm-nm read every format here, pi32v2 included.
"""
import argparse, json, os, re, shutil, subprocess, sys, tempfile, tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent            # ch32v003/
TARGETS = ROOT / "targets"
OUT = ROOT / "build" / "targets"
HOME = Path.home()


def first(*paths):
    for p in paths:
        for m in sorted(Path("/").glob(str(p).lstrip("/"))) if "*" in str(p) else [Path(p)]:
            if m.exists():
                return str(m)
    return ""


def defaults():
    env = {
        "ARM_GCC": shutil.which("arm-none-eabi-gcc") or first(HOME / ".local/opt/arm-gnu-toolchain-*/bin/arm-none-eabi-gcc"),
        "XPACK_RISCV": first(HOME / ".local/opt/xpack-riscv-none-elf-gcc-*"),
        "JIELI_TOOLCHAIN": first(HOME / ".local/opt/jieli/jieli-linux-toolchains-*"),
        "FM1_DOCK": first(HOME / "sandbox/thirdparty/fm1-dock"),
        "PY32_SDK": first(HOME / "sandbox/thirdparty/py32f0-template"),
    }
    for k, v in env.items():
        os.environ.setdefault(k, v)


def expand(s):
    # $VAR, ${VAR}, ${VAR:-default}
    def rep(m):
        name, dflt = m.group(1) or m.group(3), m.group(2)
        v = os.environ.get(name, "")
        return v if v else expand(dflt or "")
    return re.sub(r"\$\{(\w+)(?::-([^}]*))?\}|\$(\w+)", rep, s)


def tool(*names):
    for n in names:
        if shutil.which(n):
            return n
    sys.exit(f"target: none of {', '.join(names)} on PATH")


SIZE = None
NM = None


def llvm():
    global SIZE, NM
    SIZE = tool("llvm-size-21", "llvm-size", "llvm-size-19", "llvm-size-18")
    NM = tool("llvm-nm-21", "llvm-nm", "llvm-nm-19", "llvm-nm-18")


# ── the shared modules: what each is, what it keeps, what it needs of the board ──
# role: core (always), engine (one of), storage, feature. ready: drivable by fw/app.c today.
MODULES = {
    "app":     dict(role="core", sources=["fw/app.c", "fw/ui.c", "fw/font.c", "fw/tracks.c", "fw/sd.c", "fw/pff/pff.c"],
                    state={}, desc="the deck: transport, controls, track list, screen, FAT (Petit FatFs)"),
    "lite":    dict(role="engine", sources=["lite/lite.c", "lite/ulaw.c"], header="lite.h", state={"lite_deck": 1},
                    desc="mono 22.05 kHz mu-law streamed through rings; keylock (WSOLA) and varispeed", rank=1),
    "hifi":    dict(role="engine", sources=["hifi/hifi.c"], header="hifi.h", state={"hifi_deck": 1}, ready=False,
                    desc="stereo 16-bit at the track's rate from memory; the C6 engine's keylock", rank=2),
    "memcard": dict(role="storage", sources=["sim/emu/sd_emu.c"], header="sd_emu.h", state={"sd_emu": 1},
                    desc="the tracks as a card image in flash (a clip: tools/memcard.py)"),
    "dvs":     dict(role="feature", sources=["fw/timecode.c", "fw/vinyl.c"], header="vinyl.h",
                    state={"tc_dec": 1, "vinyl_follow": 1}, desc="a timecode record drives the deck (Serato)"),
    "prolink": dict(role="feature", sources=["fw/prolink.c", "fw/linksync.c"], header="prolink.h",
                    extra_headers=["linksync.h"], state={"pl": 1, "ls_peer": 1},
                    desc="Pro DJ Link: on the network as a CDJ, follows and syncs to the master"),
    "udpnet":  dict(role="feature", sources=["fw/udpnet.c"], header="udpnet.h", state={"un": 1},
                    desc="ARP, IPv4 and UDP over raw Ethernet frames (no IP stack)"),
}
INCLUDES = ["fw", "lite", "hifi", "sim/emu"]
HEADER_DIRS = {"lite.h": "lite", "hifi.h": "hifi", "sd_emu.h": "sim/emu"}

# runtime helpers, by what they stand for
HELPERS = [
    ("soft float", re.compile(r"^__(add|sub|mul|div|eq|ne|lt|le|gt|ge|unord|cmp)sf[23]|^__(fix|fixuns)sf|^__float(un)?sisf|^__aeabi_f|^__aeabi_[iu]i?2f")),
    ("double", re.compile(r"df[23]?$|^__aeabi_d|^__extendsfdf2|^__truncdfsf2|^__float(un)?sidf|^__fix(uns)?df")),
    ("64-bit division", re.compile(r"^__(u)?(div|mod)di3$|^__aeabi_[u]?ldivmod")),
    ("64-bit multiply", re.compile(r"^__muldi3$|^__aeabi_lmul$")),
    ("32-bit multiply", re.compile(r"^__mulsi3$")),
    ("32-bit division", re.compile(r"^__(u)?(div|mod)si3$|^__aeabi_[u]?idiv")),
    ("shifts", re.compile(r"^__(ash|lsh)[lr]di3$|^__aeabi_l(lsl|lsr|asr)$")),
    ("memory", re.compile(r"^(memcpy|memset|memmove|memcmp)$|^__aeabi_mem")),
    ("libm", re.compile(r"^(sin|cos|tan|exp|log|pow|sqrt|floor|ceil|round|lrint|fabs|fmod)f?$")),
]


def kind_of(sym):
    for k, rx in HELPERS:
        if rx.search(sym):
            return k
    return None


# ── compiling ──
def compiler(t):
    cc = expand(t["cpu"]["cc"]).split()
    if not cc or not cc[0]:
        return None, f"no compiler: {t['cpu']['cc']} (set the variable)"
    if not (Path(cc[0]).exists() or shutil.which(cc[0])):
        return None, f"compiler not found: {cc[0]}"
    flags = expand(t["cpu"].get("cflags", "")).split()
    return cc + flags + ["-ffreestanding", "-ffunction-sections", "-fdata-sections", "-w"], None


def compile1(cc, src, obj, includes, defines):
    cmd = cc + [f"-I{ROOT / i}" if not os.path.isabs(i) else f"-I{i}" for i in includes] + \
          [f"-D{d}" for d in defines] + ["-c", str(src), "-o", str(obj)]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    return r.returncode == 0, (r.stderr.strip().splitlines() or [""])[0:3]


def sizes(obj):
    r = subprocess.run([SIZE, "-A", str(obj)], capture_output=True, text=True).stdout
    text = data = bss = 0
    for line in r.splitlines():
        p = line.split()
        if len(p) < 2 or not p[1].isdigit():
            continue
        name, n = p[0], int(p[1])
        if name.startswith((".text", ".rodata", ".srodata", ".ram_text")):
            text += n
        elif name.startswith((".data", ".sdata")):
            data += n
        elif name.startswith((".bss", ".sbss", "COMMON", ".tbss")):
            bss += n
    # common symbols (tentative definitions) aren't a section in an object
    for line in subprocess.run([NM, "-S", str(obj)], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in ("C",):
            bss += int(p[1], 16)
    return text, data, bss


def undefined(obj):
    r = subprocess.run([NM, "-u", str(obj)], capture_output=True, text=True).stdout
    return sorted({l.split()[-1] for l in r.splitlines() if l.strip()})


def defined(obj):
    r = subprocess.run([NM, "--defined-only", str(obj)], capture_output=True, text=True).stdout
    return {l.split()[-1] for l in r.splitlines() if len(l.split()) >= 3 and l.split()[1] in "TtDdBbRrCcSsVvWw"}


def state_sizes(cc, types, tmp):
    """sizeof each module's state on the target's ABI: an array of that many bytes, its size read back"""
    headers = sorted({MODULES[m].get("header") for m in MODULES if MODULES[m].get("header")} |
                     {h for m in MODULES.values() for h in m.get("extra_headers", [])})
    src = tmp / "sizes.c"
    lines = [f'#include "{h}"' for h in headers]
    lines += [f"const unsigned char sz_{t}[sizeof({t})] = {{1}};" for t in types]
    src.write_text("\n".join(lines) + "\n")
    ok, err = compile1(cc, src, tmp / "sizes.o", INCLUDES, [])
    if not ok:
        return {}, err
    out = {}
    for line in subprocess.run([NM, "-S", str(tmp / "sizes.o")], capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 4 and p[3].startswith("sz_"):
            out[p[3][3:]] = int(p[1], 16)
    return out, None


def load(name):
    f = TARGETS / f"{name}.toml"
    if not f.exists():
        sys.exit(f"target: no {f.relative_to(ROOT)} (target.py list; target.py new to make one)")
    return tomllib.loads(f.read_text())


def probe(name, quiet=False):
    t = load(name)
    cc, err = compiler(t)
    res = {"target": name, "compiler": err or " ".join(cc[:1]), "modules": {}, "hal": None, "state": {}}
    if err:
        res["error"] = err
        return res
    missing = set(t["cpu"].get("missing", []))
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        for m, spec in MODULES.items():
            r = {"ok": True, "text": 0, "data": 0, "bss": 0, "calls": set(), "defs": set(), "errors": []}
            for src in spec["sources"]:
                obj = tmp / (src.replace("/", "_") + ".o")
                ok, e = compile1(cc, ROOT / src, obj, INCLUDES, [])
                if not ok:
                    r["ok"] = False
                    r["errors"] += [f"{src}: {x}" for x in e]
                    continue
                tx, d, b = sizes(obj)
                r["text"] += tx; r["data"] += d; r["bss"] += b
                r["calls"] |= set(undefined(obj))
                r["defs"] |= defined(obj)
            r["calls"] -= r["defs"]
            res["modules"][m] = r
        hal = t.get("hal")
        if hal:
            r = {"ok": True, "text": 0, "data": 0, "bss": 0, "calls": set(), "defs": set(), "errors": []}
            inc = [expand(i) for i in hal.get("includes", [])] + INCLUDES
            for src in hal["sources"]:
                obj = tmp / (src.replace("/", "_") + ".o")
                ok, e = compile1(cc, ROOT / src, obj, inc, hal.get("defines", []))
                if not ok:
                    r["ok"] = False
                    r["errors"] += [f"{src}: {x}" for x in e]
                    continue
                tx, d, b = sizes(obj)
                r["text"] += tx; r["data"] += d; r["bss"] += b
                r["defs"] |= defined(obj)
                r["calls"] |= set(undefined(obj))
            res["hal"] = r
        types = sorted({ty for s in MODULES.values() for ty in s["state"]})
        res["state"], serr = state_sizes(cc, types, tmp)
        if serr:
            res["state_error"] = serr
    for r in list(res["modules"].values()) + ([res["hal"]] if res["hal"] else []):
        r["helpers"] = sorted(s for s in r["calls"] if kind_of(s))
        r["hal_calls"] = sorted(s for s in r["calls"] if s.startswith("hal_"))
        r["absent"] = sorted(s for s in r["calls"] if s in missing)
        r["calls"] = sorted(r["calls"]); r["defs"] = sorted(r["defs"])
    OUT.joinpath(name).mkdir(parents=True, exist_ok=True)
    (OUT / name / "probe.json").write_text(json.dumps(res, indent=1))
    if not quiet:
        report_probe(t, res)
    return res


def report_probe(t, res):
    print(f"== {t['name']}: {t.get('desc', '')}")
    print(f"   compiler: {res['compiler']}")
    if res.get("error"):
        print(f"   {res['error']}")
        return
    print(f"   {'module':9} {'code':>7} {'data':>6} {'bss':>7}  state  runtime it calls")
    rows = list(res["modules"].items()) + ([("(hal)", res["hal"])] if res["hal"] else [])
    for m, r in rows:
        if not r["ok"]:
            print(f"   {m:9} doesn't compile: {r['errors'][0]}")
            continue
        st = sum(res["state"].get(ty, 0) * n for ty, n in MODULES.get(m, {}).get("state", {}).items())
        kinds = sorted({kind_of(s) for s in r["helpers"]} - {"memory"})
        absent = f"  NOT IN THE RUNTIME: {', '.join(r['absent'])}" if r["absent"] else ""
        print(f"   {m:9} {r['text']:7} {r['data']:6} {r['bss']:7} {st:6}  {', '.join(kinds) or '-'}{absent}")
    if res.get("state"):
        print("   state:", ", ".join(f"{k} {v}" for k, v in sorted(res["state"].items())))


# ── planning ──
def plan(name, quiet=False):
    t = load(name)
    pj = OUT / name / "probe.json"
    res = json.loads(pj.read_text()) if pj.exists() else probe(name, quiet=True)
    if res.get("error"):
        res = probe(name, quiet=True)
    a, st, net, mem = t["audio"], t["storage"]["kinds"], t["net"]["kind"], t["memory"]
    measured = not res.get("error")
    mods = res.get("modules", {})
    state = res.get("state", {})
    why = {}                                      # module: reason it's out (absent: in)

    hal = res.get("hal")
    hal_calls = set(hal["calls"]) if hal and hal["ok"] else set()

    def cost(m):
        # a module's state: its instance is the caller's, so it's counted here unless already in a measured .bss:
        # fw/app.c holds the lite_deck (taken out of app's count; the engine chosen adds its own), and a HAL that
        # drives a module (calls into it) holds that module's
        r = mods.get(m)
        if not r or not r["ok"]:
            return 0, 0
        s = sum(state.get(ty, 0) * n for ty, n in MODULES[m]["state"].items())
        if hal_calls & set(r["defs"]):
            s = 0
        if m == "app":
            s = -state.get("lite_deck", 0)
        return r["text"] + r["data"], r["data"] + r["bss"] + s

    def can_build(m):
        r = mods.get(m)
        if not measured:
            return "not measured (no compiler)"
        if not r["ok"]:
            return f"doesn't compile: {r['errors'][0]}"
        if r["absent"]:
            return f"needs {', '.join(r['absent'])}, which this runtime hasn't"
        return None

    # what the board allows
    wants = {
        "hifi": None if a["out_channels"] >= 2 and a["out_bits"] >= 12 else "the audio out is mono or under 12 bits",
        "memcard": None if "memcard" in st else "no flash-image storage",
        "dvs": None if a.get("in", "none") != "none" and a.get("in_channels", 0) >= 2 else "no stereo audio in for the record",
        "prolink": None if net in ("ip", "ethernet-raw") else "no network",
        "udpnet": None if net == "ethernet-raw" else ("the IP stack does UDP" if net == "ip" else "no network"),
    }
    if wants["hifi"] is None and "mmap" not in st and "memcard" not in st:
        wants["hifi"] = "no memory-mapped tracks (hifi reads them at random; streamed storage needs a block cache)"
    for m, w in wants.items():
        if w:
            why[m] = w
    for m in MODULES:
        if m not in why:
            e = can_build(m)
            if e:
                why[m] = e
    # the engine: the best one that's possible; one not drivable from fw/app.c yet is reported, the ready one used
    engines = sorted((m for m, s in MODULES.items() if s["role"] == "engine"), key=lambda m: -MODULES[m]["rank"])
    upgrade = None
    engine = None
    for m in engines:
        if m in why:
            continue
        if not MODULES[m].get("ready", True):
            upgrade = m
            why[m] = "possible here; not driven by fw/app.c yet (the engine interface)"
            continue
        engine = m
        break
    for m in engines:
        if m != engine and m not in why:
            why[m] = f"{engine} is the better engine here"
    chosen = ["app"] + ([engine] if engine else [])
    for m in ("memcard", "dvs", "udpnet", "prolink"):
        if m not in why:
            chosen.append(m)
    # the budget: drop the optional features last-first until it fits
    flash, ram = mem["flash_kb"] * 1024, mem["ram_kb"] * 1024
    hal_flash, hal_ram = ((hal["text"] + hal["data"], hal["data"] + hal["bss"]) if hal and hal["ok"]
                          else (0, mem.get("hal_ram_b", 0)))
    hal_measured = bool(hal and hal["ok"])

    def total(ms):
        f = hal_flash + sum(cost(m)[0] for m in ms)
        r = hal_ram + mem.get("stack_b", 0) + sum(cost(m)[1] for m in ms)
        return f, r

    for m in ("prolink", "udpnet", "dvs"):
        f, r = total(chosen)
        if (f > flash or r > ram) and m in chosen:
            chosen.remove(m)
            why[m] = "over budget"
    f, r = total(chosen)
    if "prolink" not in chosen and "udpnet" in chosen:
        chosen.remove("udpnet")
    fits = measured and f <= flash and r <= ram and engine is not None
    calls = sorted({s for m in chosen for s in mods.get(m, {}).get("hal_calls", [])})
    hal_defs = set(hal["defs"]) if hal_measured else set()
    hal_todo = [s for s in calls if s not in hal_defs] if hal_measured else calls
    helpers = sorted({s for m in chosen for s in mods.get(m, {}).get("helpers", [])})
    p = dict(target=name, measured=measured, engine=engine, upgrade=upgrade, chosen=chosen,
             out={m: why[m] for m in MODULES if m not in chosen and m in why}, flash=f, ram=r,
             flash_budget=flash, ram_budget=ram, fits=fits, hal_measured=hal_measured, hal_calls=calls,
             hal_todo=hal_todo, helpers=helpers)
    write_build(t, p)
    if not quiet:
        report_plan(t, p)
    return p


def write_build(t, p):
    d = OUT / t["name"]
    d.mkdir(parents=True, exist_ok=True)
    feats = {m: int(m in p["chosen"]) for m in MODULES if m != "app"}
    h = [f"// target.py plan {t['name']}: generated, don't edit (targets/{t['name']}.toml)", "#pragma once"]
    h += [f"#define FEAT_{m.upper()} {v}" for m, v in feats.items()]
    h += [f"#define TARGET_AUDIO_CHANNELS {t['audio']['out_channels']}",
          f"#define TARGET_AUDIO_BITS {t['audio']['out_bits']}"]
    (d / "config.h").write_text("\n".join(h) + "\n")
    srcs = [s for m in p["chosen"] for s in MODULES[m]["sources"]]
    mk = [f"# target.py plan {t['name']}: generated (targets/{t['name']}.toml)",
          f"TARGET := {t['name']}",
          f"TARGET_SRC := {' '.join(srcs)}",
          f"TARGET_INC := {' '.join('-I' + i for i in INCLUDES)} -Ibuild/targets/{t['name']}",
          f"TARGET_CFLAGS := {t['cpu'].get('cflags', '')} -include build/targets/{t['name']}/config.h"]
    (d / "target.mk").write_text("\n".join(mk) + "\n")


def kb(n):
    return f"{n / 1024:.1f} KB"


def report_plan(t, p):
    print(f"== {t['name']}: {t.get('desc', '')}")
    if not p["measured"]:
        print("   not measured: no compiler (the plan is the capabilities' only)")
    print(f"   engine:   {p['engine'] or ('none fits' if p['measured'] else '- (not measured)')}" + (f"   ({p['upgrade']} would fit: not in fw/app.c yet)" if p["upgrade"] else ""))
    print(f"   build:    {' '.join(p['chosen'])}")
    for m, w in p["out"].items():
        print(f"   - {m:8} {w}")
    note = "" if p["hal_measured"] else "  (the HAL's RAM an estimate)"
    print(f"   flash     {kb(p['flash']):>9} of {kb(p['flash_budget'])} ({100 * p['flash'] / p['flash_budget']:.0f} %)")
    print(f"   RAM       {kb(p['ram']):>9} of {kb(p['ram_budget'])} ({100 * p['ram'] / p['ram_budget']:.0f} %){note}")
    kinds = sorted({kind_of(s) for s in p["helpers"]} - {"memory"})
    print(f"   runtime:  {', '.join(kinds) or 'none beyond memcpy/memset'}")
    if p["hal_measured"]:
        print(f"   HAL:      {len(p['hal_calls']) - len(p['hal_todo'])} of {len(p['hal_calls'])} hal_ functions there"
              + (f"; to write: {', '.join(p['hal_todo'])}" if p["hal_todo"] else ""))
    else:
        print(f"   HAL to write (fw/hal.h): {', '.join(p['hal_calls'])}")
    print(f"   CPU:      not checked here (make deckbench measures RV32EC and the M0+ in qemu; the FM-1 by its probes)")
    print(f"   {'FITS' if p['fits'] else 'DOES NOT FIT'}   -> build/targets/{t['name']}/config.h, target.mk")


# ── a new compiler: what the CPU does ──
TESTS = {
    "mul32": "int f(int a, int b) { return a * b; }",
    "div32": "int f(int a, int b) { return a / b; }",
    "mul64": "long long f(long long a, long long b) { return a * b; }",
    "div64": "long long f(long long a, long long b) { return a / b; }",
    "float": "float f(float a, float b) { return a * b + a; }",
    "double": "double f(double a, double b) { return a * b + a; }",
}


def profile(v):
    v = v.strip().strip("'")
    return chr(int(v)) if v.isdigit() else v           # gcc: 77, clang: 'M'


def new(args):
    name = args.name
    cc = expand(args.cc).split() + expand(args.cflags or "").split()
    r = subprocess.run(cc + ["-dM", "-E", "-x", "c", "-"], input="", capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"target: {args.cc} won't preprocess: {r.stderr.strip()[:300]}")
    mac = dict(re.findall(r"#define (\S+) ?(.*)", r.stdout))
    isa = ("RISC-V rv%s%s" % (mac.get("__riscv_xlen", "?"), "e" if "__riscv_e" in mac or "__riscv_32e" in mac else "i")
           if "__riscv" in mac else f"Arm v{mac.get('__ARM_ARCH', '?')}{profile(mac.get('__ARM_ARCH_PROFILE', ''))}"
           + (" Thumb" if "__thumb__" in mac else "") if "__arm__" in mac or "__ARM_ARCH" in mac
           else "pi32v2 (JieLi)" if "__PI32V2__" in mac else "Xtensa" if "__XTENSA__" in mac
           else "AVR" if "__AVR__" in mac else "x86-64" if "__x86_64__" in mac else "unknown")
    if "__riscv" in mac:
        isa += "".join(x for x, k in (("m", "__riscv_mul"), ("a", "__riscv_atomic"), ("f", "__riscv_flen"),
                                      ("c", "__riscv_compressed")) if k in mac)
    hw = {}
    with tempfile.TemporaryDirectory() as td:
        for k, src in TESTS.items():
            s, o = Path(td) / f"{k}.c", Path(td) / f"{k}.o"
            s.write_text(src + "\n")
            ok = subprocess.run(cc + ["-O2", "-ffreestanding", "-c", str(s), "-o", str(o)], capture_output=True).returncode == 0
            calls = [c for c in undefined(o) if kind_of(c)] if ok else None
            hw[k] = "doesn't compile" if calls is None else ("hardware" if not calls else "runtime: " + ", ".join(calls))
    ptr = mac.get("__SIZEOF_POINTER__", "?")
    print(f"== {name}: {isa}, {ptr}-byte pointers, {'little' if 'LITTLE' in mac.get('__BYTE_ORDER__', '') else 'big'}-endian")
    for k, v in hw.items():
        print(f"   {k:7} {v}")
    f = TARGETS / f"{name}.toml"
    if f.exists() and not args.force:
        print(f"   {f.relative_to(ROOT)} exists (--force to write it again)")
        return
    soft = [k for k, v in hw.items() if v.startswith("runtime")]
    f.write_text(f'''# {name}: written by target.py new ({isa}); the board's part is yours to fill in
name = "{name}"
desc = "{isa}"

[cpu]
cc = "{args.cc}"
cflags = "{args.cflags or ''}"
mhz = 0                # the clock
# runtime helpers the chip's runtime doesn't have (a link that would fail): the probe flags any module calling one
# (in software here: {', '.join(soft) or 'nothing'})
missing = []

[memory]
flash_kb = 0           # for the deck's code
ram_kb = 0             # for the deck's .data + .bss + stack
stack_b = 512
hal_ram_b = 512        # the HAL's own RAM until it exists: [hal] sources = [...] measures it

[audio]
out = "pwm"            # pwm, i2s, dac, usb
out_bits = 10
out_channels = 1
in = "none"            # or i2s, usb-audio (with in_channels = 2: DVS)

[storage]
kinds = ["sd-spi"]     # sd-spi, memcard (an image in flash), mmap (tracks memory-mapped), usb-msc

[display]
kind = "ssd1306"

[input]
kinds = ["ec11", "buttons", "pot", "uart-midi"]

[net]
kind = "none"          # none, ip (sockets on an IP stack), ethernet-raw (frames: fw/udpnet.c)
''')
    print(f"   wrote {f.relative_to(ROOT)}: fill in [memory], [audio], [storage], [net]; then target.py plan {name}")


def all_targets():
    return sorted(p.stem for p in TARGETS.glob("*.toml"))


def main():
    defaults()
    llvm()
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sp = ap.add_subparsers(dest="cmd", required=True)
    for c in ("probe", "plan"):
        s = sp.add_parser(c)
        s.add_argument("names", nargs="*", help="targets (default: all)")
        s.add_argument("--json", action="store_true")
    sp.add_parser("list")
    sp.add_parser("check")
    s = sp.add_parser("new")
    s.add_argument("name")
    s.add_argument("--cc", required=True)
    s.add_argument("--cflags")
    s.add_argument("--force", action="store_true")
    a = ap.parse_args()
    if a.cmd == "new":
        return new(a)
    if a.cmd == "list":
        print(f"{'target':10} {'engine':7} {'flash':>7} {'RAM':>7}  build")
        for n in all_targets():
            p = plan(n, quiet=True)
            print(f"{n:10} {p['engine'] or '-':7} {100 * p['flash'] / p['flash_budget']:6.0f}% {100 * p['ram'] / p['ram_budget']:6.0f}%  "
                  f"{' '.join(p['chosen'])}{'' if p['fits'] else '  (does not fit)'}")
        return
    if a.cmd == "check":
        bad = 0
        for n in all_targets():
            exp = load(n).get("expect")
            if not exp:
                print(f"{n:10} (no [expect])")
                continue
            p = plan(n, quiet=True)
            got = dict(engine=p["engine"], upgrade=p["upgrade"], build=p["chosen"], fits=p["fits"])
            diff = {k: (v, got[k]) for k, v in exp.items() if got.get(k) != v}
            bad += bool(diff)
            print(f"{n:10} {'ok' if not diff else 'CHANGED: ' + ', '.join(f'{k} {v[0]} -> {v[1]}' for k, v in diff.items())}"
                  f"   ({100 * p['flash'] / p['flash_budget']:.0f} % flash, {100 * p['ram'] / p['ram_budget']:.0f} % RAM)")
        sys.exit(1 if bad else 0)
    names = a.names or all_targets()
    for n in names:
        if a.cmd == "probe":
            r = probe(n, quiet=a.json)
        else:
            if (OUT / n / "probe.json").exists():
                (OUT / n / "probe.json").unlink()   # a plan measures afresh
            r = plan(n, quiet=a.json)
        if a.json:
            print(json.dumps(r, indent=1))
        print()


if __name__ == "__main__":
    main()
