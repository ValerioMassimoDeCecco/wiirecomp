import struct

with open('boot.dol', 'rb') as f:
    dol = f.read()

MEM1_SIZE = 24 * 1024 * 1024
mem1 = bytearray(MEM1_SIZE)

text_offsets = struct.unpack('>7I', dol[0:28])
data_offsets = struct.unpack('>11I', dol[28:72])
text_addrs = struct.unpack('>7I', dol[72:100])
data_addrs = struct.unpack('>11I', dol[100:144])
text_sizes = struct.unpack('>7I', dol[144:172])
data_sizes = struct.unpack('>11I', dol[172:216])
bss_addr, bss_size, entry = struct.unpack('>3I', dol[216:228])

for i in range(7):
    if text_sizes[i]:
        off, addr, sz = text_offsets[i], text_addrs[i], text_sizes[i]
        m_off = addr & (MEM1_SIZE - 1)
        mem1[m_off : m_off + sz] = dol[off : off + sz]

for i in range(11):
    if data_sizes[i]:
        off, addr, sz = data_offsets[i], data_addrs[i], data_sizes[i]
        m_off = addr & (MEM1_SIZE - 1)
        mem1[m_off : m_off + sz] = dol[off : off + sz]

# Helper memory
def r32(addr):
    if (addr & 0xFF000000) in (0xCC000000, 0xCD000000):
        if addr == 0xCC000000: return 1
        if addr == 0xCC00202C: return 0x100
        return 0
    m_off = addr & (MEM1_SIZE - 1)
    return struct.unpack('>I', mem1[m_off : m_off + 4])[0]

def w32(addr, val):
    val &= 0xFFFFFFFF
    if (addr & 0xFF000000) in (0xCC000000, 0xCD000000):
        return
    m_off = addr & (MEM1_SIZE - 1)
    mem1[m_off : m_off + 4] = struct.pack('>I', val)

def r16(addr):
    m_off = addr & (MEM1_SIZE - 1)
    return struct.unpack('>H', mem1[m_off : m_off + 2])[0]

def w16(addr, val):
    m_off = addr & (MEM1_SIZE - 1)
    mem1[m_off : m_off + 2] = struct.pack('>H', val & 0xFFFF)

def r8(addr):
    m_off = addr & (MEM1_SIZE - 1)
    return mem1[m_off]

def w8(addr, val):
    m_off = addr & (MEM1_SIZE - 1)
    mem1[m_off] = val & 0xFF

# CPU State
r = [0] * 32
pc = entry
lr = 0
ctr = 0
cr = 0
xer = 0
msr = 0

def update_cr0(val):
    global cr
    val = (val & 0xFFFFFFFF)
    cr0 = 0
    if val & 0x80000000: cr0 |= 8
    elif val > 0: cr0 |= 4
    else: cr0 |= 2
    if xer & 0x80000000: cr0 |= 1
    cr = (cr & 0x0FFFFFFF) | (cr0 << 28)

def set_crf(crf, val1, val2, signed=True):
    global cr
    c = 0
    if signed:
        s1 = val1 if val1 < 0x80000000 else val1 - 0x100000000
        s2 = val2 if val2 < 0x80000000 else val2 - 0x100000000
        if s1 < s2: c |= 8
        elif s1 > s2: c |= 4
        else: c |= 2
    else:
        if val1 < val2: c |= 8
        elif val1 > val2: c |= 4
        else: c |= 2
    shift = (7 - crf) * 4
    cr = (cr & ~(0xF << shift)) | ((c & 0xF) << shift)

def eval_cond(bo, bi):
    global ctr
    ctr_ok = True
    if not (bo & 4):
        ctr = (ctr - 1) & 0xFFFFFFFF
        ctr_ok = (ctr == 0) if (bo & 2) else (ctr != 0)
    cond_ok = True
    if not (bo & 16):
        bit = (cr >> (31 - bi)) & 1
        cond_ok = bool(bit) if (bo & 8) else not bool(bit)
    return ctr_ok and cond_ok

def rotl(v, sh):
    sh &= 31
    return ((v << sh) | (v >> (32 - sh))) & 0xFFFFFFFF if sh else v

