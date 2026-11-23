#pragma once
#include <cstdint>

namespace WiiPort {

// Genera una maschera a 32-bit tra i bit mb (Mask Begin) e me (Mask End) di PowerPC
// In PowerPC i bit sono numerati da 0 (MSB) a 31 (LSB)
inline constexpr uint32_t GeneratePPCMask(uint32_t mb, uint32_t me) {
    if (mb <= me) {
        uint32_t mask = 0;
        for (uint32_t i = mb; i <= me; ++i) {
            mask |= (1u << (31 - i));
        }
        return mask;
    } else {
        // Maschera circolare invertita
        uint32_t mask = 0;
        for (uint32_t i = 0; i <= me; ++i) mask |= (1u << (31 - i));
        for (uint32_t i = mb; i <= 31; ++i) mask |= (1u << (31 - i));
        return mask;
    }
}

// Rotazione circolare a sinistra a 32-bit
inline constexpr uint32_t RotateLeft32(uint32_t val, uint32_t shift) {
    shift &= 31;
    return (shift == 0) ? val : ((val << shift) | (val >> (32 - shift)));
}

// Implementazione di rlwinm: Rotate Left Word Immediate then AND with Mask
inline constexpr uint32_t PPC_rlwinm(uint32_t val, uint32_t sh, uint32_t mb, uint32_t me) {
    uint32_t rotated = RotateLeft32(val, sh);
    uint32_t mask = GeneratePPCMask(mb, me);
    return rotated & mask;
}

// Implementazione di rlwimi: Rotate Left Word Immediate then Mask Insert
// Inserisce i bit ruotati di val dentro target solo dove la maschera ha bit a 1
inline constexpr uint32_t PPC_rlwimi(uint32_t target, uint32_t val, uint32_t sh, uint32_t mb, uint32_t me) {
    uint32_t rotated = RotateLeft32(val, sh);
    uint32_t mask = GeneratePPCMask(mb, me);
    return (target & ~mask) | (rotated & mask);
}

} // namespace WiiPort
