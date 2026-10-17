"""Read bytes from FCopLAPD.exe by virtual address."""
import struct


class Image:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        nsec, opt = struct.unpack_from("<H", self.data, pe + 6)[0], struct.unpack_from("<H", self.data, pe + 20)[0]
        self.base = struct.unpack_from("<I", self.data, pe + 24 + 28)[0]
        self.sections = []
        for i in range(nsec):
            o = pe + 24 + opt + i * 40
            name = self.data[o:o + 8].rstrip(b"\0").decode("latin1")
            vsize, va, rsize, raw = struct.unpack_from("<IIII", self.data, o + 8)
            self.sections.append((name, self.base + va, vsize, raw, rsize))

    def read(self, addr, n):
        for _, va, vsize, raw, rsize in self.sections:
            if va <= addr < va + max(vsize, rsize):
                off = addr - va
                chunk = self.data[raw + off:raw + min(off + n, rsize)] if off < rsize else b""
                return chunk + b"\0" * (n - len(chunk))  # .bss tail reads as zeros
        return b""
