#!/usr/bin/env python3
"""Statically recompile FCopLAPD.exe: translate every function's x86 code into C.

    recomp/x86c.py                    # all functions -> build/recomp/gen/*.c
    recomp/x86c.py 0x401850 0x4018a0  # just these (for testing)

Output is derived from the retail EXE and is never committed. Each original function
becomes `void F_<addr>(Cpu *c)` working on guest memory (see rt/cpu.h).
"""
import json, os, re, struct, sys

import capstone
from capstone import x86

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from image import Image  # noqa: E402
from paths import exe  # noqa: E402

OUT = os.path.join(ROOT, "build", "recomp", "gen")
TEXT_END = 0x483AE0

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True

T = {1: "u8", 2: "u16", 4: "u32", 8: "u64"}
S = {1: "s8", 2: "s16", 4: "s32", 8: "s64"}
REG32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
REG16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
REG8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
REG8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}
COND = {"o": "OF", "no": "!OF", "b": "CF", "ae": "!CF", "e": "ZF", "ne": "!ZF", "be": "(CF|ZF)",
        "a": "!(CF|ZF)", "s": "SF", "ns": "!SF", "p": "PF", "np": "!PF", "l": "(SF!=OF)",
        "ge": "(SF==OF)", "le": "(ZF|(SF!=OF))", "g": "(!ZF&&SF==OF)"}
CC_ALIAS = {"c": "b", "nae": "b", "nc": "ae", "nb": "ae", "z": "e", "nz": "ne", "na": "be", "nbe": "a",
            "pe": "p", "po": "np", "nge": "l", "nl": "ge", "ng": "le", "nle": "g"}


def cond(cc):
    return COND[CC_ALIAS.get(cc, cc)]


class Unsupported(Exception):
    pass


class Func:
    """One C function: decoded from `entry` within the original function [lo, hi).
    entry == lo except for extra entry points that other code jumps or calls into."""
    def __init__(self, gen, entry, lo, hi, name):
        self.g, self.entry, self.addr, self.end, self.name = gen, entry, lo, hi, name
        self.code = gen.img.read(lo, hi - lo + 16)  # the last instruction may straddle the end
        self.ins, self.tables, self.targets, self.calls = {}, {}, set(), set()
        self.decode()

    def inside(self, a):
        return self.addr <= a < self.end

    def decode(self):
        """Recursive descent from the entry, following branches and switch tables."""
        todo = [self.entry]
        while todo:
            pc = todo.pop()
            if not self.inside(pc):
                continue
            while pc not in self.ins:
                if not self.inside(pc):  # runs on past the end: needs an entry there
                    self.targets.add(pc)
                    break
                i = next(md.disasm(self.code[pc - self.addr:pc - self.addr + 16], pc), None)
                if i is None:
                    break
                self.ins[pc] = i
                m = i.mnemonic
                op0 = i.operands[0] if i.operands else None
                if (m.startswith("j") or m.startswith("loop")) and op0 is not None and op0.type == x86.X86_OP_IMM:
                    tgt = op0.imm & 0xFFFFFFFF
                    self.targets.add(tgt)
                    if self.inside(tgt):
                        todo.append(tgt)
                    if m == "jmp":
                        break
                elif m == "jmp":
                    if op0.type == x86.X86_OP_MEM and op0.mem.index != 0 and op0.mem.scale == 4 and op0.mem.base == 0:
                        tbl = self.table(op0.mem.disp & 0xFFFFFFFF)
                        self.tables[pc] = tbl
                        for t in tbl:
                            self.targets.add(t)
                            if self.inside(t):
                                todo.append(t)
                    break
                elif m in ("ret", "retf", "iretd", "int3", "hlt"):
                    break
                elif m == "call" and op0.type == x86.X86_OP_IMM:
                    self.calls.add(op0.imm & 0xFFFFFFFF)
                pc += i.size

    def table(self, at):
        # MSVC's memcpy indexes some tables with negative values: take valid entries just before too
        before = []
        for k in range(1, 5):
            t = struct.unpack("<I", self.g.img.read(at - 4 * k, 4))[0]
            if not self.inside(t) or at - 4 * k in self.ins:
                break
            before.append(t)
        return before + self.table_fwd(at)

    def table_fwd(self, at):
        out = []
        while True:
            t = struct.unpack("<I", self.g.img.read(at + 4 * len(out), 4))[0]
            # tables sometimes point into a neighbouring "function" (gap-filling splits):
            # accept any code address, foreign ones become extra entry points
            if len(out) > 512 or (out and at + 4 * len(out) in self.ins):
                return [x for x in out if x]
            if not 0x401000 <= t < TEXT_END:
                if out:  # an unused leading slot may hold anything
                    return [x for x in out if x]
                t = 0
            out.append(t)


