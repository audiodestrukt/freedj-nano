# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: GPL-3.0-or-later
"""chipcaps: what a chip has, from what describes it (for tools/target.py).

Three sources, each for what it knows:
  the compiler     the ISA and what its flags make hardware: FPU (single, double), FMA, DSP and SIMD extensions,
                   vector, hardware multiply and divide, atomics, bit manipulation, unaligned access
  the SVD          (CMSIS-SVD, the vendor's register description) the CPU block (FPU, DSP, MPU, caches) and the
                   peripherals: DMA and its channels, I2C, SPI, I2S, USB, Ethernet, SDIO, PIO, QSPI/PSRAM, an
                   external memory bus, display engines (LTDC, LCD_CAM: the ones with a vsync), 2D GPUs, DSP
                   accelerators (CORDIC, FMAC), ADC, DAC, timers, PWM, radios; the cores (one interrupt matrix a
                   core, or an inter-core FIFO)
  the linker script  the memories: flash, RAM, PSRAM / external RAM, tightly coupled RAM

The clock, the board (what's wired: the DAC, the card, the screen's TE pin) and anything the files don't say are
the target file's. Every value says where it came from.
"""
import re, subprocess, urllib.request, xml.etree.ElementTree as ET
from pathlib import Path


# ── the compiler ──
def macros(cc):
    r = subprocess.run(cc + ["-dM", "-E", "-x", "c", "-"], input="", capture_output=True, text=True)
    if r.returncode:
        return None, r.stderr.strip()[:300]
    return dict(re.findall(r"#define (\S+) ?(.*)", r.stdout)), None


def _profile(v):
    v = v.strip().strip("'")
    return chr(int(v)) if v.isdigit() else v           # gcc: 77, clang: 'M'


def isa(mac):
    if "__riscv" in mac:
        s = "rv%s%s" % (mac.get("__riscv_xlen", "?"), "e" if "__riscv_e" in mac or "__riscv_32e" in mac else "i")
        s += "".join(x for x, k in (("m", "__riscv_mul"), ("a", "__riscv_atomic"), ("f", "__riscv_flen"),
                                    ("c", "__riscv_compressed"), ("v", "__riscv_vector")) if k in mac)
        if mac.get("__riscv_flen") == "64":
            s = s.replace("f", "fd", 1)
        return "RISC-V " + s
    if "__ARM_ARCH" in mac or "__arm__" in mac:
        return f"Arm v{mac.get('__ARM_ARCH', '?')}{_profile(mac.get('__ARM_ARCH_PROFILE', ''))}" + \
               (" Thumb" if "__thumb__" in mac else "")
    for k, n in (("__PI32V2__", "pi32v2 (JieLi)"), ("__XTENSA__", "Xtensa"), ("__AVR__", "AVR"),
                 ("__x86_64__", "x86-64"), ("__aarch64__", "AArch64"), ("__mips__", "MIPS")):
        if k in mac:
            return n
    return "unknown"


def compiler_caps(mac):
    """what the compiler, with these flags, treats as hardware (the macros' view; the test functions confirm it)"""
    c = {}
    arm_fp = int(mac.get("__ARM_FP", "0"), 0)
    if "__riscv" in mac:
        fl = mac.get("__riscv_flen")
        c["fpu"] = "double" if fl == "64" else "single" if fl == "32" else "none"
        c["mul"] = "__riscv_mul" in mac
        c["div"] = "__riscv_div" in mac
        c["atomics"] = "__riscv_atomic" in mac
        c["vector"] = "__riscv_vector" in mac
        c["bitmanip"] = "__riscv_zbb" in mac
        c["dsp"] = any(k in mac for k in ("__riscv_p", "__riscv_zpn", "__riscv_xesppie"))
    elif "__ARM_ARCH" in mac:
        c["fpu"] = "double" if arm_fp & 8 else "single" if arm_fp & 4 else "none"
        c["div"] = "__ARM_FEATURE_IDIV" in mac
        c["dsp"] = "__ARM_FEATURE_DSP" in mac
        c["simd"] = "neon" if "__ARM_NEON" in mac else "mve" if "__ARM_FEATURE_MVE" in mac else \
                    "simd32" if "__ARM_FEATURE_SIMD32" in mac else "none"
        c["atomics"] = "__ARM_FEATURE_LDREX" in mac
        c["clz"] = "__ARM_FEATURE_CLZ" in mac
        c["unaligned"] = "__ARM_FEATURE_UNALIGNED" in mac
    elif "__XTENSA__" in mac:
        c["fpu"] = "none" if "__XTENSA_SOFT_FLOAT__" in mac else "single"
    c["fma"] = "__FP_FAST_FMAF" in mac or "__FP_FAST_FMA" in mac
    c["soft_float_abi"] = "__SOFTFP__" in mac
    c["pointer"] = int(mac.get("__SIZEOF_POINTER__", "0"))
    c["endian"] = "big" if "BIG" in mac.get("__BYTE_ORDER__", "") and "__BIG_ENDIAN__" in mac else "little"
    return {k: v for k, v in c.items()}


