#!/usr/bin/env python3
"""
Generatore di un file .DOL di test contenente vere istruzioni PowerPC 750CL (Broadway).
Include:
- Inizializzazione registri (li, addis)
- Aritmetica Paired-Single SIMD (psq_l, ps_add, psq_st)
- Scrittura su WGPIPE (comandi di disegno GPU)
- Loop condizionale e ritorno (blr)
"""

import struct
import sys

def build_test_dol(output_path="test_game.dol"):
    # Layout di memoria del DOL:
    # Text Section 0: caricata a 0x80004000
    # Data Section 0: caricata a 0x80008000
    text_addr = 0x80004000
    data_addr = 0x80008000
    entry_point = text_addr

    # Codifica istruzioni PowerPC 750CL (Big-Endian)
    instructions = [
        # 1. Inizializzazione: li r3, 0 (r3 = 0)
        0x38600000,
        # 2. lis r4, 0xCC00 (r4 = 0xCC000000 -> Base MMIO)
        0x3C80CC00,
        # 3. ori r4, r4, 0x8000 (r4 = 0xCC008000 -> WGPIPE FIFO GPU)
        0x60848000,
        # 4. li r5, 0x80 (Comando GX_TRIANGLES)
        0x38A00080,
        # 5. stw r5, 0(r4) (Invia comando primitiva a WGPIPE)
        0x90A40000,
        
        # 6. Caricamento Paired-Single: psq_l f1, 0(r3), W=0, I=0 (Opcode 56)
        # Formato: 56, fD=1, rA=3, W=0, I=0, d=0 -> 0xE0230000
        0xE0230000,
        # 7. ps_add f2, f1, f1 (f2 = f1 + f1 SIMD) -> Opcode 4, XO=10
        # Formato: 4, fD=2, fA=1, fB=1, XO=10 -> 0x10410828
        0x10410828,
        # 8. psq_st f2, 0(r3), W=0, I=0 (Opcode 60) -> 0xF0430000
        0xF0430000,

        # 9. addi r3, r3, 1 (r3 = r3 + 1)
        0x38630001,
        # 10. cmplwi r3, 10 (Confronta r3 con 10)
        0x2803000A,
        # 11. bc 12, 2, -12 (Se r3 < 10 torna all'istruzione 8) -> bne / blt
        0x4080FFF4,

        # 12. blr (Ritorno da funzione)
        0x4E800020
    ]

    code_bytes = b"".join(struct.pack(">I", instr) for instr in instructions)
    data_bytes = b"WII_NATIVE_PORT_TEST_DATA\x00\x00\x00"

    header_size = 0x100 # 256 bytes

    # Offset nel file .dol
    text_offset = header_size
    data_offset = text_offset + len(code_bytes)
    # Allineamento a 32 bytes
    if len(code_bytes) % 32 != 0:
        pad = 32 - (len(code_bytes) % 32)
        code_bytes += b"\x00" * pad
        data_offset = text_offset + len(code_bytes)

    # Costruzione Header DOL (Big-Endian)
    text_offsets = [text_offset] + [0] * 6
    data_offsets = [data_offset] + [0] * 10
    text_addrs   = [text_addr] + [0] * 6
    data_addrs   = [data_addr] + [0] * 10
    text_sizes   = [len(code_bytes)] + [0] * 6
    data_sizes   = [len(data_bytes)] + [0] * 10

    bss_addr = 0x80009000
    bss_size = 0x1000

    header = bytearray(0x100)

    # Text offsets (7 x 4 bytes)
    for i in range(7):
        struct.pack_into(">I", header, i * 4, text_offsets[i])
    # Data offsets (11 x 4 bytes)
    for i in range(11):
        struct.pack_into(">I", header, 0x1C + i * 4, data_offsets[i])
    # Text addrs (7 x 4 bytes)
    for i in range(7):
        struct.pack_into(">I", header, 0x48 + i * 4, text_addrs[i])
    # Data addrs (11 x 4 bytes)
    for i in range(11):
        struct.pack_into(">I", header, 0x64 + i * 4, data_addrs[i])
    # Text sizes (7 x 4 bytes)
    for i in range(7):
        struct.pack_into(">I", header, 0x90 + i * 4, text_sizes[i])
    # Data sizes (11 x 4 bytes)
    for i in range(11):
        struct.pack_into(">I", header, 0xAC + i * 4, data_sizes[i])

    struct.pack_into(">I", header, 0xD8, bss_addr)
    struct.pack_into(">I", header, 0xDC, bss_size)
    struct.pack_into(">I", header, 0xE0, entry_point)

    with open(output_path, "wb") as f:
        f.write(header)
        f.write(code_bytes)
        f.write(data_bytes)

    print(f"[TEST DOL] Generato con successo: {output_path}")
    print(f"           EntryPoint: 0x{entry_point:08X}, Istruzioni: {len(instructions)}")

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "test_game.dol"
    build_test_dol(out)