def ppc_mask(mb, me):
    m = 0
    if mb <= me:
        for i in range(mb, me + 1): m |= (1 << (31 - i))
    else:
        for i in range(0, me + 1): m |= (1 << (31 - i))
        for i in range(mb, 32): m |= (1 << (31 - i))
    return m & 0xFFFFFFFF

# Run simulation
steps = 0
print(f"Starting emulation at 0x{pc:08x}...")
for _ in range(50000):
    steps += 1
    instr = r32(pc)
    op = (instr >> 26) & 0x3F
    rD = (instr >> 21) & 0x1F
    rA = (instr >> 16) & 0x1F
    rB = (instr >> 11) & 0x1F
    simm = instr & 0xFFFF
    if simm & 0x8000: simm -= 0x10000
    uimm = instr & 0xFFFF
    xo10 = (instr >> 1) & 0x3FF
    rc = instr & 1

    next_pc = pc + 4

    if op == 14: # addi / li
        r[rD] = (simm if rA == 0 else (r[rA] + simm)) & 0xFFFFFFFF
    elif op == 15: # addis / lis
        r[rD] = ((uimm << 16) if rA == 0 else (r[rA] + (uimm << 16))) & 0xFFFFFFFF
    elif op == 18: # b, bl
        li = instr & 0x03FFFFFC
        if li & 0x02000000: li |= 0xFC000000
        target = li if (instr & 2) else ((pc + li) & 0xFFFFFFFF)
        if instr & 1: lr = pc + 4
        next_pc = target
    elif op == 16: # bc
        bd = instr & 0xFFFC
        if bd & 0x8000: bd -= 0x10000
        target = bd if (instr & 2) else ((pc + bd) & 0xFFFFFFFF)
        bo = (instr >> 21) & 0x1F
        bi = (instr >> 16) & 0x1F
        if eval_cond(bo, bi):
            if instr & 1: lr = pc + 4
            next_pc = target
    elif op == 19:
        if xo10 == 16: # blr
            next_pc = lr
        elif xo10 == 528: # bctr / bctrl
            if instr & 1: lr = pc + 4
            next_pc = ctr
        elif xo10 == 150: # isync
            pass
        elif xo10 == 193: # crxor
            crD = (instr >> 21) & 0x1F
            crA = (instr >> 16) & 0x1F
            crB = (instr >> 11) & 0x1F
            bA = (cr >> (31 - crA)) & 1
            bB = (cr >> (31 - crB)) & 1
            res = bA ^ bB
            cr = (cr & ~(1 << (31 - crD))) | (res << (31 - crD))
    elif op == 21: # rlwinm
        sh = (instr >> 11) & 0x1F
        mb = (instr >> 6) & 0x1F
        me = (instr >> 1) & 0x1F
        r[rA] = rotl(r[rD], sh) & ppc_mask(mb, me)
        if rc: update_cr0(r[rA])
    elif op == 20: # rlwimi
        sh = (instr >> 11) & 0x1F
        mb = (instr >> 6) & 0x1F
        me = (instr >> 1) & 0x1F
        m = ppc_mask(mb, me)
        r[rA] = (r[rA] & ~m) | (rotl(r[rD], sh) & m)
        if rc: update_cr0(r[rA])
    elif op == 24: # ori
        r[rA] = r[rD] | uimm
    elif op == 25: # oris
        r[rA] = r[rD] | (uimm << 16)
    elif op == 28: # andi.
        r[rA] = r[rD] & uimm
        update_cr0(r[rA])
    elif op == 10: # cmpli
        crf = (instr >> 23) & 7
        set_crf(crf, r[rA], uimm, signed=False)
    elif op == 11: # cmpi
        crf = (instr >> 23) & 7
        set_crf(crf, r[rA], simm & 0xFFFFFFFF, signed=True)
    elif op == 32: # lwz
        ea = ((0 if rA == 0 else r[rA]) + simm) & 0xFFFFFFFF
        r[rD] = r32(ea)
    elif op == 36: # stw
        ea = ((0 if rA == 0 else r[rA]) + simm) & 0xFFFFFFFF
        w32(ea, r[rD])
    elif op == 37: # stwu
        ea = (r[rA] + simm) & 0xFFFFFFFF
        w32(ea, r[rD])
        r[rA] = ea
    elif op == 34: # lbz
        ea = ((0 if rA == 0 else r[rA]) + simm) & 0xFFFFFFFF
        r[rD] = r8(ea)
    elif op == 38: # stb
        ea = ((0 if rA == 0 else r[rA]) + simm) & 0xFFFFFFFF
        w8(ea, r[rD])
    elif op == 40: # lhz
        ea = ((0 if rA == 0 else r[rA]) + simm) & 0xFFFFFFFF
        r[rD] = r16(ea)
    elif op == 44: # sth
        ea = ((0 if rA == 0 else r[rA]) + simm) & 0xFFFFFFFF
        w16(ea, r[rD])
    elif op == 47: # stmw
        ea = ((0 if rA == 0 else r[rA]) + simm) & 0xFFFFFFFF
        for reg in range(rD, 32):
            w32(ea, r[reg])
            ea = (ea + 4) & 0xFFFFFFFF
    elif op == 46: # lmw
        ea = ((0 if rA == 0 else r[rA]) + simm) & 0xFFFFFFFF
        for reg in range(rD, 32):
            r[reg] = r32(ea)
            ea = (ea + 4) & 0xFFFFFFFF
    elif op == 31:
        if xo10 == 0: # cmp
            crf = (instr >> 23) & 7
            set_crf(crf, r[rA], r[rB], signed=True)
        elif xo10 == 32: # cmpl
            crf = (instr >> 23) & 7
            set_crf(crf, r[rA], r[rB], signed=False)
        elif xo10 == 266: # add
            r[rD] = (r[rA] + r[rB]) & 0xFFFFFFFF
            if rc: update_cr0(r[rD])
        elif xo10 == 40: # subf
            r[rD] = (r[rB] - r[rA]) & 0xFFFFFFFF
            if rc: update_cr0(r[rD])
        elif xo10 == 444: # or / mr
            r[rA] = (r[rD] | r[rB]) & 0xFFFFFFFF
            if rc: update_cr0(r[rA])
        elif xo10 == 28: # and
            r[rA] = (r[rD] & r[rB]) & 0xFFFFFFFF
            if rc: update_cr0(r[rA])
        elif xo10 == 316: # xor
            r[rA] = (r[rD] ^ r[rB]) & 0xFFFFFFFF
            if rc: update_cr0(r[rA])
        elif xo10 == 124: # nor
            r[rA] = (~(r[rD] | r[rB])) & 0xFFFFFFFF
            if rc: update_cr0(r[rA])
        elif xo10 == 24: # slw
            sh = r[rB] & 0x3F
            r[rA] = (r[rD] << sh) & 0xFFFFFFFF if sh < 32 else 0
            if rc: update_cr0(r[rA])
        elif xo10 == 536: # srw
            sh = r[rB] & 0x3F
            r[rA] = (r[rD] >> sh) if sh < 32 else 0
            if rc: update_cr0(r[rA])
        elif xo10 == 339: # mfspr
            spr = ((rA & 0x1F) << 5) | ((rA >> 5) & 0x1F)
            if spr == 8: r[rD] = lr
            elif spr == 9: r[rD] = ctr
            elif spr == 1: r[rD] = xer
            else: r[rD] = 0
        elif xo10 == 467: # mtspr
            spr = ((rA & 0x1F) << 5) | ((rA >> 5) & 0x1F)
            if spr == 8: lr = r[rD]
            elif spr == 9: ctr = r[rD]
            elif spr == 1: xer = r[rD]
        elif xo10 == 83: # mfmsr
            r[rD] = msr
        elif xo10 == 146: # mtmsr
            msr = r[rD]
        elif xo10 in (598, 854, 86, 470, 54, 278, 1014, 982, 210, 595): # cache/sync/mmu ops
            pass
        elif xo10 == 23: # lwzx
            ea = ((0 if rA == 0 else r[rA]) + r[rB]) & 0xFFFFFFFF
            r[rD] = r32(ea)
        elif xo10 == 151: # stwx
            ea = ((0 if rA == 0 else r[rA]) + r[rB]) & 0xFFFFFFFF
            w32(ea, r[rD])
        else:
            print(f"Unhandled op31 xo {xo10} at 0x{pc:08x}")
            break
    else:
        print(f"Unhandled op {op} at 0x{pc:08x}")
        break

    pc = next_pc

print(f"Executed {steps} instructions! Final PC: 0x{pc:08x}, LR: 0x{lr:08x}")