# ── the SVD ──
PERIPHS = [   # (capability, a peripheral's name matches)
    ("dma",      r"^(G?DMA\d*|DMAC\d*|GPDMA\d*|BDMA\d*|MDMA|HPDMA\d*|LPDMA\d*|EDMA\d*|AHB_DMA|AXI_DMA)$"),
    ("i2c",      r"^(LP_)?I2C\d*$|^TWIM?\d*$|^LPI2C\d*$|^RTC_I2C$"),
    ("spi",      r"^(LP)?SPI\d*$|^SPIM\d*$"),
    ("i2s",      r"^(LP_)?I2S\d*$|^SAI\d*$|^ALNK\d*$"),
    ("usb",      r"^USB|^OTG|^USBHS|^USBFS|^USBD$"),
    ("ethernet", r"^(ETH|EMAC|ENET|GMAC)"),
    ("sdio",     r"^(SDIO|SDMMC|SDHOST|SDHC|USDHC)\d*$"),
    ("pio",      r"^PIO\d+$|^PARL_IO$|^FLEXIO\d*$"),
    ("qspi",     r"^(QSPI|OCTOSPI\d*|XSPI\d*|QMI|XIP_SSI|SSI|FLEXSPI\d*|EXTMEM|OSPI\d*|HSPI\d*)$"),
    ("ext_bus",  r"^(FMC|FSMC|SEMC|EXMC|SDRAMC?)$"),
    ("display",  r"^(LTDC|LCD_CAM|LCDC|LCDIF|DSI(HOST)?|HSTX_CTRL|DCMIPP|MIPI_DSI)$"),
    ("gpu",      r"^(DMA2D|GPU2D|GPU|PPA|PXP|NEMA\w*|VGLITE|JPEG)$"),
    ("dsp_accel", r"^(CORDIC|FMAC|POWERQUAD|MATHACL|HWDIV|INTERP\d*)$"),
    ("adc",      r"ADC"),
    ("dac",      r"^DAC\d*$"),
    ("timer",    r"^(TIM|TIMER|TIMG|LPTIM|CTIMER|SYSTIMER|GPTIMER)\d*$"),
    ("pwm",      r"PWM|^LEDC$"),
    ("uart",     r"U(S)?ART"),
    ("radio",    r"^(MODEM_\w+|IEEE802154|BB|BLE\w*|RADIO|WIFI\w*|BT\w*)$"),
]


def fetch(src, cache):
    """a path, or a URL (fetched once into cache/)"""
    if not re.match(r"https?://", src):
        p = Path(src)
        return p if p.exists() else None
    cache.mkdir(parents=True, exist_ok=True)
    p = cache / src.rsplit("/", 1)[-1]
    if not p.exists():
        try:
            with urllib.request.urlopen(src, timeout=30) as r:
                p.write_bytes(r.read())
        except Exception:
            return None
    return p


def _dma_channels(per):
    """the channels a DMA peripheral's registers name (CH3_..., CCR3 (ST, Puya), CFGR3 (WCH), ..._CH3 (Espressif),
    CH[%s] arrays by their dim)"""
    nums, dims = set(), 0
    regs = per.find("registers")
    if regs is None:
        return 0
    for r in regs.iter():
        if r.tag not in ("register", "cluster"):
            continue
        name = (r.findtext("name") or "").upper()
        if r.findtext("dim") and re.search(r"CH|CHANNEL", name):
            dims = max(dims, int(r.findtext("dim"), 0))
        nums |= {int(m.group(1)) for m in re.finditer(r"(?:^|_)(?:CH|CCR|CNDTR|CPAR|CMAR|CHAN|CFGR|CNTR|PADDR|MADDR)(\d+)(?=_|$)", name)}
    n = (max(nums) + (1 if 0 in nums else 0)) if nums else 0
    return max(n, dims)


