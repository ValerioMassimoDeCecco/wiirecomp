import struct

with open("boot.dol", "rb") as f:
    dol = f.read()

addr = 0x8002C830
off = 0x100 + (addr - 0x80004000)
for i in range(off, off + 0x500, 4):
    w = struct.unpack(">I", dol[i:i+4])[0]
    cur_addr = 0x80004000 + (i - 0x100)
    op = (w >> 26) & 0x3F
    if w == 0x4E800020:
        print(f"0x{cur_addr:08X}: blr")
    elif op == 18:
        li = (w & 0x03FFFFFC)
        if li & 0x02000000:
            li |= 0xFC000000
        tgt = li if (w & 2) else (cur_addr + li)
        bname = "bl" if (w & 1) else "b"
        print(f"0x{cur_addr:08X}: {bname} 0x{tgt:08X}")
