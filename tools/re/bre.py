"""Small capstone helper used to read Buildo.exe.

    pip install capstone pefile
    set BUILDO_EXE=path\\to\\Buildo.exe
    python bre.py str OnSpawn          # find strings and the code that uses them
    python bre.py dis 4316f0 120       # disassemble from an address
    python bre.py xref 4cb9ac          # instructions that mention an address
    python bre.py hex 434418 64        # raw bytes
"""
import os, sys, re, struct, pefile, capstone

EXE = os.environ.get("BUILDO_EXE", "Buildo.exe")
pe = pefile.PE(EXE)
BASE = pe.OPTIONAL_HEADER.ImageBase
data = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = False

text = [s for s in pe.sections if s.Name.startswith(b".text")][0]
TS, TE = BASE + text.VirtualAddress, BASE + text.VirtualAddress + text.Misc_VirtualSize


def rd(va, n):
    return data[va - BASE: va - BASE + n]


def cstr(va):
    off = va - BASE
    if off < 0 or off >= len(data):
        return None
    end = data.find(b"\0", off)
    s = data[off:end]
    if len(s) == 0 or len(s) > 200:
        return None
    try:
        t = s.decode("ascii")
    except Exception:
        return None
    if all(32 <= ord(c) < 127 or c in "\n\r\t" for c in t):
        return t
    return None


def find_str(sub):
    out = []
    for m in re.finditer(re.escape(sub.encode()), data):
        # find start of string
        st = m.start()
        while st > 0 and data[st - 1] != 0:
            st -= 1
        out.append(BASE + st)
    return sorted(set(out))


_all = None


def all_insns():
    global _all
    if _all is None:
        md2 = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        md2.skipdata = True
        _all = list(md2.disasm(rd(TS, TE - TS), TS))
    return _all


def xrefs(va):
    h = "0x%x" % va
    return [i.address for i in all_insns() if h in i.op_str]


def dis(va, n=200, stop_ret=True):
    out = []
    for i in md.disasm(rd(va, n * 8), va):
        ann = ""
        for m in re.finditer(r"0x[0-9a-f]+", i.op_str):
            v = int(m.group(0), 16)
            s = cstr(v)
            if s:
                ann += "  ; " + repr(s)
        out.append("%08x  %-8s %s%s" % (i.address, i.mnemonic, i.op_str, ann))
        n -= 1
        if n <= 0:
            break
    return "\n".join(out)


def func_start(va):
    # walk back to find typical prologue after int3/ret padding
    for a in range(va, va - 0x4000, -1):
        b = rd(a, 3)
        if b[:1] == b"\x55" and b[1:3] == b"\x8b\xec" and rd(a - 1, 1) in (b"\xcc", b"\xc3", b"\x90", b"\x00") :
            return a
        if rd(a - 1, 1) == b"\xcc" and rd(a, 1) != b"\xcc" and rd(a - 2, 1) == b"\xcc":
            return a
    return None


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "str":
        for s in sys.argv[2:]:
            for va in find_str(s):
                print("%08x %r  xrefs=%s" % (va, cstr(va), ["%x" % x for x in xrefs(va)]))
    elif cmd == "dis":
        va = int(sys.argv[2], 16)
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 120
        print(dis(va, n))
    elif cmd == "xref":
        va = int(sys.argv[2], 16)
        for x in xrefs(va):
            print("%08x  in func %s" % (x, hex(func_start(x) or 0)))
    elif cmd == "fs":
        print(hex(func_start(int(sys.argv[2], 16))))
    elif cmd == "hex":
        va = int(sys.argv[2], 16)
        print(rd(va, int(sys.argv[3])).hex())


def callers(target):
    out = []
    t0 = TS - BASE
    seg = data[t0:TE - BASE]
    import struct as _s
    i = seg.find(b"\xe8")
    while i >= 0 and i + 5 <= len(seg):
        rel = _s.unpack_from("<i", seg, i + 1)[0]
        if TS + i + 5 + rel == target:
            out.append(TS + i)
        i = seg.find(b"\xe8", i + 1)
    return out
