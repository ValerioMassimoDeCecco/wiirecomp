#pragma once
// Risoluzione per FIRMA degli indirizzi delle funzioni libogc/newlib da agganciare.
// Sostituisce gli indirizzi hardcoded (validi solo per un DOL specifico).
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include "dol_loader.hpp"
#include "libogc_signatures.hpp"

namespace WiiPort {

struct LibogcResolveResult {
    std::map<std::string, uint32_t> addr;     // chiave -> indirizzo trovato nel DOL
    std::vector<std::string> missing;         // firma non trovata
    std::vector<std::string> ambiguous;       // piu' di una corrispondenza (nessun hook, per sicurezza)
    bool sameLayoutAsReference = false;       // tutte le funzioni sono esattamente agli indirizzi di riferimento

    bool Has(const std::string& key) const { return addr.count(key) != 0; }
    uint32_t Get(const std::string& key) const { auto it = addr.find(key); return it == addr.end() ? 0 : it->second; }
};

inline LibogcResolveResult ResolveLibogcSymbols(const DolExecutable& dol) {
    LibogcResolveResult res;

    // Decodifica big-endian di ogni sezione di codice, una volta sola
    struct Sec { uint32_t base; std::vector<uint32_t> w; };
    std::vector<Sec> secs;
    for (const auto& s : dol.sections) {
        if (!s.isText) continue;
        Sec sc; sc.base = s.address;
        sc.w.resize(s.size / 4);
        for (size_t i = 0; i < sc.w.size(); ++i) {
            const uint8_t* p = &s.data[i * 4];
            sc.w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
        }
        secs.push_back(std::move(sc));
    }

    size_t atRef = 0;
    for (size_t k = 0; k < kLibogcSignatureCount; ++k) {
        const LibogcSignature& sig = kLibogcSignatures[k];
        std::vector<uint32_t> hits;
        for (const auto& sc : secs) {
            if (sc.w.size() < sig.count) continue;
            for (size_t i = 0; i + sig.count <= sc.w.size(); ++i) {
                bool ok = true;
                for (uint32_t j = 0; j < sig.count; ++j) {
                    if ((sc.w[i + j] & sig.mask[j]) != sig.value[j]) { ok = false; break; }
                }
                if (ok) hits.push_back(sc.base + (uint32_t)(i * 4));
            }
        }
        if (hits.size() == 1) {
            res.addr[sig.key] = hits[0];
            if (hits[0] == sig.refAddr) ++atRef;
        } else if (hits.empty()) {
            res.missing.push_back(sig.key);
        } else {
            res.ambiguous.push_back(sig.key);
        }
    }
    res.sameLayoutAsReference = (atRef == kLibogcSignatureCount);

    auto MarkResolved = [&](const std::string& key, uint32_t a) {
        res.addr[key] = a;
        res.missing.erase(std::remove(res.missing.begin(), res.missing.end(), key), res.missing.end());
        res.ambiguous.erase(std::remove(res.ambiguous.begin(), res.ambiguous.end(), key), res.ambiguous.end());
    };

    // 1. Fallback euristico universale per VIDEO_SetNextFramebuffer
    if (!res.Has("video_setnextfb")) {
        for (const auto& sc : secs) {
            if (sc.w.size() < 4) continue;
            for (size_t i = 0; i + 3 <= sc.w.size(); ++i) {
                uint32_t w1 = sc.w[i];
                uint32_t w2 = sc.w[i + 1];
                uint32_t w3 = sc.w[i + 2];
                if (w1 == 0x39200001 && (w2 & 0xFFFF0000) == 0x906A0000 && (w3 & 0xFFFF0000) == 0x912D0000) {
                    size_t cur = i;
                    while (cur > 0 && (sc.w[cur] & 0xFFFF0000) != 0x94210000) --cur;
                    if ((sc.w[cur] & 0xFFFF0000) == 0x94210000) {
                        MarkResolved("video_setnextfb", sc.base + (uint32_t)(cur * 4));
                        break;
                    }
                }
            }
            if (res.Has("video_setnextfb")) break;
        }
    }
    if (!res.Has("video_setnextfb")) {
        for (const auto& sc : secs) {
            if (sc.w.size() < 24) continue;
            for (size_t i = 3; i + 20 < sc.w.size(); ++i) {
                if ((sc.w[i] & 0xFC1FFFFF) == 0x7C0000A6 &&
                    (sc.w[i + 1] & 0xFC000000) == 0x54000000 &&
                    (sc.w[i + 2] & 0xFC1FFFFF) == 0x7C000124) {
                    bool storesFb30 = false;
                    for (size_t k = 3; k < 25 && i + k < sc.w.size(); ++k) {
                        if ((sc.w[i + k] & 0xFFE0FFFF) == 0x90600030) { storesFb30 = true; break; }
                    }
                    if (!storesFb30) continue;
                    // Funzione leaf libogc 1.8.x (li r0,0 + mfmsr) oppure con stwu r1,-N(r1)
                    if (sc.w[i - 3] == 0x38000000 && (sc.w[i - 2] & 0xFC0007FE) == 0x7C000378) {
                        MarkResolved("video_setnextfb", sc.base + (uint32_t)((i - 3) * 4));
                        break;
                    }
                    size_t cur = i;
                    while (cur > 0 && cur + 8 > i && (sc.w[cur] & 0xFFFF0000) != 0x94210000) --cur;
                    if ((sc.w[cur] & 0xFFFF0000) == 0x94210000) {
                        MarkResolved("video_setnextfb", sc.base + (uint32_t)(cur * 4));
                        break;
                    }
                }
            }
            if (res.Has("video_setnextfb")) break;
        }
    }

    // 2. Fallback euristico universale per VIDEO_WaitVSync
    if (!res.Has("video_waitvsync")) {
        for (const auto& sc : secs) {
            if (sc.w.size() < 6) continue;
            for (size_t i = 0; i + 3 <= sc.w.size(); ++i) {
                uint32_t w1 = sc.w[i];
                uint32_t w2 = sc.w[i + 1];
                uint32_t w3 = sc.w[i + 2];
                // Pattern libogc 3.x (lwz r9,d(r13); cmplw cr0,r0,r9; beq -disp)
                if ((w1 & 0xFFFF0000) == 0x812D0000 && (w2 & 0xFFFF07FF) == 0x7C090040 &&
                    (w3 & 0xFFFF0003) == 0x41820000 && (w3 & 0x8000) != 0) {
                    size_t cur = i;
                    while (cur > 0 && (sc.w[cur] & 0xFFFF0000) != 0x94210000) --cur;
                    if ((sc.w[cur] & 0xFFFF0000) == 0x94210000) {
                        MarkResolved("video_waitvsync", sc.base + (uint32_t)(cur * 4));
                        break;
                    }
                }
                // Pattern generale libogc 1.8.x+ (lwz rX,d(r13) ... bl ... lwz rY,d(r13); cmpw/cmplw crF,rY,rX; beq crF,-disp)
                if ((w1 & 0xFC1F0000) == 0x800D0000 &&
                    ((w2 & 0xFC6007FF) == 0x7C000000 || (w2 & 0xFC6007FF) == 0x7C000040) &&
                    (w3 & 0xFFE30003) == 0x41820000 && (w3 & 0x8000) != 0) {
                    uint32_t crF1 = (w2 >> 23) & 7;
                    uint32_t crF2 = (w3 >> 18) & 7;
                    uint32_t rY = (w1 >> 21) & 0x1F;
                    uint32_t rA = (w2 >> 16) & 0x1F;
                    uint32_t rB = (w2 >> 11) & 0x1F;
                    uint32_t off13 = w1 & 0xFFFF;
                    if (crF1 == crF2 && (rA == rY || rB == rY)) {
                        uint32_t rOther = (rA == rY) ? rB : rA;
                        bool sawBl = false, sawPreLoad = false;
                        for (size_t b = (i > 12 ? i - 12 : 0); b < i; ++b) {
                            if ((sc.w[b] & 0xFC000001) == 0x48000001) sawBl = true;
                            if ((sc.w[b] & 0xFC1F0000) == 0x800D0000 &&
                                ((sc.w[b] >> 21) & 0x1F) == rOther &&
                                (sc.w[b] & 0xFFFF) == off13) {
                                sawPreLoad = true;
                            }
                        }
                        if (sawBl && sawPreLoad) {
                            size_t cur = i;
                            while (cur > 0 && (sc.w[cur] & 0xFFFF0000) != 0x94210000) --cur;
                            if ((sc.w[cur] & 0xFFFF0000) == 0x94210000) {
                                MarkResolved("video_waitvsync", sc.base + (uint32_t)(cur * 4));
                                break;
                            }
                        }
                    }
                }
            }
            if (res.Has("video_waitvsync")) break;
        }
    }

    // 3. Fallback euristico universale per SYS_GetArena1Lo / SYS_GetArena1Hi
    if (!res.Has("arena1lo") || !res.Has("arena1hi")) {
        for (const auto& sc : secs) {
            if (sc.w.size() < 4) continue;
            for (size_t i = 0; i + 4 <= sc.w.size(); ++i) {
                uint32_t w1 = sc.w[i];
                uint32_t w2 = sc.w[i + 1];
                uint32_t w3 = sc.w[i + 2];
                uint32_t w4 = sc.w[i + 3];
                if ((w1 & 0xFFFF0000) == 0x806D0000 && w2 == 0x4E800020 &&
                    (w3 & 0xFFFF0000) == 0x806D0000 && w4 == 0x4E800020) {
                    MarkResolved("arena1lo", sc.base + (uint32_t)(i * 4));
                    MarkResolved("arena1hi", sc.base + (uint32_t)((i + 2) * 4));
                    break;
                }
            }
            if (res.Has("arena1lo") && res.Has("arena1hi")) break;
        }
    }

    // 4. Fallback euristico universale per WPAD (ButtonsDown, ScanPads, ButtonsHeld)
    if (!res.Has("wpad_b")) {
        for (const auto& sc : secs) {
            if (sc.w.size() < 6) continue;
            for (size_t i = 0; i + 4 <= sc.w.size(); ++i) {
                uint32_t w1 = sc.w[i];
                if ((w1 & 0xFC7FFFFF) == 0x28030004 || (w1 & 0xFC7FFFFF) == 0x28030005) {
                    bool hasMulli = false;
                    for (size_t k = 1; k <= 4 && i + k < sc.w.size(); ++k) {
                        if (sc.w[i + k] == 0x1C630154) { hasMulli = true; break; }
                    }
                    if (!hasMulli) continue;
                    uint32_t bdownAddr = sc.base + (uint32_t)(i * 4);
                    // Se preceduta subito da un'istruzione con stesso stub e' probabile ButtonsDown/ButtonsHeld
                    MarkResolved("wpad_b", bdownAddr);

                    // Scansione avanti dopo il prossimo blr per WPAD_ScanPads (o backward)
                    size_t fwd = i;
                    while (fwd < i + 20 && fwd < sc.w.size() && sc.w[fwd] != 0x4E800020) ++fwd;
                    if (fwd + 4 < sc.w.size() && sc.w[fwd] == 0x4E800020 &&
                        ((sc.w[fwd + 1] & 0xFFFF0000) == 0x94210000 || sc.w[fwd + 2] == 0x9421FFE8)) {
                        MarkResolved("wpad_a", sc.base + (uint32_t)((fwd + 1) * 4));
                    } else {
                        size_t cur = i;
                        while (cur > 0 && sc.w[cur] != 0x4E800020) --cur;
                        if (cur > 0 && sc.w[cur] == 0x4E800020) {
                            size_t scanStart = cur;
                            while (scanStart > 0 && (sc.w[scanStart] & 0xFFFF0000) != 0x94210000) --scanStart;
                            if ((sc.w[scanStart] & 0xFFFF0000) == 0x94210000) {
                                MarkResolved("wpad_a", sc.base + (uint32_t)(scanStart * 4));
                            }
                        }
                    }

                    // Cerca altro 1C630154 nelle vicinanze per ButtonsHeld (senza mai collidere con wpad_a o wpad_b)
                    uint32_t wpadA = res.Get("wpad_a");
                    for (size_t j = (i > 40 ? i - 40 : 0); j + 4 < sc.w.size() && j < i + 40; ++j) {
                        uint32_t cand = sc.base + (uint32_t)(j * 4);
                        if (cand == bdownAddr || cand == wpadA) continue;
                        if ((sc.w[j] & 0xFC7FFFFF) == 0x28030004 || (sc.w[j] & 0xFC7FFFFF) == 0x28030005) {
                            for (size_t k = 1; k <= 4 && j + k < sc.w.size(); ++k) {
                                if (sc.w[j + k] == 0x1C630154) {
                                    MarkResolved("wpad_c", cand);
                                    break;
                                }
                            }
                        }
                        if (res.Has("wpad_c")) break;
                    }
                    if (!res.Has("wpad_c") && i + 10 < sc.w.size()) {
                        uint32_t fallbackC = bdownAddr + 40;
                        if (fallbackC != wpadA) MarkResolved("wpad_c", fallbackC);
                    }
                    break;
                }
            }
            if (res.Has("wpad_b")) break;
        }
    }

    if (!res.Has("wpad_a") && res.Has("wpad_b")) {
        uint32_t bdown = res.Get("wpad_b");
        for (const auto& sc : secs) {
            if (bdown > sc.base && bdown < sc.base + sc.w.size() * 4) {
                size_t bIdx = (bdown - sc.base) / 4;
                size_t cur = (bIdx > 0) ? (bIdx - 1) : 0;
                while (cur > 0 && sc.w[cur] != 0x4E800020) --cur;
                if (cur > 0 && sc.w[cur] == 0x4E800020) {
                    size_t scanStart = cur;
                    while (scanStart > 0 && (sc.w[scanStart] & 0xFFFF0000) != 0x94210000) --scanStart;
                    if ((sc.w[scanStart] & 0xFFFF0000) == 0x94210000) {
                        MarkResolved("wpad_a", sc.base + (uint32_t)(scanStart * 4));
                    }
                }
                break;
            }
        }
    }

    // 5. Fallback euristico universale per memalign e _memalign_r via SYS_AllocateFramebuffer
    if (!res.Has("memalign")) {
        for (const auto& sc : secs) {
            if (sc.w.size() < 10) continue;
            for (size_t i = 0; i + 10 <= sc.w.size(); ++i) {
                if (sc.w[i] == 0x1C8600D8) { // mulli r4, r6, 216 in SYS_AllocateFramebuffer
                    for (size_t k = i; k < i + 10 && k + 1 < sc.w.size(); ++k) {
                        if (sc.w[k] == 0x38600020 && (sc.w[k + 1] & 0xFC000000) == 0x48000000) {
                            uint32_t br = sc.w[k + 1];
                            int32_t rel = (int32_t)(br & 0x03FFFFFC);
                            if (rel & 0x02000000) rel |= (int32_t)0xFC000000;
                            uint32_t target = sc.base + (uint32_t)((k + 1) * 4) + (uint32_t)rel;
                            if (dol.IsCodeAddress(target)) {
                                MarkResolved("memalign", target);
                                break;
                            }
                        }
                    }
                    if (res.Has("memalign")) break;
                }
            }
            if (res.Has("memalign")) break;
        }
    }
    if (!res.Has("memalign")) {
        for (const auto& sc : secs) {
            if (sc.w.size() < 12) continue;
            for (size_t i = 0; i + 6 < sc.w.size(); ++i) {
                // SYS_AllocateFramebuffer libogc 1.8.x: bl VIDEO_GetFrameBufferSize; mr r4, r3; li r3, 32; bl memalign
                if ((sc.w[i] & 0xFC000001) == 0x48000001 &&
                    sc.w[i + 1] == 0x7C641B78 &&
                    sc.w[i + 2] == 0x38600020 &&
                    (sc.w[i + 3] & 0xFC000001) == 0x48000001) {
                    uint32_t br = sc.w[i + 3];
                    int32_t rel = (int32_t)(br & 0x03FFFFFC);
                    if (rel & 0x02000000) rel |= (int32_t)0xFC000000;
                    uint32_t target = sc.base + (uint32_t)((i + 3) * 4) + (uint32_t)rel;
                    if (dol.IsCodeAddress(target)) {
                        MarkResolved("memalign", target);
                        break;
                    }
                }
            }
            if (res.Has("memalign")) break;
        }
    }

    if (res.Has("memalign") && !res.Has("memalign_r")) {
        uint32_t ma = res.Get("memalign");
        for (const auto& sc : secs) {
            if (ma >= sc.base && ma < sc.base + sc.w.size() * 4) {
                size_t idx = (ma - sc.base) / 4;
                uint32_t lastBl = 0;
                for (size_t k = idx; k < idx + 24 && k < sc.w.size(); ++k) {
                    uint32_t w = sc.w[k];
                    if (w == 0x4E800020) break; // blr
                    if ((w & 0xFC000003) == 0x48000000) { // b (tail call a _memalign_r)
                        int32_t rel = (int32_t)(w & 0x03FFFFFC);
                        if (rel & 0x02000000) rel |= (int32_t)0xFC000000;
                        uint32_t target = sc.base + (uint32_t)(k * 4) + (uint32_t)rel;
                        if (dol.IsCodeAddress(target) && target != ma) {
                            MarkResolved("memalign_r", target);
                            lastBl = 0;
                            break;
                        }
                    } else if ((w & 0xFC000003) == 0x48000001) { // bl (es. bl __getreent prima di b/bl _memalign_r)
                        int32_t rel = (int32_t)(w & 0x03FFFFFC);
                        if (rel & 0x02000000) rel |= (int32_t)0xFC000000;
                        uint32_t target = sc.base + (uint32_t)(k * 4) + (uint32_t)rel;
                        if (dol.IsCodeAddress(target) && target != ma) {
                            lastBl = target;
                        }
                    }
                }
                if (!res.Has("memalign_r") && lastBl != 0) {
                    MarkResolved("memalign_r", lastBl);
                }
                break;
            }
        }
    }

    // 6. Fallback euristico universale per __lwp_heap_allocate (li r9, -10 -> 0x3920FFF6)
    if (!res.Has("lwp_heap_alloc")) {
        for (const auto& sc : secs) {
            for (size_t i = 0; i < sc.w.size(); ++i) {
                if (sc.w[i] == 0x3920FFF6) {
                    MarkResolved("lwp_heap_alloc", sc.base + (uint32_t)(i * 4));
                    break;
                }
            }
            if (res.Has("lwp_heap_alloc")) break;
        }
    }

    // 7. Fallback euristico universale per sys_init (0x3D20DEAD, 0x6129BEEF)
    if (!res.Has("sys_init")) {
        for (const auto& sc : secs) {
            if (sc.w.size() < 2) continue;
            for (size_t i = 0; i + 1 < sc.w.size(); ++i) {
                if (sc.w[i] == 0x3D20DEAD && sc.w[i + 1] == 0x6129BEEF) {
                    size_t cur = i;
                    while (cur > 0 && (sc.w[cur] & 0xFFFF0000) != 0x94210000) --cur;
                    if ((sc.w[cur] & 0xFFFF0000) == 0x94210000) {
                        MarkResolved("sys_init", sc.base + (uint32_t)(cur * 4));
                        break;
                    }
                }
            }
            if (res.Has("sys_init")) break;
        }
    }

    // 8. Fallback euristico universale per puts
    if (!res.Has("puts")) {
        for (const auto& sc : secs) {
            if (sc.w.size() < 12) continue;
            for (size_t i = 0; i + 12 <= sc.w.size(); ++i) {
                if (sc.w[i] == 0x9421FFF0 && sc.w[i+1] == 0x7C0802A6 && sc.w[i+2] == 0x93E1000C && sc.w[i+3] == 0x7C7F1B78 &&
                    sc.w[i+6] == 0x80010014 && sc.w[i+7] == 0x7FE4FB78 && sc.w[i+8] == 0x83E1000C && sc.w[i+9] == 0x38210010 &&
                    sc.w[i+10] == 0x7C0803A6 && (sc.w[i+11] & 0xFC000001) == 0x48000000) {
                    uint32_t br = sc.w[i+11];
                    int32_t rel = (int32_t)(br & 0x03FFFFFC);
                    if (rel & 0x02000000) rel |= (int32_t)0xFC000000;
                    uint32_t target = sc.base + (uint32_t)((i + 11) * 4) + (uint32_t)rel;
                    uint32_t pc = sc.base + (uint32_t)(i * 4);
                    if (target < pc && (pc - target) < 0x400) {
                        MarkResolved("puts", pc);
                        break;
                    }
                }
            }
            if (res.Has("puts")) break;
        }
    }

    // 9. Fallback euristico universale per console_write via devoptab "stdout"
    if (!res.Has("console_write")) {
        uint32_t stdoutAddr = 0;
        for (const auto& s : dol.sections) {
            if (s.isText || s.size < 8) continue;
            for (size_t i = 0; i + 7 <= s.size; ++i) {
                if (std::memcmp(&s.data[i], "stdout\0", 7) == 0) {
                    stdoutAddr = s.address + (uint32_t)i;
                    break;
                }
            }
            if (stdoutAddr != 0) break;
        }
        if (stdoutAddr != 0) {
            uint8_t needle[4] = {
                (uint8_t)(stdoutAddr >> 24),
                (uint8_t)(stdoutAddr >> 16),
                (uint8_t)(stdoutAddr >> 8),
                (uint8_t)(stdoutAddr & 0xFF)
            };
            for (const auto& s : dol.sections) {
                if (s.isText || s.size < 24) continue;
                for (size_t i = 0; i + 20 <= s.size; i += 4) {
                    if (std::memcmp(&s.data[i], needle, 4) == 0) {
                        uint32_t wr = ((uint32_t)s.data[i + 16] << 24) |
                                      ((uint32_t)s.data[i + 17] << 16) |
                                      ((uint32_t)s.data[i + 18] << 8) |
                                      (uint32_t)s.data[i + 19];
                        if (dol.IsCodeAddress(wr)) {
                            MarkResolved("console_write", wr);
                            break;
                        }
                    }
                }
                if (res.Has("console_write")) break;
            }
        }
    }

    // 10. Fallback euristico universale per crt0 all'entry point del DOL
    if (!res.Has("crt0")) {
        if (dol.IsCodeAddress(dol.entryPoint)) {
            MarkResolved("crt0", dol.entryPoint);
        }
    }

    // 11. Fallback euristico universale per video_init via registro VI_DISP_CONFIG (0xCC002002)
    if (!res.Has("video_init")) {
        for (const auto& sc : secs) {
            if (sc.w.size() < 10) continue;
            for (size_t i = 0; i + 4 <= sc.w.size(); ++i) {
                uint32_t w1 = sc.w[i];
                if ((w1 & 0xFC1F0000) == 0x3C000000 && (w1 & 0xFFFF) == 0xCC00) {
                    uint32_t rd = (w1 >> 21) & 0x1F;
                    for (size_t j = 1; j <= 3 && i + j < sc.w.size(); ++j) {
                        uint32_t w2 = sc.w[i + j];
                        if (((w2 & 0xFC00FFFF) == 0x60002002 || (w2 & 0xFC00FFFF) == 0x38002002) &&
                            ((w2 >> 21) & 0x1F) == rd) {
                            size_t cur = i;
                            while (cur > 0 && (sc.w[cur] & 0xFFFF0000) != 0x94210000) --cur;
                            if ((sc.w[cur] & 0xFFFF0000) == 0x94210000) {
                                MarkResolved("video_init", sc.base + (uint32_t)(cur * 4));
                                break;
                            }
                        }
                    }
                }
                if (res.Has("video_init")) break;
            }
            if (res.Has("video_init")) break;
        }
    }

    // 12. Fallback euristico universale per vi_write_i2c da chiamate interne a vi_init_encoder
    if (!res.Has("vi_write_i2c") && res.Has("vi_init_encoder")) {
        uint32_t encAddr = res.Get("vi_init_encoder");
        for (const auto& sc : secs) {
            if (encAddr >= sc.base && encAddr < sc.base + sc.w.size() * 4) {
                size_t start = (encAddr - sc.base) / 4;
                std::map<uint32_t, int> callCounts;
                for (size_t i = start; i < start + 120 && i < sc.w.size(); ++i) {
                    uint32_t w = sc.w[i];
                    if (w == 0x4E800020) break; // blr
                    if ((w & 0xFC000001) == 0x48000001) { // bl
                        int32_t rel = (int32_t)(w & 0x03FFFFFC);
                        if (rel & 0x02000000) rel |= (int32_t)0xFC000000;
                        uint32_t tgt = sc.base + (uint32_t)(i * 4) + (uint32_t)rel;
                        if (dol.IsCodeAddress(tgt)) callCounts[tgt]++;
                    }
                }
                for (const auto& kv : callCounts) {
                    if (kv.second >= 5) {
                        for (const auto& sc2 : secs) {
                            if (kv.first >= sc2.base && kv.first + 8 <= sc2.base + sc2.w.size() * 4) {
                                size_t off = (kv.first - sc2.base) / 4;
                                if ((sc2.w[off] & 0xFFFF0000) == 0x94210000 && sc2.w[off + 1] == 0x7C0802A6) {
                                    if (sc2.w[off + 3] == 0x83ED0220 || (sc2.w[off + 3] & 0xFFFF0000) == 0x83ED0000) {
                                        MarkResolved("vi_write_i2c", kv.first);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if (res.Has("vi_write_i2c")) break;
                }
                break;
            }
        }
    }

    // 12b. Fallback per vi_write_i2c diretto via registri GPIO/I2C VI (0xCD8000C0 e 0xCD8000C4)
    if (!res.Has("vi_write_i2c")) {
        for (const auto& sc : secs) {
            if (sc.w.size() < 20) continue;
            for (size_t i = 0; i + 8 <= sc.w.size(); ++i) {
                uint32_t w0 = sc.w[i];
                if ((w0 & 0xFC1FFFFF) == 0x3C00CD80) {
                    bool hasC0 = false, hasC4 = false;
                    for (size_t k = i + 1; k < i + 12 && k < sc.w.size(); ++k) {
                        uint32_t wk = sc.w[k];
                        if ((wk & 0xFFFF) == 0x00C0 && ((wk >> 26) == 24 || (wk >> 26) == 14)) hasC0 = true;
                        if ((wk & 0xFFFF) == 0x00C4 && ((wk >> 26) == 24 || (wk >> 26) == 14)) hasC4 = true;
                    }
                    if (hasC0 && hasC4) {
                        size_t cur = i;
                        while (cur > 0 && sc.w[cur] != 0x4E800020 && (sc.w[cur] & 0xFFFF0000) != 0x94210000) --cur;
                        if (sc.w[cur] == 0x4E800020) ++cur;
                        MarkResolved("vi_write_i2c", sc.base + (uint32_t)(cur * 4));
                        break;
                    }
                }
            }
            if (res.Has("vi_write_i2c")) break;
        }
    }

    // 13. Fallback euristico universale per register_frame (frame_dummy in libgcc/crtbegin)
    if (!res.Has("register_frame")) {
        for (const auto& sc : secs) {
            if (sc.w.size() < 4) continue;
            for (size_t i = 0; i + 4 <= sc.w.size(); ++i) {
                if ((sc.w[i] & 0xFFFF0000) == 0x3C600000 &&
                    (sc.w[i + 1] & 0xFFFF0000) == 0x38630000 &&
                    (sc.w[i + 2] & 0xFC000001) == 0x48000000 &&
                    (sc.w[i + 3] & 0xFFFF0000) == 0x94210000) {
                    MarkResolved("register_frame", sc.base + (uint32_t)(i * 4));
                    break;
                }
            }
            if (res.Has("register_frame")) break;
        }
    }

    // 14. Fallback euristico universale per init_user_rt (__init_user_runtime con '_arg')
    if (!res.Has("init_user_rt")) {
        for (const auto& sc : secs) {
            if (sc.w.size() < 4) continue;
            for (size_t i = 0; i + 2 <= sc.w.size(); ++i) {
                if ((sc.w[i] & 0xFFFF) == 0x5F61 && (sc.w[i + 1] & 0xFFFF) == 0x7267) {
                    size_t cur = i;
                    while (cur > 0 && (sc.w[cur] & 0xFFFF0000) != 0x94210000) {
                        if (sc.w[cur] == 0x4E800020) { ++cur; break; }
                        --cur;
                    }
                    MarkResolved("init_user_rt", sc.base + (uint32_t)(cur * 4));
                    break;
                }
            }
            if (res.Has("init_user_rt")) break;
        }
    }

    // 15. Fallback euristico universale per wpad_d
    if (!res.Has("wpad_d")) {
        for (const auto& sc : secs) {
            if (sc.w.size() < 9) continue;
            for (size_t i = 0; i + 9 <= sc.w.size(); ++i) {
                if (sc.w[i] == 0x9421FFF8 && sc.w[i + 1] == 0x7C0802A6 && sc.w[i + 2] == 0x9001000C &&
                    (sc.w[i + 3] & 0xFC000001) == 0x48000001 && sc.w[i + 4] == 0x8001000C &&
                    sc.w[i + 5] == 0x38600001 && sc.w[i + 6] == 0x38210008 &&
                    sc.w[i + 7] == 0x7C0803A6 && sc.w[i + 8] == 0x4E800020) {
                    MarkResolved("wpad_d", sc.base + (uint32_t)(i * 4));
                    break;
                }
            }
            if (res.Has("wpad_d")) break;
        }
    }

    // 16. Fallback euristico universale per wpad_init via controllo SDA prima di wpad_b
    if (!res.Has("wpad_init") && res.Has("wpad_b")) {
        uint32_t bdown = res.Get("wpad_b");
        for (const auto& sc : secs) {
            if (bdown >= sc.base && bdown < sc.base + sc.w.size() * 4) {
                size_t bIdx = (bdown - sc.base) / 4;
                size_t startIdx = (bIdx > 350) ? (bIdx - 350) : 0;
                for (size_t i = startIdx; i < bIdx; ++i) {
                    uint32_t fnAddr = sc.base + (uint32_t)(i * 4);
                    if (fnAddr == res.Get("wpad_a") || fnAddr == bdown || fnAddr == res.Get("wpad_c")) continue;
                    if ((sc.w[i] & 0xFFFF0000) == 0x94210000) {
                        bool hasMfmsr = false;
                        bool hasCmpwi = false;
                        for (size_t j = i; j < i + 18 && j < bIdx; ++j) {
                            if ((sc.w[j] & 0xFC1FFFFF) == 0x7C0000A6) hasMfmsr = true;
                            if ((sc.w[j] & 0xFF80FFFF) == 0x2C090000 || (sc.w[j] & 0xFC1FFFFF) == 0x2C000000) hasCmpwi = true;
                        }
                        if (hasMfmsr && hasCmpwi) {
                            MarkResolved("wpad_init", fnAddr);
                            break;
                        }
                    }
                }
                break;
            }
        }
    }

    // 17. Fallback euristico universale per ipc_b e ipc_a via registro hardware Broadway (0xCD006430)
    if (!res.Has("ipc_b")) {
        for (const auto& sc : secs) {
            if (sc.w.size() < 6) continue;
            for (size_t i = 0; i + 3 <= sc.w.size(); ++i) {
                if ((sc.w[i] & 0xFC1F0000) == 0x3C000000 && (sc.w[i] & 0xFFFF) == 0xCD00) {
                    if ((sc.w[i + 1] & 0xFFFF) == 0x6430 || (sc.w[i + 2] & 0xFFFF) == 0x6430) {
                        size_t cur = i;
                        while (cur > 0) {
                            if ((sc.w[cur] & 0xFFFF0000) == 0x94210000) break;
                            if (sc.w[cur] == 0x4E800020) { ++cur; break; }
                            --cur;
                        }
                        uint32_t ipcb = sc.base + (uint32_t)(cur * 4);
                        MarkResolved("ipc_b", ipcb);

                        if (!res.Has("ipc_a")) {
                            for (size_t k = cur; k < cur + 50 && k < sc.w.size(); ++k) {
                                if (sc.w[k] == 0x4E800020 && k + 1 < sc.w.size()) {
                                    MarkResolved("ipc_a", sc.base + (uint32_t)((k + 1) * 4));
                                    break;
                                }
                            }
                        }
                        break;
                    }
                }
            }
            if (res.Has("ipc_b")) break;
        }
    }

    // 18. Fallback euristico universale per _write_r via prologo newlib (vecchia e nuova)
    if (!res.Has("write_r")) {
        static const uint32_t wrPat[10] = {
            0x9421FFE8, 0x7C0802A6, 0x93E10014, 0x7C7F1B78, 0x7C832378,
            0x93A1000C, 0x93C10010, 0x7CDD3378, 0x9001001C, 0x7CBE2B78
        };
        static const uint32_t wrPatOld[6] = {
            0x2F84FFFF, 0x7C0802A6, 0x9421FFE8, 0x93E10014, 0x7C7F1B78, 0x9001001C
        };
        for (const auto& sc : secs) {
            if (sc.w.size() < 10) continue;
            for (size_t i = 0; i + 10 <= sc.w.size(); ++i) {
                bool matchNew = true;
                for (size_t k = 0; k < 10; ++k) {
                    if (sc.w[i + k] != wrPat[k]) { matchNew = false; break; }
                }
                if (matchNew) {
                    MarkResolved("write_r", sc.base + (uint32_t)(i * 4));
                    break;
                }
                bool matchOld = true;
                for (size_t k = 0; k < 6; ++k) {
                    if (sc.w[i + k] != wrPatOld[k]) { matchOld = false; break; }
                }
                if (matchOld && i + 15 < sc.w.size() && sc.w[i + 15] == 0x800B0010) {
                    MarkResolved("write_r", sc.base + (uint32_t)(i * 4));
                    break;
                }
            }
            if (res.Has("write_r")) break;
        }
    }

    // 18b. Fallback universale per setjmp / longjmp (newlib PPC setjmp.S)
    if (!res.Has("setjmp") || !res.Has("longjmp")) {
        for (const auto& sc : secs) {
            if (sc.w.size() < 6) continue;
            for (size_t i = 0; i + 5 <= sc.w.size(); ++i) {
                if (!res.Has("setjmp") &&
                    sc.w[i] == 0x38630007 && sc.w[i + 1] == 0x54630038 &&
                    sc.w[i + 2] == 0x90230000 && sc.w[i + 3] == 0x94430004 && sc.w[i + 4] == 0x95A30004) {
                    MarkResolved("setjmp", sc.base + (uint32_t)(i * 4));
                }
                if (!res.Has("longjmp") &&
                    sc.w[i] == 0x38630007 && sc.w[i + 1] == 0x54630038 &&
                    sc.w[i + 2] == 0x80230000 && sc.w[i + 3] == 0x84430004 && sc.w[i + 4] == 0x85A30004) {
                    MarkResolved("longjmp", sc.base + (uint32_t)(i * 4));
                }
            }
        }
    }

    // 19. Fallback euristico universale per _malloc_r da bl in _memalign_r
    if (res.Has("memalign_r") && !res.Has("malloc_r")) {
        uint32_t mar = res.Get("memalign_r");
        for (const auto& sc : secs) {
            if (mar >= sc.base && mar < sc.base + sc.w.size() * 4) {
                size_t idx = (mar - sc.base) / 4;
                for (size_t k = idx; k < idx + 64 && k < sc.w.size(); ++k) {
                    uint32_t w = sc.w[k];
                    if ((w & 0xFC000001) == 0x48000001) { // bl
                        int32_t rel = (int32_t)(w & 0x03FFFFFC);
                        if (rel & 0x02000000) rel |= (int32_t)0xFC000000;
                        uint32_t target = sc.base + (uint32_t)(k * 4) + (uint32_t)rel;
                        if (dol.IsCodeAddress(target)) {
                            MarkResolved("malloc_r", target);
                            break;
                        }
                    }
                }
                break;
            }
        }
    }

    // 20. Fallback euristico universale per _sbrk_r da malloc_r
    if (res.Has("malloc_r") && !res.Has("sbrk_r")) {
        uint32_t mr = res.Get("malloc_r");
        for (const auto& sc : secs) {
            if (mr >= sc.base && mr < sc.base + sc.w.size() * 4) {
                size_t idx = (mr - sc.base) / 4;
                for (size_t k = idx; k < idx + 350 && k < sc.w.size(); ++k) {
                    uint32_t w = sc.w[k];
                    if ((w & 0xFC000001) == 0x48000001) { // bl
                        int32_t rel = (int32_t)(w & 0x03FFFFFC);
                        if (rel & 0x02000000) rel |= (int32_t)0xFC000000;
                        uint32_t target = sc.base + (uint32_t)(k * 4) + (uint32_t)rel;
                        for (const auto& tsc : secs) {
                            if (target >= tsc.base && target + 8 <= tsc.base + tsc.w.size() * 4) {
                                size_t toff = (target - tsc.base) / 4;
                                if ((tsc.w[toff] & 0xFFFF0000) == 0x94210000 && tsc.w[toff + 1] == 0x7C0802A6) {
                                    MarkResolved("sbrk_r", target);
                                    break;
                                }
                            }
                        }
                    }
                    if (res.Has("sbrk_r")) break;
                }
                break;
            }
        }
    }

    // 21. Fallback euristico universale per malloc, free, _free_r
    if (res.Has("malloc_r") && (!res.Has("malloc") || !res.Has("free") || !res.Has("free_r"))) {
        uint32_t mr = res.Get("malloc_r");
        uint32_t mallocFound = 0;
        uint32_t freeFound = 0;
        for (const auto& sc : secs) {
            if (sc.w.size() < 16) continue;
            for (size_t i = 0; i + 10 <= sc.w.size(); ++i) {
                if (sc.w[i] == 0x9421FFF0 && sc.w[i + 1] == 0x7C0802A6 &&
                    sc.w[i + 2] == 0x93E1000C && sc.w[i + 3] == 0x7C7F1B78 &&
                    sc.w[i + 4] == 0x90010014) {
                    bool isMalloc = false;
                    for (size_t k = i; k < i + 25 && k < sc.w.size(); ++k) {
                        uint32_t w = sc.w[k];
                        if ((w & 0xFC000001) == 0x48000000) { // b
                            int32_t rel = (int32_t)(w & 0x03FFFFFC);
                            if (rel & 0x02000000) rel |= (int32_t)0xFC000000;
                            if (sc.base + (uint32_t)(k * 4) + (uint32_t)rel == mr) {
                                isMalloc = true;
                                break;
                            }
                        }
                    }
                    if (isMalloc && mallocFound == 0) {
                        mallocFound = sc.base + (uint32_t)(i * 4);
                        for (size_t k = i + 10; k < i + 35 && k + 5 <= sc.w.size(); ++k) {
                            if (sc.w[k] == 0x9421FFF0 && sc.w[k + 1] == 0x7C0802A6 &&
                                sc.w[k + 2] == 0x93E1000C && sc.w[k + 3] == 0x7C7F1B78 &&
                                sc.w[k + 4] == 0x90010014) {
                                freeFound = sc.base + (uint32_t)(k * 4);
                                for (size_t j = k; j < k + 25 && j < sc.w.size(); ++j) {
                                    uint32_t w = sc.w[j];
                                    if ((w & 0xFC000001) == 0x48000000) { // b
                                        int32_t rel = (int32_t)(w & 0x03FFFFFC);
                                        if (rel & 0x02000000) rel |= (int32_t)0xFC000000;
                                        uint32_t freeR = sc.base + (uint32_t)(j * 4) + (uint32_t)rel;
                                        if (dol.IsCodeAddress(freeR)) {
                                            MarkResolved("free_r", freeR);
                                        }
                                        break;
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
        if (mallocFound != 0) MarkResolved("malloc", mallocFound);
        if (freeFound != 0) MarkResolved("free", freeFound);
    }
    if (res.Has("malloc_r") && (!res.Has("malloc") || !res.Has("free") || !res.Has("free_r"))) {
        uint32_t mr = res.Get("malloc_r");
        for (const auto& sc : secs) {
            if (mr < sc.base || mr >= sc.base + sc.w.size() * 4) continue;
            size_t mrIdx = (mr - sc.base) / 4;
            size_t lo = (mrIdx > 48) ? (mrIdx - 48) : 0;
            for (size_t i = lo; i + 10 < mrIdx; ++i) {
                if (sc.w[i] == 0x7C0802A6 && sc.w[i + 2] == 0x9421FFF8 && sc.w[i + 3] == 0x7C641B78) {
                    for (size_t k = i; k < i + 12 && k < mrIdx; ++k) {
                        uint32_t w = sc.w[k];
                        if ((w & 0xFC000000) == 0x48000000) {
                            int32_t rel = (int32_t)(w & 0x03FFFFFC);
                            if (rel & 0x02000000) rel |= (int32_t)0xFC000000;
                            uint32_t target = sc.base + (uint32_t)(k * 4) + (uint32_t)rel;
                            if (target == mr && !res.Has("malloc")) {
                                MarkResolved("malloc", sc.base + (uint32_t)(i * 4));
                            } else if (target != mr && dol.IsCodeAddress(target) && !res.Has("free")) {
                                MarkResolved("free", sc.base + (uint32_t)(i * 4));
                                if (!res.Has("free_r")) MarkResolved("free_r", target);
                            }
                            break;
                        }
                    }
                }
            }
            break;
        }
    }

    // 22. Fallback euristico universale per _calloc_r e calloc
    if (res.Has("malloc_r") && (!res.Has("calloc_r") || !res.Has("calloc"))) {
        uint32_t mr = res.Get("malloc_r");
        uint32_t callocR = 0;
        for (const auto& sc : secs) {
            if (sc.w.size() < 10) continue;
            for (size_t i = 0; i + 10 <= sc.w.size(); ++i) {
                uint32_t wi = sc.w[i];
                bool isMul = ((wi & 0xFC0007FE) == 0x7C000016) || ((wi & 0xFC0007FE) == 0x7C0001D6);
                if (isMul) { // mulhwu o mullw
                    for (size_t k = i; k < i + 14 && k < sc.w.size(); ++k) {
                        uint32_t w = sc.w[k];
                        if ((w & 0xFC000001) == 0x48000001) { // bl
                            int32_t rel = (int32_t)(w & 0x03FFFFFC);
                            if (rel & 0x02000000) rel |= (int32_t)0xFC000000;
                            if (sc.base + (uint32_t)(k * 4) + (uint32_t)rel == mr) {
                                size_t cur = i;
                                while (cur > 0 && (sc.w[cur] & 0xFFFF0000) != 0x94210000 &&
                                       (cur - i > 4 || sc.w[cur] != wi)) {
                                    --cur;
                                }
                                // Se inizia prima di stwu con mullw / mflr r0, includi il vero entry point
                                if ((sc.w[cur] & 0xFFFF0000) == 0x94210000) {
                                    if (cur > 0 && sc.w[cur - 1] == 0x7C0802A6) --cur;
                                    if (cur > 0 && ((sc.w[cur - 1] & 0xFC0007FE) == 0x7C0001D6)) --cur;
                                }
                                callocR = sc.base + (uint32_t)(cur * 4);
                                break;
                            }
                        }
                    }
                }
                if (callocR != 0) break;
            }
            if (callocR != 0) break;
        }
        if (callocR != 0) {
            MarkResolved("calloc_r", callocR);
            for (const auto& sc : secs) {
                for (size_t i = 0; i < sc.w.size(); ++i) {
                    uint32_t w = sc.w[i];
                    if ((w & 0xFC000000) == 0x48000000) { // b o bl
                        int32_t rel = (int32_t)(w & 0x03FFFFFC);
                        if (rel & 0x02000000) rel |= (int32_t)0xFC000000;
                        if (sc.base + (uint32_t)(i * 4) + (uint32_t)rel == callocR) {
                            size_t cur = i;
                            while (cur > 0 && (sc.w[cur] & 0xFFFF0000) != 0x94210000) --cur;
                            if ((sc.w[cur] & 0xFFFF0000) == 0x94210000 && cur > 0 && sc.w[cur - 1] == 0x7C0802A6) {
                                --cur;
                            }
                            MarkResolved("calloc", sc.base + (uint32_t)(cur * 4));
                            break;
                        }
                    }
                }
                if (res.Has("calloc")) break;
            }
        }
    }

    // 23. Fallback per lwp_dispatch_a e lwp_dispatch_b (quando presenti)
    if (!res.Has("lwp_dispatch_a") || !res.Has("lwp_dispatch_b")) {
        std::vector<uint32_t> dispatches;
        for (const auto& sc : secs) {
            if (sc.w.size() < 6) continue;
            for (size_t i = 0; i + 5 <= sc.w.size(); ++i) {
                if (sc.w[i] == 0x2C03FFFF && sc.w[i + 1] == 0x4D820020 &&
                    sc.w[i + 2] == 0x5469843E && sc.w[i + 3] == 0x2C090002 &&
                    sc.w[i + 4] == 0x4C820020) {
                    dispatches.push_back(sc.base + (uint32_t)(i * 4));
                }
            }
        }
        if (dispatches.size() >= 2) {
            if (!res.Has("lwp_dispatch_a")) MarkResolved("lwp_dispatch_a", dispatches[0]);
            if (!res.Has("lwp_dispatch_b")) MarkResolved("lwp_dispatch_b", dispatches[1]);
        }
    }

    // 24. Fallback per lwp_misc (quando presente)
    if (!res.Has("lwp_misc")) {
        for (const auto& sc : secs) {
            if (sc.w.size() < 7) continue;
            for (size_t i = 0; i + 7 <= sc.w.size(); ++i) {
                if (sc.w[i] == 0x81230034 && sc.w[i + 1] == 0x2C090000 &&
                    sc.w[i + 2] == 0x41820018 && sc.w[i + 3] == 0x28090001 &&
                    sc.w[i + 4] == 0x4182000C && sc.w[i + 5] == 0x38600000 &&
                    sc.w[i + 6] == 0x4E800020) {
                    MarkResolved("lwp_misc", sc.base + (uint32_t)(i * 4));
                    break;
                }
            }
            if (res.Has("lwp_misc")) break;
        }
    }

    // 25. Fallback per abort (quando presente)
    if (!res.Has("abort")) {
        for (const auto& sc : secs) {
            if (sc.w.size() < 8) continue;
            for (size_t i = 0; i + 8 <= sc.w.size(); ++i) {
                if (sc.w[i] == 0x9421FFF8 && sc.w[i + 2] == 0x7C0802A6 &&
                    sc.w[i + 3] == 0x38A0000E && sc.w[i + 5] == 0x38600002 &&
                    (sc.w[i + 7] & 0xFC000001) == 0x48000001) {
                    MarkResolved("abort", sc.base + (uint32_t)(i * 4));
                    break;
                }
            }
            if (res.Has("abort")) break;
        }
    }

    // 26. Fallback euristico per GX_DrawDone / GX_WaitDrawDone (evita attesa infinita di LWP/IRQ)
    if (!res.Has("gx_drawdone")) {
        for (const auto& sc : secs) {
            if (sc.w.size() < 8) continue;
            for (size_t i = 0; i + 5 < sc.w.size(); ++i) {
                // Pattern BP PE_FINISH: caricamento 0x45000002 o attesa flag _gxfinished vicino a bl LWP_ThreadSleep
                uint32_t w0 = sc.w[i];
                uint32_t w1 = sc.w[i + 1];
                if (((w0 & 0xFC1FFFFF) == 0x3C004500 && (w1 & 0xFFFF) == 0x0002) ||
                    ((w0 & 0xFFFF0000) == 0x38000000 && w0 == 0x45000002)) {
                    size_t cur = i;
                    while (cur > 0 && (sc.w[cur] & 0xFFFF0000) != 0x94210000) --cur;
                    if ((sc.w[cur] & 0xFFFF0000) == 0x94210000) {
                        MarkResolved("gx_drawdone", sc.base + (uint32_t)(cur * 4));
                        break;
                    }
                }
            }
            if (res.Has("gx_drawdone")) break;
        }
    }

    // 27. Riconoscimento moduli opzionali non linkati nel DOL
    // Alcuni homebrew non usano threading, console testo o GX; se il compilatore/linker
    // non ha incluso tali funzioni nel DOL, vengono rimosse da missing.
    static const char* kOptionalKeys[] = {
        "con_initex", "lwp_dispatch_a", "lwp_dispatch_b", "lwp_misc", "abort", "gx_drawdone"
    };
    for (const char* opt : kOptionalKeys) {
        if (!res.Has(opt)) {
            res.missing.erase(std::remove(res.missing.begin(), res.missing.end(), opt), res.missing.end());
        }
    }

    return res;
}

} // namespace WiiPort
