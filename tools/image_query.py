# Consultas sobre el volcado de la imagen guest (big-endian, base 0x82000000).
import os as _os
ROOT = _os.path.abspath(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..'))
import struct, sys

BASE = 0x82000000
data = open(_os.path.join(ROOT, 'project', 'out', 'build', 'win-amd64-release', 'lo_image_dump.bin'), "rb").read()

def find_u32(value, limit=40):
    pat = struct.pack(">I", value)
    out, i = [], 0
    while len(out) < limit:
        i = data.find(pat, i)
        if i < 0:
            break
        if i % 4 == 0:
            out.append(BASE + i)
        i += 1
    return out

def hexdump_u32(va, count):
    off = va - BASE
    ws = struct.unpack(">%dI" % count, data[off:off + count * 4])
    for k in range(0, count, 8):
        row = " ".join("%08X" % w for w in ws[k:k + 8])
        print("  %08X: %s" % (va + k * 4, row))

def find_utf16be(text, limit=10):
    pat = text.encode("utf-16-be")
    out, i = [], 0
    while len(out) < limit:
        i = data.find(pat, i)
        if i < 0:
            break
        out.append(BASE + i)
        i += 1
    return out

cmd = sys.argv[1]
if cmd == "u32":
    for va in find_u32(int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 40):
        print("0x%08X" % va)
elif cmd == "dump":
    hexdump_u32(int(sys.argv[2], 16), int(sys.argv[3]))
elif cmd == "utf16":
    for va in find_utf16be(sys.argv[2]):
        # contexto: la cadena y vecinas
        off = va - BASE
        chunk = data[off:off + 120]
        s = chunk.decode("utf-16-be", errors="replace").split("\x00")[0]
        print("0x%08X: %s" % (va, s))
elif cmd == "ascii":
    pat = sys.argv[2].encode()
    i, n = 0, 0
    while n < 15:
        i = data.find(pat, i)
        if i < 0:
            break
        start = i
        while start > 0 and 32 <= data[start - 1] < 127:
            start -= 1
        end = i
        while end < len(data) and 32 <= data[end] < 127:
            end += 1
        print("0x%08X: %s" % (BASE + start, data[start:end].decode(errors="replace")))
        i = end
        n += 1
