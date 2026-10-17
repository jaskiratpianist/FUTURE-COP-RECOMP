#!/usr/bin/env python3
"""Differential test: run functions in Unicorn (the original x86 code) and as recompiled
C from the same random states, and compare registers and memory.

    recomp/difftest.py                  # every function, summary
    recomp/difftest.py 0x401850 -v      # one function, show differences
"""
import argparse, ctypes, json, os, random, struct, subprocess, sys

from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn import UC_HOOK_MEM_READ_UNMAPPED, UC_HOOK_MEM_WRITE_UNMAPPED
from unicorn import x86_const as X

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from image import Image  # noqa: E402
from paths import exe  # noqa: E402

BUILD = os.path.join(ROOT, "build", "recomp")
LIB = os.path.join(BUILD, "harness.so")
SENTINEL, FOREIGN = 0xDEAD0000, 0x7F000000
SCRATCH, SCRATCH_N = 0x10000000, 0x10000
STACK, STACK_N = 0x20000000, 0x100000
ESP0 = STACK + 0x80000
IMG_LO, IMG_HI = 0x400000, 0x522000
COMPARE = [(0x486000, 0x514000 - 0x486000), (0x516000, 0x3C60), (SCRATCH, SCRATCH_N), (ESP0 - 0x8000, 0x8040)]
REGS = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
UREG = [X.UC_X86_REG_EAX, X.UC_X86_REG_ECX, X.UC_X86_REG_EDX, X.UC_X86_REG_EBX,
        X.UC_X86_REG_ESP, X.UC_X86_REG_EBP, X.UC_X86_REG_ESI, X.UC_X86_REG_EDI]
STATUS = ["ok", "trap", "fault", "import", "timeout"]


class Cpu(ctypes.Structure):
    _fields_ = [("r", ctypes.c_uint32 * 8), ("fs_base", ctypes.c_uint32), ("df", ctypes.c_uint32),
                ("st", ctypes.c_double * 8), ("top", ctypes.c_uint32), ("sw", ctypes.c_uint16),
                ("cw", ctypes.c_uint16), ("mm", ctypes.c_uint64 * 8), ("fl", ctypes.c_uint32 * 5),
                ("sti", ctypes.c_int64 * 8), ("stx", ctypes.c_uint8 * 8)]


def build_lib(trace=False):
    gen = os.path.join(BUILD, "gen")
    objs = os.path.join(BUILD, "obj_trace" if trace else "obj")
    lib = LIB.replace(".so", "_trace.so") if trace else LIB
    rt = os.path.join(HERE, "rt")
    os.makedirs(objs, exist_ok=True)
    srcs = sorted(os.path.join(gen, f) for f in os.listdir(gen) if f.endswith(".c"))
    srcs += [os.path.join(rt, "rt.c"), os.path.join(rt, "harness.c")]
    procs = []
    for s in srcs:
        o = os.path.join(objs, os.path.basename(s)[:-2] + ".o")
        if not os.path.exists(o) or os.path.getmtime(o) < os.path.getmtime(s):
            procs.append(subprocess.Popen(["gcc", "-O1", "-fPIC", "-w", "-c", f"-I{rt}", f"-I{gen}", s, "-o", o]
                                          + (["-DRECOMP_TRACE"] if trace else [])))
    if any(p.wait() for p in procs):
        sys.exit("compile failed")
    subprocess.run(["gcc", "-shared", "-o", lib] + [os.path.join(objs, os.path.basename(s)[:-2] + ".o") for s in srcs]
                   + ["-lm"], check=True)
    return lib


class World:
    """The initial guest memory image shared by both sides."""
    def __init__(self):
        self.img = Image(exe())
        mem = bytearray(IMG_HI - IMG_LO)
        for _, va, vsize, raw, rsize in self.img.sections:
            data = self.img.data[raw:raw + min(rsize, vsize)]
            mem[va - IMG_LO:va - IMG_LO + len(data)] = data
        # import address table entries point at a "foreign" range both sides treat as an import call
        for k, line in enumerate(l for l in open(os.path.join(ROOT, "data", "symbols.txt")) if not l.startswith("#")):
            a = int(line.split()[0], 16)
            struct.pack_into("<I", mem, a - IMG_LO, FOREIGN + 4 * k)
        self.image = bytes(mem)


