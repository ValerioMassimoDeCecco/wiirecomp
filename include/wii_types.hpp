#pragma once
#include <cstdint>
#include <cstring>

#if defined(_MSC_VER)
    #include <intrin.h>
    #define WII_BYTESWAP16(val) _byteswap_ushort(val)
    #define WII_BYTESWAP32(val) _byteswap_ulong(val)
    #define WII_BYTESWAP64(val) _byteswap_uint64(val)
#else
    #define WII_BYTESWAP16(val) __builtin_bswap16(val)
    #define WII_BYTESWAP32(val) __builtin_bswap32(val)
    #define WII_BYTESWAP64(val) __builtin_bswap64(val)
#endif

namespace WiiPort {

// Rappresentazione del registro a 64-bit per Paired-Single SIMD
union alignas(8) PairedSingle {
    double d;
    float  ps[2]; // ps[0] = componente alta (ps0), ps[1] = componente bassa (ps1)
    uint64_t u64;
    struct {
        uint32_t hi;
        uint32_t lo;
    } raw;
};

// Graphics Quantization Register (GQR) decodificato
struct GQRRegister {
    uint8_t l_scale; // Bits [31:28]: Shift di dequantizzazione per Load (2^scale)
    uint8_t l_type;  // Bits [27:24]: Tipo di dato in memoria per Load (0=float, 4=u8, 5=u16, 6=s8, 7=s16)
    uint8_t s_scale; // Bits [15:12]: Shift di quantizzazione per Store
    uint8_t s_type;  // Bits [11:8] : Tipo di dato convertito per Store

    inline void SetRaw(uint32_t val) {
        l_scale = (val >> 24) & 0x3F;
        l_type  = (val >> 16) & 0x07;
        s_scale = (val >> 8)  & 0x3F;
        s_type  = val & 0x07;
    }

    inline uint32_t GetRaw() const {
        return ((uint32_t)l_scale << 24) | ((uint32_t)l_type << 16) |
               ((uint32_t)s_scale << 8)  | (uint32_t)s_type;
    }
};

// Stato architetturale completo della CPU Broadway (PPC 750CL)
struct alignas(64) PPCContext {
    uint32_t     r[32];      // Registri General Purpose (r0 - r31)
    PairedSingle f[32];      // Registri Floating-Point / Paired Singles (f0 - f31)
    
    uint32_t     cr;         // Condition Register (8 campi da 4-bit: cr0 - cr7)
    uint32_t     lr;         // Link Register (indirizzo di ritorno chiamate)
    uint32_t     ctr;        // Count Register (cicli e salti indiretti)
    uint32_t     xer;        // Fixed-point Exception Register (carry, overflow, byte count)
    uint32_t     pc;         // Program Counter corrente
    
    GQRRegister  gqr[8];     // Registri di Quantizzazione Grafica (GQR0 - GQR7, SPR 912 - 919)

    // Reset dello stato della CPU
    void Reset() {
        std::memset(r, 0, sizeof(r));
        std::memset(f, 0, sizeof(f));
        cr = 0;
        lr = 0;
        ctr = 0;
        xer = 0;
        pc = 0;
        for (int i = 0; i < 8; ++i) {
            gqr[i].SetRaw(0);
        }
    }

    // Helper per estrarre e impostare campi CR (cr0..cr7)
    inline uint32_t GetCRField(uint32_t fieldIndex) const {
        return (cr >> (28 - (fieldIndex * 4))) & 0xF;
    }

    inline void SetCRField(uint32_t fieldIndex, uint32_t val) {
        uint32_t shift = 28 - (fieldIndex * 4);
        cr = (cr & ~(0xF << shift)) | ((val & 0xF) << shift);
    }
};

} // namespace WiiPort