class Gen:
    def __init__(self):
        self.img = Image(exe())
        self.funcs = [f for f in json.load(open(os.path.join(ROOT, "data", "functions.json")))]
        self.starts = {int(f["addr"], 16) for f in self.funcs}
        self.unsupported = {}
        self.fns = {}  # entry -> Func

    def build(self):
        """Decode every function, then add extra entry points for every place that other
        code jumps or calls into the middle of, until nothing new turns up."""
        ranges = sorted((int(f["addr"], 16), int(f["addr"], 16) + f["size"], f["name"]) for f in self.funcs)
        for lo, hi, name in ranges:
            self.fns[lo] = Func(self, lo, lo, hi, name)
        self.starts = set(self.fns)
        import bisect
        los = [r[0] for r in ranges]
        todo = list(self.fns.values())
        # code addresses stored in data (function pointer and jump target tables the
        # self-modifying renderer uses) must be enterable too
        insn_at = {a for fn in self.fns.values() for a in fn.ins}
        inside = set()  # bytes strictly inside a decoded instruction can never be a target
        for fn in self.fns.values():
            for a, i in fn.ins.items():
                inside.update(range(a + 1, a + i.size))
        data_ptrs = set()
        for name, va, vsize, raw, rsize in self.img.sections:
            if name in (".rdata", ".data", "UVA_DATA"):
                blob = self.img.read(va, rsize)
                for k in range(0, len(blob) - 3, 4):
                    t = struct.unpack_from("<I", blob, k)[0]
                    if (t in insn_at or 0x401000 <= t < TEXT_END and t not in inside
                            and self.img.read(t, 1)[0] not in (0x00, 0x90, 0xCC)) and t not in self.fns:
                        data_ptrs.add(t)
        holder = Func.__new__(Func)
        holder.targets, holder.calls, holder.ins = data_ptrs, set(), {}
        holder.addr = holder.end = 0
        todo.append(holder)
        while todo:
            new = []
            for fn in todo:
                for t in fn.targets | fn.calls:
                    if fn.inside(t) and t in fn.ins or t in self.fns or not 0x401000 <= t < TEXT_END:
                        continue
                    k = bisect.bisect_right(los, t) - 1
                    if k < 0 or t >= ranges[k][1]:
                        continue
                    lo, hi, name = ranges[k]
                    e = Func(self, t, lo, hi, f"{name}+0x{t - lo:x}")
                    self.fns[t] = e
                    self.starts.add(t)
                    new.append(e)
            todo = new
        self.self_modifying()

    PATCH_TABLE = 0x48ea20  # FUN_0045b1c0 rewrites code for the 555/565 pixel formats from this table

    def self_modifying(self):
        """Find code bytes the game rewrites at run time. Fields that are exactly an
        immediate, a displacement or a branch offset are read from guest memory by the
        generated code; instructions whose opcode bytes change get one version per
        possible encoding, chosen by comparing the current bytes."""
        allins = {}
        for fn in self.fns.values():
            allins.update(fn.ins)
        fields = {}
        for a, i in allins.items():
            if i.mnemonic == "mov" and i.operands[0].type == x86.X86_OP_MEM:
                m = i.operands[0].mem
                if m.base == 0 and m.index == 0 and 0x401000 <= m.disp < TEXT_END:
                    fields[m.disp] = (i.operands[0].size, None)
        table, at = [], self.PATCH_TABLE
        while self.img.read(at, 1)[0]:
            t = self.img.read(at, 1)[0]
            f, v0, v1 = struct.unpack("<III", self.img.read(at + 1, 12))
            table.append((f, t // 8, v0, v1))
            fields[f] = (t // 8, (v0, v1))
            at += 13
        states = []
        for k in (0, 1):
            img = {}
            for f, n, *v in table:
                for j, b in enumerate(v[k].to_bytes(n, "little")):
                    img[f + j] = b
            states.append(img)
        starts = sorted(allins)
        import bisect
        self.dyn_imm, self.dyn_disp, self.dyn_rel, self.variants = {}, {}, {}, {}
        for f, (n, vals) in sorted(fields.items()):
            k = bisect.bisect_right(starts, f) - 1
            i = allins.get(starts[k]) if k >= 0 else None
            if i is None or not i.address <= f < i.address + i.size:
                print(f"warning: code field 0x{f:x} is not inside a decoded instruction")
                continue
            off, branch = f - i.address, i.mnemonic in ("jmp", "call") or i.mnemonic.startswith("j")
            if off == i.imm_offset and n == i.imm_size and f + n <= i.address + i.size:
                (self.dyn_rel if branch else self.dyn_imm)[i.address] = (f, n)
            elif off == i.disp_offset and n == i.disp_size:
                self.dyn_disp[i.address] = (f, n)
            elif vals is None:
                print(f"warning: runtime-written field 0x{f:x} is not an operand of {i.mnemonic} {i.op_str}")
            else:
                for a in starts[k:]:
                    if a >= f + n:
                        break
                    self.variants[a] = None
        for a in list(self.variants):
            i = allins[a]
            vs, seen = [], {bytes(i.bytes)}
            for img in states:
                b = bytes(img.get(a + j, x) for j, x in enumerate(i.bytes))
                if b in seen:
                    continue
                seen.add(b)
                v = next(md.disasm(b, a), None)
                if v is None or v.size != i.size:
                    raise SystemExit(f"variant of 0x{a:x} changes the instruction length")
                vs.append(v)
            self.variants[a] = vs
            for fn in self.fns.values():
                if a in fn.ins:
                    for v in vs:
                        if v.mnemonic.startswith("j") and v.operands[0].type == x86.X86_OP_IMM:
                            fn.targets.add(v.operands[0].imm & 0xFFFFFFFF)
        print(f"self-modifying code: {len(self.dyn_imm)} immediates, {len(self.dyn_disp)} displacements, "
              f"{len(self.dyn_rel)} branch offsets read at run time; {len(self.variants)} rewritten instructions")

    def field(self, f, n, size):
        """Read a rewritten code field; narrower fields are sign-extended like the CPU does."""
        v = f"rd{8 * n}(0x{f:x}u)"
        return f"(({T[size]})({S[n]}){v})" if n < size else v

    # ---- operands ---------------------------------------------------------------
    def addr(self, i, op):
        m, parts = op.mem, []
        if m.base:
            parts.append(i.reg_name(m.base))
        if m.index:
            parts.append(f"{i.reg_name(m.index)}*{m.scale}" if m.scale != 1 else i.reg_name(m.index))
        if i.address in self.dyn_disp:
            parts.append(self.field(*self.dyn_disp[i.address], 4))
        elif m.disp or not parts:
            parts.append(f"0x{m.disp & 0xFFFFFFFF:x}u")
        if m.segment == x86.X86_REG_FS:
            parts.append("c->fs_base")
        return "(u32)(" + " + ".join(parts) + ")"

    def rd(self, i, op, size=None):
        size = size or op.size
        if op.type == x86.X86_OP_IMM:
            if i.address in self.dyn_imm:
                return f"(({T[size]}){self.field(*self.dyn_imm[i.address], 4)})"
            return f"(({T[size]})0x{op.imm & ((1 << (8 * size)) - 1):x}u)"
        if op.type == x86.X86_OP_MEM:
            return f"rd{8 * size}({self.addr(i, op)})"
        r = i.reg_name(op.reg)
        if r in REG32:
            return r
        if r in REG16:
            return f"((u16){REG16[r]})"
        if r in REG8L:
            return f"((u8){REG8L[r]})"
        if r in REG8H:
            return f"((u8)({REG8H[r]} >> 8))"
        if r.startswith("mm"):
            return f"c->mm[{r[2]}]"
        raise Unsupported(f"register {r}")

    def wr(self, i, op, val):
        if op.type == x86.X86_OP_MEM:
            return f"wr{8 * op.size}({self.addr(i, op)}, {val});"
        r = i.reg_name(op.reg)
        if r in REG32:
            return f"{r} = (u32)({val});"
        if r in REG16:
            return f"{REG16[r]} = ({REG16[r]} & 0xffff0000u) | (u16)({val});"
        if r in REG8L:
            return f"{REG8L[r]} = ({REG8L[r]} & ~0xffu) | (u8)({val});"
        if r in REG8H:
            return f"{REG8H[r]} = ({REG8H[r]} & ~0xff00u) | ((u32)(u8)({val}) << 8);"
        if r.startswith("mm"):
            return f"c->mm[{r[2]}] = {val};"
        raise Unsupported(f"register {r}")

    # ---- control flow -------------------------------------------------------------
    def goto(self, fn, tgt):
        """Jump to tgt: a label in this function, or a tail call."""
        if fn.inside(tgt) and tgt in fn.ins:
            return f"goto L_{tgt:x};"
        # leaving the function by a jump: flags may still be live, pass them along
        if tgt in self.starts:
            return f"{{ SYNC; FSYNC; F_{tgt:x}(c); return; }}"
        # into the middle of other code: the caller's return address is already on the stack
        return f"{{ SYNC; FSYNC; dispatch(c, 0x{tgt:x}u); return; }}"

    def call(self, i, target_expr, direct=None):
        nxt = i.address + i.size
        push = f"esp -= 4; wr32(esp, 0x{nxt:x}u); SYNC;"
        if direct is not None and direct in self.starts:
            return f"{{ {push} F_{direct:x}(c); LOAD; }}"
        return f"{{ u32 t = {target_expr}; {push} dispatch(c, t); LOAD; }}"

    # ---- instructions ---------------------------------------------------------------
    def flags_zsp(self, r, size):
        return f"ZF = ({r}) == 0; SF = ({r}) >> {8 * size - 1}; PF = PAR({r});"

    def alu(self, i, m):
        d, s = i.operands
        n = d.size
        t, bits = T[n], 8 * n - 1
        a, b = self.rd(i, d), self.rd(i, s, n)
        if m in ("add", "adc"):
            carry = "+ cin" if m == "adc" else ""
            cf = "CF = cin ? r <= x : r < x;" if m == "adc" else "CF = r < x;"
            return (f"{{ u32 cin = CF; {t} x = {a}, y = {b}, r = ({t})(x + y {carry}); {cf} "
                    f"OF = ((x ^ r) & (y ^ r)) >> {bits}; {self.flags_zsp('r', n)} {self.wr(i, d, 'r')} (void)cin; }}")
        if m in ("sub", "sbb", "cmp"):
            borrow = "- cin" if m == "sbb" else ""
            cf = "CF = cin ? x <= y : x < y;" if m == "sbb" else "CF = x < y;"
            store = "" if m == "cmp" else self.wr(i, d, "r")
            return (f"{{ u32 cin = CF; {t} x = {a}, y = {b}, r = ({t})(x - y {borrow}); {cf} "
                    f"OF = ((x ^ y) & (x ^ r)) >> {bits}; {self.flags_zsp('r', n)} {store} (void)cin; }}")
        op = {"and": "&", "or": "|", "xor": "^", "test": "&"}[m]
        if m == "xor" and d.type == x86.X86_OP_REG and s.type == x86.X86_OP_REG and d.reg == s.reg:
            return f"{self.wr(i, d, '0')} CF = OF = SF = 0; ZF = 1; PF = 1;"
        store = "" if m == "test" else self.wr(i, d, "r")
        return f"{{ {t} r = ({t})({a} {op} {b}); CF = OF = 0; {self.flags_zsp('r', n)} {store} }}"

    def shift(self, i, m):
        d = i.operands[0]
        n, t, st = d.size, T[d.size], S[d.size]
        w = 8 * n
        cnt = self.rd(i, i.operands[1], 1) if len(i.operands) > 1 else "1"
        a = self.rd(i, d)
        body = {
            "shl": f"r = ({t})(x << k); CF = k <= {w} ? (x >> ({w} - k)) & 1 : 0; OF = (r >> {w - 1}) ^ CF;",
            "shr": f"r = ({t})(x >> k); CF = (x >> (k - 1)) & 1; OF = x >> {w - 1};",
            "sar": f"r = ({t})(({st})x >> (k < {w} ? k : {w - 1})); CF = (({st})x >> (k - 1 < {w} ? k - 1 : {w - 1})) & 1; OF = 0;",
            "rol": f"k &= {w - 1}; r = ({t})((x << k) | (x >> (({w} - k) & {w - 1}))); CF = r & 1; OF = (r >> {w - 1}) ^ CF;",
            "ror": f"k &= {w - 1}; r = ({t})((x >> k) | (x << (({w} - k) & {w - 1}))); CF = r >> {w - 1}; OF = (r >> {w - 1}) ^ ((r >> {w - 2}) & 1);",
            "rcr": f"r = x; while (k--) {{ u32 lo = r & 1; r = ({t})((r >> 1) | (CF << {w - 1})); CF = lo; }} OF = (r >> {w - 1}) ^ ((r >> {w - 2}) & 1);",
            "rcl": f"r = x; while (k--) {{ u32 hi = r >> {w - 1}; r = ({t})((r << 1) | CF); CF = hi; }} OF = (r >> {w - 1}) ^ CF;",
        }[m]
        zsp = "" if m in ("rol", "ror", "rcr", "rcl") else self.flags_zsp("r", n)
        return f"{{ u32 k = {cnt} & 31; if (k) {{ {t} x = {a}, r; {body} {zsp} {self.wr(i, d, 'r')} }} }}"

    def shxd(self, i, m):
        d, s, k = i.operands
        a, b, cnt = self.rd(i, d), self.rd(i, s), self.rd(i, k, 1)
        if m == "shld":
            body = "r = (x << k) | (y >> (32 - k)); CF = (x >> (32 - k)) & 1;"
        else:
            body = "r = (x >> k) | (y << (32 - k)); CF = (x >> (k - 1)) & 1;"
        return (f"{{ u32 k = {cnt} & 31; if (k) {{ u32 x = {a}, y = {b}, r; {body} OF = 0; "
                f"{self.flags_zsp('r', 4)} {self.wr(i, d, 'r')} }} }}")

    def string_op(self, i):
        b = list(i.bytes)
        rep, size = None, 4
        while b[0] in (0x66, 0xF2, 0xF3, 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65):
            if b[0] == 0x66:
                size = 2
            elif b[0] in (0xF2, 0xF3):
                rep = b[0]
            b.pop(0)
        op = b[0]
        if op & 1 == 0:
            size = 1
        t, w = T[size], 8 * size
        step = f"(c->df ? -{size} : {size})"
        kind = {0xA4: "movs", 0xA5: "movs", 0xAA: "stos", 0xAB: "stos", 0xAC: "lods", 0xAD: "lods",
                0xAE: "scas", 0xAF: "scas", 0xA6: "cmps", 0xA7: "cmps"}[op]
        acc = {1: "(u8)eax", 2: "(u16)eax", 4: "eax"}[size]
        set_acc = {1: "eax = (eax & ~0xffu) | v;", 2: "eax = (eax & 0xffff0000u) | v;", 4: "eax = v;"}[size]
        cmp = f"{{ {t} r = ({t})(x - y); CF = x < y; OF = ((x ^ y) & (x ^ r)) >> {w - 1}; {self.flags_zsp('r', size)} }}"
        one = {
            "movs": f"wr{w}(edi, rd{w}(esi)); esi += {step}; edi += {step};",
            "stos": f"wr{w}(edi, {acc}); edi += {step};",
            "lods": f"{{ {t} v = rd{w}(esi); {set_acc} }} esi += {step};",
            "scas": f"{{ {t} x = {acc}, y = rd{w}(edi); {cmp} }} edi += {step};",
            "cmps": f"{{ {t} x = rd{w}(esi), y = rd{w}(edi); {cmp} }} esi += {step}; edi += {step};",
        }[kind]
        if rep is None:
            return one
        if kind in ("movs", "stos", "lods"):
            if kind == "movs" and size == 4:
                fast = (f"if (!c->df && (edi >= esi + ecx * 4 || esi >= edi + ecx * 4)) "
                        f"{{ memmove(MEM + edi, MEM + esi, ecx * 4); esi += ecx * 4; edi += ecx * 4; ecx = 0; }} ")
                return fast + f"while (ecx) {{ {one} ecx--; }}"
            return f"while (ecx) {{ {one} ecx--; }}"
        stop = "!ZF" if rep == 0xF3 else "ZF"  # repe stops on difference, repne on equality
        return f"while (ecx) {{ {one} ecx--; if ({stop}) break; }}"

    def x87(self, fn, i):
        return re.sub(r"\bST\((\w+)\) = ", r"STW(\1) = ", self.x87_raw(fn, i))

    def x87_raw(self, fn, i):
        """x87 decoded from the opcode bytes (capstone's names for the reversed
        forms are easy to misread)."""
        b = list(i.bytes)
        while b[0] in (0x66, 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x9B):
            b.pop(0)
        op, modrm = b[0], b[1]
        reg, rm = (modrm >> 3) & 7, modrm & 7
        mem = next((o for o in i.operands if o.type == x86.X86_OP_MEM), None)
        A = self.addr(i, mem) if mem is not None else None
        arith = ["+", "*", None, None, "-", "-r", "/", "/r"]
        if op in (0xD8, 0xDA, 0xDC, 0xDE) and modrm < 0xC0:
            src = {0xD8: f"(double)rdf32({A})", 0xDC: f"rdf64({A})",
                   0xDA: f"(double)(s32)rd32({A})", 0xDE: f"(double)(s16)rd16({A})"}[op]
            if reg in (2, 3):
                return f"rt_fcom(c, ST(0), {src});" + (" FPOP();" if reg == 3 else "")
            o = arith[reg]
            if o.endswith("r"):
                return f"ST(0) = {src} {o[0]} ST(0);"
            return f"ST(0) = ST(0) {o} {src};"
        if op == 0xD8:  # st0 = st0 op st(i)
            if reg in (2, 3):
                return f"rt_fcom(c, ST(0), ST({rm}));" + (" FPOP();" if reg == 3 else "")
            o = arith[reg]
            return f"ST(0) = ST({rm}) {o[0]} ST(0);" if o.endswith("r") else f"ST(0) = ST(0) {o} ST({rm});"
        if op in (0xDC, 0xDE) and modrm >= 0xC0:  # st(i) = st(i) op st0 (sub/div reversed in encoding)
            pop = " FPOP();" if op == 0xDE else ""
            if op == 0xDE and modrm == 0xD9:
                return "rt_fcom(c, ST(0), ST(1)); FPOP(); FPOP();"
            if reg in (2, 3):
                return f"rt_fcom(c, ST(0), ST({rm}));" + (" FPOP();" if reg == 3 else "") + pop
            expr = {0: f"ST({rm}) + ST(0)", 1: f"ST({rm}) * ST(0)", 4: f"ST(0) - ST({rm})", 5: f"ST({rm}) - ST(0)",
                    6: f"ST(0) / ST({rm})", 7: f"ST({rm}) / ST(0)"}[reg]
            return f"ST({rm}) = {expr};{pop}"
        if modrm < 0xC0:
            key = (op, reg)
            table = {
                (0xD9, 0): f"FPUSH((double)rdf32({A}));", (0xDD, 0): f"FPUSH(rdf64({A}));",
                (0xDB, 5): f"FPUSH(rt_ld80({A}));",
                (0xD9, 2): f"wrf32({A}, (float)ST(0));", (0xD9, 3): f"wrf32({A}, (float)ST(0)); FPOP();",
                (0xDD, 2): f"wrf64({A}, ST(0));", (0xDD, 3): f"wrf64({A}, ST(0)); FPOP();",
                (0xDB, 7): f"rt_st80({A}, ST(0)); FPOP();",
                (0xDF, 0): f"FPUSH((double)(s16)rd16({A}));", (0xDB, 0): f"FPUSH((double)(s32)rd32({A}));",
                (0xDF, 5): f"FPUSHI(rd64({A}));",
                (0xDF, 2): f"{{ s64 v = rt_fist(c, ST(0)); wr16({A}, (v >= -32768 && v <= 32767) ? (u16)v : 0x8000u); }}",
                (0xDF, 3): f"{{ s64 v = rt_fist(c, ST(0)); wr16({A}, (v >= -32768 && v <= 32767) ? (u16)v : 0x8000u); FPOP(); }}",
                (0xDB, 2): f"{{ s64 v = rt_fist(c, ST(0)); wr32({A}, (v >= INT32_MIN && v <= INT32_MAX) ? (u32)v : 0x80000000u); }}",
                (0xDB, 3): f"{{ s64 v = rt_fist(c, ST(0)); wr32({A}, (v >= INT32_MIN && v <= INT32_MAX) ? (u32)v : 0x80000000u); FPOP(); }}",
                (0xDF, 7): f"wr64({A}, (u64)FIST64(ST(0))); FPOP();",
                (0xD9, 5): f"c->cw = rd16({A});", (0xD9, 7): f"wr16({A}, c->cw);",
                (0xDD, 7): f"wr16({A}, (u16)(c->sw | ((c->top & 7) << 11)));",
            }
            if key in table:
                return table[key]
            raise Unsupported(f"x87 {op:02x} /{reg} mem")
        if op == 0xD9:
            if 0xC0 <= modrm <= 0xC7:
                return f"FPUSHR({rm});"
            if 0xC8 <= modrm <= 0xCF:
                return f"FXCH({rm});"
            simple = {0xE0: "ST(0) = -ST(0);", 0xE1: "ST(0) = fabs(ST(0));", 0xE4: "rt_fcom(c, ST(0), 0.0);",
                      0xE5: "rt_fxam(c);", 0xE8: "FPUSH(1.0);", 0xE9: "FPUSH(M_LN10 / M_LN2);",
                      0xEA: "FPUSH(M_LOG2E);", 0xEB: "FPUSH(M_PI);", 0xEC: "FPUSH(M_LN2 / M_LN10);",
                      0xED: "FPUSH(M_LN2);", 0xEE: "FPUSH(0.0);", 0xF0: "ST(0) = exp2(ST(0)) - 1.0;",
                      0xF1: "ST(1) = ST(1) * log2(ST(0)); FPOP();", 0xF2: "ST(0) = tan(ST(0)); FPUSH(1.0); c->sw &= ~0x400;",
                      0xF3: "ST(1) = atan2(ST(1), ST(0)); FPOP();",
                      0xF4: "{ double x = ST(0); int e = ilogb(x); ST(0) = (double)e; FPUSH(scalbn(x, -e)); }",
                      0xF8: "ST(0) = fmod(ST(0), ST(1)); c->sw &= ~0x400;", 0xF5: "ST(0) = remainder(ST(0), ST(1)); c->sw &= ~0x400;",
                      0xFA: "ST(0) = sqrt(ST(0));",
                      0xFB: "{ double x = ST(0); ST(0) = sin(x); FPUSH(cos(x)); c->sw &= ~0x400; }",
                      0xFC: "ST(0) = rt_frnd(c, ST(0));", 0xFD: "ST(0) = ldexp(ST(0), (int)trunc(ST(1)));",
                      0xFE: "ST(0) = sin(ST(0)); c->sw &= ~0x400;", 0xFF: "ST(0) = cos(ST(0)); c->sw &= ~0x400;",
                      0xD0: ""}
            if modrm in simple:
                return simple[modrm]
        if op == 0xDD:
            if 0xD0 <= modrm <= 0xD7:
                return f"FCOPY({rm}, 0);"
            if 0xD8 <= modrm <= 0xDF:
                return f"FCOPY({rm}, 0); FPOP();"
            if 0xC0 <= modrm <= 0xC7:
                return ""  # ffree
            if 0xE0 <= modrm <= 0xE7:
                return f"rt_fcom(c, ST(0), ST({rm}));"
            if 0xE8 <= modrm <= 0xEF:
                return f"rt_fcom(c, ST(0), ST({rm})); FPOP();"
        if op == 0xDA and modrm == 0xE9:
            return "rt_fcom(c, ST(0), ST(1)); FPOP(); FPOP();"
        if op == 0xDB:
            if modrm == 0xE2:
                return "c->sw &= 0x7f00;"
            if modrm == 0xE3:
                return "c->sw = 0; c->top = 0; c->cw = 0x37f;"
            if 0xE8 <= modrm <= 0xF7:
                return (f"{{ double a = ST(0), b = ST({rm}); if (a != a || b != b) ZF = PF = CF = 1; "
                        f"else {{ ZF = a == b; CF = a < b; PF = 0; }} OF = SF = 0; }}")
        if op == 0xDF:
            if modrm == 0xE0:
                return "eax = (eax & 0xffff0000u) | (u16)(c->sw | ((c->top & 7) << 11));"
            if 0xE8 <= modrm <= 0xF7:
                return (f"{{ double a = ST(0), b = ST({rm}); if (a != a || b != b) ZF = PF = CF = 1; "
                        f"else {{ ZF = a == b; CF = a < b; PF = 0; }} OF = SF = 0; FPOP(); }}")
        raise Unsupported(f"x87 {op:02x} {modrm:02x}")

    def insn(self, fn, i):
        m = i.mnemonic
        ops = i.operands
        if m.startswith("lock "):
            m = m[5:]
        if m in ("nop", "wait", "fwait", "emms", "cli", "sti", "fnop") or m.startswith("prefetch"):
            return ""
        if i.bytes[0] in (0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF) or (
                i.bytes[0] in (0x9B, 0x66) and len(i.bytes) > 1 and 0xD8 <= i.bytes[1] <= 0xDF):
            return self.x87(fn, i)
        if m in ("mov", "movq", "movd"):
            return self.wr(i, ops[0], self.rd(i, ops[1], ops[0].size))
        if m in ("movsx", "movzx"):
            t = S[ops[1].size] if m == "movsx" else T[ops[1].size]
            return self.wr(i, ops[0], f"({S[ops[0].size] if m == 'movsx' else T[ops[0].size]})({t}){self.rd(i, ops[1])}")
        if m == "lea":
            return self.wr(i, ops[0], self.addr(i, ops[1]))
        if m in ("add", "adc", "sub", "sbb", "cmp", "and", "or", "xor", "test"):
            return self.alu(i, m)
        if m in ("inc", "dec"):
            d = ops[0]
            t, bits = T[d.size], 8 * d.size - 1
            o = "+" if m == "inc" else "-"
            ovf = f"r == ({t})(1u << {bits})" if m == "inc" else f"x == ({t})(1u << {bits})"
            return f"{{ {t} x = {self.rd(i, d)}, r = ({t})(x {o} 1); OF = {ovf}; {self.flags_zsp('r', d.size)} {self.wr(i, d, 'r')} }}"
        if m == "neg":
            d = ops[0]
            t, bits = T[d.size], 8 * d.size - 1
            return (f"{{ {t} x = {self.rd(i, d)}, r = ({t})(0 - x); CF = x != 0; OF = x == ({t})(1u << {bits}); "
                    f"{self.flags_zsp('r', d.size)} {self.wr(i, d, 'r')} }}")
        if m == "not":
            return self.wr(i, ops[0], f"~{self.rd(i, ops[0])}")
        if m in ("shl", "sal", "shr", "sar", "rol", "ror", "rcr", "rcl"):
            return self.shift(i, "shl" if m == "sal" else m)
        if m in ("shld", "shrd"):
            return self.shxd(i, m)
        if m == "push":
            v = self.rd(i, ops[0], 4)
            if ops[0].size == 2 and ops[0].type != x86.X86_OP_IMM:
                return f"{{ u16 v = {self.rd(i, ops[0])}; esp -= 2; wr16(esp, v); }}"
            return f"{{ u32 v = {v}; esp -= 4; wr32(esp, v); }}"
        if m == "pop":
            if ops[0].size == 2:
                return f"{{ u16 v = rd16(esp); esp += 2; {self.wr(i, ops[0], 'v')} }}"
            return f"{{ u32 v = rd32(esp); esp += 4; {self.wr(i, ops[0], 'v')} }}"
        if m == "pushal":
            return ("{ u32 s = esp; esp -= 32; wr32(esp + 28, eax); wr32(esp + 24, ecx); wr32(esp + 20, edx); "
                    "wr32(esp + 16, ebx); wr32(esp + 12, s); wr32(esp + 8, ebp); wr32(esp + 4, esi); wr32(esp, edi); }")
        if m == "popal":
            return ("{ edi = rd32(esp); esi = rd32(esp + 4); ebp = rd32(esp + 8); ebx = rd32(esp + 16); "
                    "edx = rd32(esp + 20); ecx = rd32(esp + 24); eax = rd32(esp + 28); esp += 32; }")
        if m == "pushfd":
            return ("{ u32 v = CF | (PF << 2) | (ZF << 6) | (SF << 7) | (c->df << 10) | (OF << 11) | 0x202u; "
                    "esp -= 4; wr32(esp, v); }")
        if m == "popfd":
            return ("{ u32 v = rd32(esp); esp += 4; CF = v & 1; PF = (v >> 2) & 1; ZF = (v >> 6) & 1; "
                    "SF = (v >> 7) & 1; c->df = (v >> 10) & 1; OF = (v >> 11) & 1; }")
        if m == "lahf":
            return "eax = (eax & ~0xff00u) | ((SF << 15) | (ZF << 14) | (PF << 10) | 0x200u | (CF << 8));"
        if m == "sahf":
            return "{ u32 h = eax >> 8; CF = h & 1; PF = (h >> 2) & 1; ZF = (h >> 6) & 1; SF = (h >> 7) & 1; }"
        if m in ("clc", "stc", "cmc"):
            return {"clc": "CF = 0;", "stc": "CF = 1;", "cmc": "CF ^= 1;"}[m]
        if m in ("cld", "std"):
            return f"c->df = {int(m == 'std')};"
        if m == "xchg":
            a, b = self.rd(i, ops[0]), self.rd(i, ops[1])
            return f"{{ {T[ops[0].size]} x = {a}, y = {b}; {self.wr(i, ops[0], 'y')} {self.wr(i, ops[1], 'x')} }}"
        if m == "bswap":
            return self.wr(i, ops[0], f"__builtin_bswap32({self.rd(i, ops[0])})")
        if m in ("bt", "bts", "btr", "btc") and ops[0].type == x86.X86_OP_MEM and ops[1].type == x86.X86_OP_REG:
            # a register bit offset addresses a bit string: it can reach past the dword
            upd = {"bt": "", "bts": "wr32(p, x | (1u << k));", "btr": "wr32(p, x & ~(1u << k));",
                   "btc": "wr32(p, x ^ (1u << k));"}[m]
            n = self.rd(i, ops[1])
            return (f"{{ u32 p = {self.addr(i, ops[0])} + (u32)(((s32){n} >> 5) * 4), k = {n} & 31, x = rd32(p); "
                    f"CF = (x >> k) & 1; {upd} }}")
        if m in ("bt", "bts", "btr", "btc"):
            w = 8 * ops[0].size
            x = self.rd(i, ops[0])
            k = f"({self.rd(i, ops[1])} & {w - 1})"
            upd = {"bt": "", "bts": self.wr(i, ops[0], "x | (1u << k)"), "btr": self.wr(i, ops[0], "x & ~(1u << k)"),
                   "btc": self.wr(i, ops[0], "x ^ (1u << k)")}[m]
            return f"{{ u32 x = {x}, k = {k}; CF = (x >> k) & 1; {upd} }}"
        if m == "cdq":
            return "edx = (u32)((s32)eax >> 31);"
        if m == "cwde":
            return "eax = (u32)(s32)(s16)eax;"
        if m == "cbw":
            return "eax = (eax & 0xffff0000u) | (u16)(s16)(s8)eax;"
        if m == "cwd":
            return "edx = (edx & 0xffff0000u) | (((s16)eax < 0) ? 0xffffu : 0);"
        if m == "xlatb":
            return "eax = (eax & ~0xffu) | rd8(ebx + (u8)eax);"
        if m in ("mul", "imul") and len(ops) == 1:
            s, sg = ops[0], m == "imul"
            n = s.size
            v = self.rd(i, s)
            if n == 1:
                p = f"(s16)(s8)eax * (s16)(s8){v}" if sg else f"(u16)((u8)eax * (u8){v})"
                ovf = "(s16)p != (s8)p" if sg else "(p >> 8) != 0"
                return f"{{ u16 p = (u16)({p}); eax = (eax & 0xffff0000u) | p; CF = OF = {ovf}; }}"
            if n == 4:
                p = f"(u64)((s64)(s32)eax * (s32){v})" if sg else f"(u64)eax * {v}"
                ovf = "(s64)p != (s32)p" if sg else "edx != 0"
                return f"{{ u64 p = {p}; eax = (u32)p; edx = (u32)(p >> 32); CF = OF = {ovf}; }}"
            raise Unsupported("16-bit mul")
        if m == "imul":
            d = ops[0]
            a, b = (self.rd(i, ops[0]), self.rd(i, ops[1], d.size)) if len(ops) == 2 else (self.rd(i, ops[1]), self.rd(i, ops[2], d.size))
            st = S[d.size]
            return (f"{{ s64 p = (s64)({st}){a} * ({st}){b}; {T[d.size]} r = ({T[d.size]})p; "
                    f"CF = OF = p != ({st})r; {self.wr(i, d, 'r')} }}")
        if m in ("div", "idiv"):
            s, n = ops[0], ops[0].size
            v = self.rd(i, s)
            if n == 4:
                if m == "div":
                    return (f"{{ u32 d = {v}; u64 x = ((u64)edx << 32) | eax; if (!d) rt_trap(c, 0x{i.address:x}u, \"div0\"); "
                            f"eax = (u32)(x / d); edx = (u32)(x % d); }}")
                return (f"{{ s32 d = (s32){v}; s64 x = (s64)(((u64)edx << 32) | eax); if (!d) rt_trap(c, 0x{i.address:x}u, \"div0\"); "
                        f"eax = (u32)(s32)(x / d); edx = (u32)(s32)(x % d); }}")
            if n == 1:
                if m == "div":
                    return (f"{{ u8 d = {v}; u16 x = (u16)eax; if (!d) rt_trap(c, 0x{i.address:x}u, \"div0\"); "
                            f"eax = (eax & 0xffff0000u) | ((u32)(u8)(x % d) << 8) | (u8)(x / d); }}")
                return (f"{{ s8 d = (s8){v}; s16 x = (s16)eax; if (!d) rt_trap(c, 0x{i.address:x}u, \"div0\"); "
                        f"eax = (eax & 0xffff0000u) | ((u32)(u8)(x % d) << 8) | (u8)(x / d); }}")
            if n == 2:
                if m == "div":
                    return (f"{{ u16 d = {v}; u32 x = ((edx & 0xffff) << 16) | (eax & 0xffff); if (!d) rt_trap(c, 0x{i.address:x}u, \"div0\"); "
                            f"eax = (eax & 0xffff0000u) | (u16)(x / d); edx = (edx & 0xffff0000u) | (u16)(x % d); }}")
                return (f"{{ s16 d = (s16){v}; s32 x = (s32)(((edx & 0xffff) << 16) | (eax & 0xffff)); if (!d) rt_trap(c, 0x{i.address:x}u, \"div0\"); "
                        f"eax = (eax & 0xffff0000u) | (u16)(x / d); edx = (edx & 0xffff0000u) | (u16)(x % d); }}")
        if m.startswith("set"):
            return self.wr(i, ops[0], f"({cond(m[3:])} ? 1 : 0)")
        if m.startswith("cmov"):
            return f"if ({cond(m[4:])}) {self.wr(i, ops[0], self.rd(i, ops[1]))}"
        op1 = next(x for x in i.bytes if x not in (0x66, 0xF2, 0xF3, 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65))
        if op1 in (0xA4, 0xA5, 0xA6, 0xA7, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF):
            return self.string_op(i)
        if m == "leave":
            return "esp = ebp; ebp = rd32(esp); esp += 4;"
        if m == "enter":
            return f"esp -= 4; wr32(esp, ebp); ebp = esp; esp -= {ops[0].imm};" if ops[1].imm == 0 else "rt_unimpl(c, 0, \"enter\");"
        if m == "cpuid":
            return "SYNC; rt_cpuid(c); LOAD;"
        if m == "rdtsc":
            return "SYNC; rt_rdtsc(c); LOAD;"
        if m == "in":
            n = ops[0].size
            port = f"0x{ops[1].imm:x}u" if ops[1].type == x86.X86_OP_IMM else "(u16)edx"
            return self.wr(i, ops[0], f"rt_in(c, {port}, {n})")
        if m == "out":
            port = f"0x{ops[0].imm:x}u" if ops[0].type == x86.X86_OP_IMM else "(u16)edx"
            return f"rt_out(c, {port}, {self.rd(i, ops[1])}, {ops[1].size});"
        # control flow
        nxt = i.address + i.size
        if i.address in self.dyn_rel:
            t = f"(u32)(0x{nxt:x}u + (u32){self.field(*self.dyn_rel[i.address], 4)})"
            if m == "call":
                return self.call(i, t)
            jump = f"{{ u32 t = {t}; {self.jump_dyn(fn, 't')} }}"
            if m == "jmp":
                return jump
            cc = {"jecxz": "!ecx", "jcxz": "!(u16)ecx"}.get(m) or cond(m[1:])
            return f"if ({cc}) {jump}"
        if m == "call":
            op = ops[0]
            if op.type == x86.X86_OP_IMM:
                return self.call(i, f"0x{op.imm & 0xFFFFFFFF:x}u", op.imm & 0xFFFFFFFF)
            return self.call(i, self.rd(i, op, 4))
        if m == "ret":
            k = 4 + (ops[0].imm if ops else 0)
            return f"esp += {k}; SYNC; return;"
        if m == "jmp":
            op = ops[0]
            if op.type == x86.X86_OP_IMM:
                return self.goto(fn, op.imm & 0xFFFFFFFF)
            if i.address in fn.tables:
                cases = " ".join(f"case 0x{t:x}u: {self.goto(fn, t)}" for t in sorted(set(fn.tables[i.address])))
                return (f"{{ u32 t = {self.rd(i, op, 4)}; switch (t) {{ {cases} default: {self.jump_dyn(fn, 't')} }} }}")
            # tail jump through a pointer (import thunks, function pointers): the caller's
            # return address is already on the stack
            return f"{{ u32 t = {self.rd(i, op, 4)}; SYNC; FSYNC; dispatch(c, t); return; }}"
        if m.startswith("j"):
            tgt = ops[0].imm & 0xFFFFFFFF
            cc = {"jecxz": "!ecx", "jcxz": "!(u16)ecx"}.get(m) or cond(m[1:])
            return f"if ({cc}) {self.goto(fn, tgt)}"
        if m.startswith("loop"):
            tgt = ops[0].imm & 0xFFFFFFFF
            extra = {"loop": "", "loope": " && ZF", "loopne": " && !ZF"}[m]
            return f"ecx--; if (ecx{extra}) {self.goto(fn, tgt)}"
        if m in ("int3", "int1", "hlt", "int", "into", "ud2"):
            return f"SYNC; rt_trap(c, 0x{i.address:x}u, \"{m}\"); return;"
        raise Unsupported(m)

    def jump_dyn(self, fn, t):
        """Jump to a target only known at run time, through the function's one shared switch."""
        fn.dyn = True
        return f"{{ dyn_t = {t}; goto dyn; }}"

    def function(self, fn):
        body = []
        addrs = sorted(fn.ins)
        for k, a in enumerate(addrs):
            i = fn.ins[a]
            body.append((a, None))
            try:
                code = self.insn(fn, i)
                for v in self.variants.get(a) or []:
                    b = "".join(f"\\x{x:02x}" for x in v.bytes)
                    code = (f"if (!memcmp(MEM + 0x{a:x}u, \"{b}\", {v.size})) {{ {self.insn(fn, v)} /* {v.mnemonic} {v.op_str} */ }} "
                            f"else {{ {code} }}")
            except Unsupported as e:
                self.unsupported[str(e)] = self.unsupported.get(str(e), 0) + 1
                code = f"SYNC; rt_unimpl(c, 0x{a:x}u, \"{i.mnemonic} {i.op_str}\"); return;"
            body.append((a, f"    TR(0x{a:x}u); {code} /* {a:x}: {i.mnemonic} {i.op_str.replace('*/', '* /')} */"))
            m = i.mnemonic
            ends = m in ("jmp", "ret", "retf", "iretd", "int3", "hlt") or code.endswith("return;")
            nxt = a + i.size
            if not ends and (k + 1 == len(addrs) or addrs[k + 1] != nxt):
                body.append((a, f"    {self.goto(fn, nxt)} /* falls through */"))
        used = set(re.findall(r"goto L_([0-9a-f]+);", "\n".join(l for _, l in body if l)))
        if getattr(fn, "dyn", False):
            used.update(f"{a:x}" for a in fn.ins)
        out = [f"void F_{fn.entry:x}(Cpu *c) /* {fn.name} */", "{", "    REGS;", "    LOAD;"]
        if not fn.ins:
            out.append(f"    SYNC; rt_trap(c, 0x{fn.entry:x}u, \"no code\");")
        elif fn.entry != min(fn.ins):
            out.append(f"    goto L_{fn.entry:x};")
            used.add(f"{fn.entry:x}")
        for a, line in body:
            if line is None:
                if f"{a:x}" in used:
                    out.append(f"L_{a:x}:;")
            else:
                out.append(line)
        if getattr(fn, "dyn", False):
            out.insert(4, "    u32 dyn_t;")
            cases = " ".join(f"case 0x{a:x}u: goto L_{a:x};" for a in sorted(fn.ins))
            out.append(f"dyn: switch (dyn_t) {{ {cases} default: SYNC; FSYNC; dispatch(c, dyn_t); return; }}")
        out.append("}")
        return fn, "\n".join(out)


def main():
    g = Gen()
    g.build()
    want = {int(a, 16) for a in sys.argv[1:]}
    funcs = [fn for e, fn in sorted(g.fns.items()) if not want or e in want]
    os.makedirs(OUT, exist_ok=True)
    for old in os.listdir(OUT):
        os.remove(os.path.join(OUT, old))
    head = "#include <math.h>\n#include \"cpu.h\"\n#include \"protos.h\"\n"
    chunk, n, files = [], 0, []
    bodies = []
    for fn in funcs:
        _, src = g.function(fn)
        bodies.append(src)
    per = max(1, len(bodies) // 40 + 1)
    for k in range(0, len(bodies), per):
        name = os.path.join(OUT, f"gen_{k // per:02d}.c")
        open(name, "w").write(head + "\n\n".join(bodies[k:k + per]) + "\n")
        files.append(name)
    with open(os.path.join(OUT, "protos.h"), "w") as h:
        for e in sorted(g.fns):
            h.write(f"void F_{e:x}(Cpu *c);\n")
    with open(os.path.join(OUT, "table.c"), "w") as t:
        t.write("#include \"cpu.h\"\n#include \"protos.h\"\n")
        t.write("const struct { u32 addr; void (*fn)(Cpu *); } recomp_table[] = {\n")
        for e in sorted(g.fns):
            if not want or e in want:
                t.write(f"    {{0x{e:x}u, F_{e:x}}},\n")
        t.write("};\nconst unsigned recomp_table_len = sizeof recomp_table / sizeof recomp_table[0];\n")
    import pefile
    pe = pefile.PE(exe())
    with open(os.path.join(OUT, "imports.c"), "w") as t:
        t.write("#include \"cpu.h\"\nconst struct { u32 iat; const char *dll, *name; } recomp_imports[] = {\n")
        n = 0
        for d in pe.DIRECTORY_ENTRY_IMPORT:
            for imp in d.imports:
                name = imp.name.decode() if imp.name else f"#{imp.ordinal}"
                t.write(f'    {{0x{imp.address:x}u, "{d.dll.decode()}", "{name}"}},\n')
                n += 1
        t.write(f"}};\nconst unsigned recomp_imports_n = {n};\n")
    print(f"{len(bodies)} functions ({len(g.fns) - len(g.funcs)} extra entry points) -> {len(files)} files in {OUT}")
    if g.unsupported:
        print("unsupported:", ", ".join(f"{k} x{v}" for k, v in sorted(g.unsupported.items(), key=lambda x: -x[1])))


if __name__ == "__main__":
    main()
