#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <functional>
#include <iostream>
#include "wii_types.hpp"

namespace WiiPort {

using FunctionHookCallback = std::function<bool(PPCContext*)>; // Ritorna true se la funzione originale va saltata

// Motore per Mod, Hook nativi C++, Hack Widescreen / 60+ FPS e HD Texture
class ModEngine {
public:
    // Hooks per sostituire o estendere funzioni originali a runtime
    std::unordered_map<uint32_t, FunctionHookCallback> preHooks;
    std::unordered_map<uint32_t, std::function<void(PPCContext*)>> postHooks;

    // Impostazioni Widescreen e Frame Pacing
    float targetAspectRatio = 16.0f / 9.0f; // Supporto 16:9 nativo o 21:9 Ultrawide
    bool unlockFramerate = false;           // 60 / 120 / 144 FPS
    float customFovMultiplier = 1.0f;

    // Registra un hook C++ prima che una specifica funzione venga eseguita
    void RegisterPreHook(uint32_t ppcAddress, FunctionHookCallback callback) {
        preHooks[ppcAddress] = callback;
    }

    // Registra un hook C++ al termine dell'esecuzione di una funzione
    void RegisterPostHook(uint32_t ppcAddress, std::function<void(PPCContext*)> callback) {
        postHooks[ppcAddress] = callback;
    }

    // Intercetta la matrice di proiezione per il supporto Ultrawide (21:9 / 32:9)
    void PatchProjectionMatrix(float* projMatrix4x4) {
        if (targetAspectRatio > (16.0f / 9.0f)) {
            float aspectCorrection = (16.0f / 9.0f) / targetAspectRatio;
            // Corregge la componente orizzontale della matrice di proiezione
            projMatrix4x4[0] *= aspectCorrection * customFovMultiplier;
        }
    }

    // Esegue il pre-hook se presente
    inline bool ExecutePreHook(uint32_t addr, PPCContext* ctx) {
        auto it = preHooks.find(addr);
        if (it != preHooks.end()) {
            return it->second(ctx); // Se true, salta l'implementazione originale
        }
        return false;
    }

    // Esegue il post-hook se presente
    inline void ExecutePostHook(uint32_t addr, PPCContext* ctx) {
        auto it = postHooks.find(addr);
        if (it != postHooks.end()) {
            it->second(ctx);
        }
    }
};

extern ModEngine g_ModEngine;

} // namespace WiiPort
