import struct

with open('boot.dol', 'rb') as f:
    dol = f.read()

def disasm_simple(pc, instr):
    op = (instr >> 26) & 0x3F
    rD = (instr >> 21) & 0x1F
    rA = (instr >> 16) & 0x1F
    rB = (instr >> 11) & 0x1F
    simm = (instr & 0xFFFF)
    if simm & 0x8000: simm -= 0x10000
    uimm = instr & 0xFFFF
    xo10 = (instr >> 1) & 0x3FF
    
    if op == 14: return f'addi r{rD}, r{rA}, {simm}' if rA != 0 else f'li r{rD}, {simm}'
    if op == 15: return f'addis r{rD}, r{rA}, {hex(uimm)}' if rA != 0 else f'lis r{rD}, {hex(uimm)}'
    if op == 18:
        lk = instr & 1
        li = (instr & 0x03FFFFFC)
        if li & 0x02000000: li |= 0xFC000000
        target = li if (instr & 2) else (pc + li)
        b_name = "bl" if lk else "b"
        return f'{b_name} {hex(target)}'
    if op == 16:
        bd = instr & 0xFFFC
        if bd & 0x8000: bd -= 0x10000
        target = bd if (instr & 2) else (pc + bd)
        return f'bc {hex(target)}'
    if op == 19:
        if xo10 == 16: return 'blr'
        if xo10 == 528: return 'bctrl' if (instr&1) else 'bctr'
        return f'op19_xo{xo10}'
    if op == 31:
        if xo10 == 444: return f'mr r{rA}, r{rD}' if rD == rB else f'or r{rA}, r{rD}, r{rB}'
        if xo10 == 266: return f'add r{rD}, r{rA}, r{rB}'
        if xo10 == 40: return f'subf r{rD}, r{rA}, r{rB}'
        if xo10 == 339: return f'mfspr r{rD}, {rA}'
        if xo10 == 467: return f'mtspr {rA}, r{rD}'
        return f'op31_xo{xo10}'
    if op == 32: return f'lwz r{rD}, {simm}(r{rA})'
    if op == 36: return f'stw r{rD}, {simm}(r{rA})'
    if op == 37: return f'stwu r{rD}, {simm}(r{rA})'
    if op == 24: return f'ori r{rA}, r{rD}, {hex(uimm)}'
    if op == 10: return f'cmpli crf0, r{rA}, {uimm}'
    if op == 11: return f'cmpi crf0, r{rA}, {simm}'
    return f'op_{op}'

for i in range(36):
    pc = 0x80004000 + i * 4
    raw = struct.unpack('>I', dol[0x100 + i*4 : 0x100 + (i+1)*4])[0]
    print(f'{hex(pc)}: {raw:08x}  {disasm_simple(pc, raw)}')