def svd_caps(path):
    root = ET.parse(path).getroot()
    out = {"device": root.findtext("name") or "", "vendor": (root.findtext("vendor") or "").strip()}
    cpu = root.find("cpu")
    if cpu is not None:
        t = lambda k: (cpu.findtext(k) or "").strip().lower() in ("1", "true")
        out["cpu"] = (cpu.findtext("name") or "").strip()
        out["fpu"] = ("double" if t("fpuDP") else "single") if t("fpuPresent") else "none"
        out["dsp"] = t("dspPresent")
        out["mpu"] = t("mpuPresent")
        out["icache"] = t("icachePresent")
        out["dcache"] = t("dcachePresent")
    pers = root.find("peripherals").findall("peripheral")
    names = [(p.findtext("name") or "").upper() for p in pers]
    found = {}
    for cap, rx in PERIPHS:
        hit = [n for n in names if re.search(rx, n)]
        if cap == "spi" and out["vendor"].lower().startswith("espressif"):
            hit = [n for n in hit if n not in ("SPI0", "SPI1")]   # Espressif's SPI0/1: the flash and PSRAM's
        if hit:
            found[cap] = hit
    out["periph"] = found
    chans = [_dma_channels(p) for p in pers if re.search(PERIPHS[0][1], (p.findtext("name") or "").upper())]
    if found.get("dma"):
        out["dma_channels"] = sum(chans) or None
    cores = len([n for n in names if re.match(r"^INTERRUPT_CORE\d+$|^CPU\d+_?INT|^CORE\d+_INT", n)])
    if not cores and "SIO" in names:
        cores = 2                                    # the RP2040/RP2350's SIO: the inter-core FIFOs
    if cores:
        out["cores"] = cores
    # PSRAM: a memory controller that maps one (RP2350's QMI has a second chip select for it). Espressif's
    # EXTMEM is the flash cache on chips with PSRAM and without (the C6 has none): the target file says.
    if found.get("qspi") and any(n in names for n in ("QMI", "OCTOSPI1", "OCTOSPI", "XSPI1", "FLEXSPI", "FLEXSPI1")):
        out["psram_capable"] = True
    return out


# ── the linker script ──
def _length(s):
    s = s.strip()
    m = re.fullmatch(r"(0x[0-9a-fA-F]+|\d+)\s*([KkMm])?", s)
    if not m:
        return None
    n = int(m.group(1), 0)
    return n * {"k": 1024, "m": 1024 * 1024}.get((m.group(2) or "").lower(), 1)


def ld_caps(path):
    text = Path(path).read_text(errors="replace")
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    m = re.search(r"MEMORY\s*\{(.*?)\}", text, flags=re.S)
    out = {"regions": {}}
    if not m:
        return out
    for name, attr, origin, length in re.findall(
            r"(\w+)\s*(\([^)]*\))?\s*:\s*ORIGIN\s*=\s*([^,]+?)\s*,\s*LENGTH\s*=\s*([^\n;]+)", m.group(1)):
        n = _length(length)
        if n is None:
            continue
        u = name.upper()
        kind = ("psram" if re.search(r"PSRAM|SPIRAM|EXTRAM|SDRAM|HYPERRAM|EXT_RAM", u) else
                "tcm" if re.search(r"TCM", u) else
                "flash" if re.search(r"FLASH|ROM|XIP|IROM|DROM", u) else
                "ram" if re.search(r"RAM|SCRATCH|DATA|BSS", u) or "w" in (attr or "") else "other")
        out["regions"][name] = {"kind": kind, "origin": origin.strip(), "bytes": n}
    for k in ("flash", "ram", "psram", "tcm"):
        b = sum(r["bytes"] for r in out["regions"].values() if r["kind"] == k)
        if b:
            out[k + "_kb"] = b // 1024
    return out


# ── the compiler flags each CPU wants (the SVD's cpu name; what a target's cflags should have) ──
CPU_FLAGS = {
    "CM0":     "-mcpu=cortex-m0 -mthumb",
    "CM0PLUS": "-mcpu=cortex-m0plus -mthumb", "CM0+": "-mcpu=cortex-m0plus -mthumb",
    "CM3":     "-mcpu=cortex-m3 -mthumb",
    "CM4":     "-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16",
    "CM7":     "-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16",
    "CM7DP":   "-mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16",
    "CM23":    "-mcpu=cortex-m23 -mthumb",
    "CM33":    "-mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16",
    "CM55":    "-mcpu=cortex-m55 -mthumb -mfloat-abi=hard",
    "CM85":    "-mcpu=cortex-m85 -mthumb -mfloat-abi=hard",
    "RV32IMAC": "-march=rv32imac_zicsr -mabi=ilp32",
    "RV32IMC": "-march=rv32imc_zicsr -mabi=ilp32",
    "RV32IMAFC": "-march=rv32imafc_zicsr -mabi=ilp32f",
    "RV32EC":  "-march=rv32ec_zicsr -mabi=ilp32e",
}


def cpu_flags(svd):
    name = (svd.get("cpu") or "").upper().replace(" ", "")
    if name == "CM7" and svd.get("fpu") == "double":
        name = "CM7DP"
    if name in CPU_FLAGS and svd.get("fpu") == "none" and "float-abi=hard" in CPU_FLAGS[name]:
        return CPU_FLAGS[name].split(" -mfloat-abi")[0]          # (an M4 or M33 without its FPU)
    return CPU_FLAGS.get(name)