def state(rng):
    def val():
        k = rng.random()
        if k < 0.4:
            return rng.randrange(0, 256)
        if k < 0.75:
            return SCRATCH + rng.randrange(0, SCRATCH_N // 2) & ~3
        return rng.getrandbits(32)
    regs = [val() for _ in range(8)]
    regs[4] = ESP0
    stack = b"".join(struct.pack("<I", val()) for _ in range(32))
    scratch = bytes(rng.getrandbits(8) for _ in range(256)) * (SCRATCH_N // 256)
    return regs, stack, scratch


def run_unicorn(w, addr, regs, stack, scratch, trace=None):
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(IMG_LO, IMG_HI - IMG_LO)
    uc.mem_write(IMG_LO, w.image)
    uc.mem_map(SCRATCH, SCRATCH_N)
    uc.mem_write(SCRATCH, scratch)
    uc.mem_map(STACK, STACK_N)
    uc.mem_write(ESP0, stack)
    uc.mem_map(FOREIGN, 0x1000)
    uc.mem_write(FOREIGN, b"\xcc" * 0x1000)
    esp = ESP0 - 4
    uc.mem_write(esp, struct.pack("<I", SENTINEL))
    for r, v in zip(UREG, regs):
        uc.reg_write(r, v)
    uc.reg_write(X.UC_X86_REG_ESP, esp)
    uc.reg_write(X.UC_X86_REG_FPCW, 0x27F)
    hit = {}

    def on_code(uc, a, size, _):
        hit["import"] = a
        uc.emu_stop()
    uc.hook_add(UC_HOOK_CODE, on_code, begin=FOREIGN, end=FOREIGN + 0xFFF)
    pages = []

    def on_unmapped(uc, access, a, size, value, _):
        for p in {a & ~0xFFF, (a + size - 1) & ~0xFFF}:
            if p in pages or len(pages) >= 256 or p < 0x1000:
                return False
            uc.mem_map(p, 0x1000)
            pages.append(p)
        return True
    uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED, on_unmapped)
    if trace is not None:
        def on_ins(uc, a, size, _):
            trace.append((a, *(uc.reg_read(r) for r in UREG)))
        uc.hook_add(UC_HOOK_CODE, on_ins, begin=0x401000, end=0x483AE0)
    try:
        uc.emu_start(addr, SENTINEL, count=1_000_000)
    except UcError as e:
        return "fault", None
    if "import" not in hit and uc.reg_read(X.UC_X86_REG_EIP) != SENTINEL:
        return "timeout", None
    out = [uc.reg_read(r) for r in UREG]
    mem = [bytes(uc.mem_read(a, n)) for a, n in COMPARE]
    mem += [bytes(uc.mem_read(p, 0x1000)) for p in sorted(pages)]
    # an import call is compared as far as it got: same import, same state
    return ("import", hit["import"]) if "import" in hit else "ok", (out, mem, sorted(pages))


class CSide:
    def __init__(self, w, lib=LIB):
        self.lib = ctypes.CDLL(lib)
        assert self.lib.h_init() == 0
        assert self.lib.h_cpu_size() == ctypes.sizeof(Cpu), "Cpu struct out of sync with rt/cpu.h"
        self.lib.h_mem.restype = ctypes.POINTER(ctypes.c_uint8)
        self.base = ctypes.cast(self.lib.h_mem(), ctypes.c_void_p).value
        for a, n in ((IMG_LO, IMG_HI - IMG_LO), (SCRATCH, SCRATCH_N), (STACK, STACK_N)):
            assert self.lib.h_map(a, n) == 0
        self.w = w

    def run(self, addr, regs, stack, scratch):
        ctypes.memmove(self.base + IMG_LO, self.w.image, len(self.w.image))
        ctypes.memmove(self.base + SCRATCH, scratch, len(scratch))
        ctypes.memset(self.base + STACK, 0, STACK_N)
        ctypes.memmove(self.base + ESP0, stack, len(stack))
        c = Cpu()
        for k, v in enumerate(regs):
            c.r[k] = v
        c.cw = 0x27F
        self.lib.h_reset()
        st = self.lib.h_run(ctypes.byref(c), addr, 2000)
        pp = ctypes.POINTER(ctypes.c_uint32)()
        pages = sorted(pp[k] for k in range(self.lib.h_demand(ctypes.byref(pp))))
        if st and STATUS[st] != "import":
            return STATUS[st], self.lib.h_last()
        mem = [ctypes.string_at(self.base + a, n) for a, n in COMPARE]
        mem += [ctypes.string_at(self.base + p, 0x1000) for p in pages]
        status = ("import", self.lib.h_last()) if st else "ok"
        return status, (list(c.r), mem, pages)


def diff(u, cres):
    (ur, um, up), (cr, cm, cp) = u, cres
    out = [f"{REGS[k]} 0x{ur[k]:x} vs 0x{cr[k]:x}" for k in range(8) if ur[k] != cr[k]]
    if up != cp:
        return out + [f"demand pages {len(up)} vs {len(cp)}"]
    for (a, n), x, y in zip(COMPARE + [(p, 0x1000) for p in up], um, cm):
        if x != y:
            k = next(j for j in range(n) if x[j] != y[j])
            out.append(f"mem 0x{a + k:x}: {x[k:k + 8].hex()} vs {y[k:k + 8].hex()}")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addrs", nargs="*")
    ap.add_argument("-n", type=int, default=6, help="trials per function")
    ap.add_argument("-v", action="store_true")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--trace", type=int, metavar="TRIAL", help="find the first diverging instruction in this trial")
    a = ap.parse_args()
    w = World()
    if a.trace is not None:
        cs = CSide(w, build_lib(trace=True))
        addr = int(a.addrs[0], 16)
        regs, stack, scratch = state(random.Random(addr * 100 + a.trace))
        ut = []
        print("unicorn:", run_unicorn(w, addr, regs, stack, scratch, ut)[0], "C:", cs.run(addr, regs, stack, scratch)[0])
        n = ctypes.c_uint32()
        cs.lib.h_trace.restype = ctypes.POINTER(ctypes.c_uint32)
        p = cs.lib.h_trace(ctypes.byref(n))
        ct = [tuple(p[9 * k + j] for j in range(9)) for k in range(n.value)]
        for k, (x, y) in enumerate(zip(ut, ct)):
            if x != y:
                prev = ut[k - 1][0] if k else 0
                print(f"step {k}: after 0x{prev:x}")
                print("  unicorn", f"0x{x[0]:x}", " ".join(f"{REGS[j]}={x[j + 1]:x}" for j in range(8)))
                print("  C      ", f"0x{y[0]:x}", " ".join(f"{REGS[j]}={y[j + 1]:x}" for j in range(8)))
                return
        print(f"traces agree for {min(len(ut), len(ct))} steps (unicorn {len(ut)}, C {len(ct)})")
        return
    build_lib()
    funcs = [int(f["addr"], 16) for f in json.load(open(os.path.join(ROOT, "data", "functions.json")))]
    if a.addrs:
        funcs = [int(x, 16) for x in a.addrs]
    import multiprocessing
    jobs = [(funcs[k::a.j], a.n, a.v) for k in range(a.j)]
    with multiprocessing.get_context("fork").Pool(a.j) as pool:
        results = [r for part in pool.map(worker, jobs) for r in part]
    tally = {"pass": 0, "fail": 0, "untested": 0}
    failed = []
    for addr, passed, bad, lines in sorted(results):
        for l in lines:
            print(l)
        if bad:
            tally["fail"] += 1
            failed.append(addr)
        elif passed:
            tally["pass"] += 1
        else:
            tally["untested"] += 1
    print(f"{len(funcs)} functions: {tally['pass']} pass, {tally['fail']} fail, "
          f"{tally['untested']} untested (every trial faulted or timed out in Unicorn)")
    if failed:
        print("failing:", " ".join(f"0x{x:x}" for x in failed[:80]))


def worker(job):
    funcs, n, verbose = job
    w = World()
    cs = CSide(w)
    out = []
    for addr in funcs:
        passed = bad = 0
        lines = []
        for t in range(n):
            regs, stack, scratch = state(random.Random(addr * 100 + t))
            us, ures = run_unicorn(w, addr, regs, stack, scratch)
            if us not in ("ok",) and not isinstance(us, tuple):
                continue
            cst, cres = cs.run(addr, regs, stack, scratch)
            if us != cst:
                d = [f"unicorn {us}, C side {cst}" + (f" at 0x{cres:x}" if isinstance(cres, int) else "")]
            else:
                d = diff(ures, cres)
            if d:
                bad += 1
                if verbose:
                    lines.append(f"0x{addr:x} trial {t}: " + "; ".join(d[:6]))
            else:
                passed += 1
        out.append((addr, passed, bad, lines))
    return out


if __name__ == "__main__":
    main()
