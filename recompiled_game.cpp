// Codice generato automaticamente da Wii Universal AOT Recompiler
#include "wii_runtime.hpp"
#include "ppc_bitops.hpp"
#include "mod_engine.hpp"

using namespace WiiPort;

// Prototipi delle funzioni ricompilate
void Func_0x80004000(PPCContext* ctx);

// Function at 0x80004000
void Func_0x80004000(PPCContext* ctx) {
    if (g_ModEngine.ExecutePreHook(0x80004000, ctx)) return;

    /* 0x80004000: 0x38600000 */ ctx->r[3] = 0;
    /* 0x80004004: 0x3c80cc00 */ ctx->r[4] = 0xcc000000;
    /* 0x80004008: 0x60848000 */ ctx->r[4] = ctx->r[4] | 0x8000;
    /* 0x8000400c: 0x38a00080 */ ctx->r[5] = 128;
    /* 0x80004010: 0x90a40000 */ g_Memory.Write32(ctx->r[4] + 0, ctx->r[5]);

loc_0x80004014:
    /* 0x80004014: 0xe0230000 */ DequantizeLoad(ctx, 1, ctx->r[3] + 0, false, 0);
    /* 0x80004018: 0x10410828 */ ctx->f[2].ps[0] = ctx->f[1].ps[0] + ctx->f[1].ps[0]; ctx->f[2].ps[1] = ctx->f[1].ps[1] + ctx->f[1].ps[1];
    /* 0x8000401c: 0xf0430000 */ QuantizeStore(ctx, 2, ctx->r[3] + 0, false, 0);
    /* 0x80004020: 0x38630001 */ ctx->r[3] = ctx->r[3] + 1;
    /* 0x80004024: 0x2803000a */ UpdateCR0(ctx, ctx->r[3] - 10);
    /* 0x80004028: 0x4080fff4 */ if (EvaluateCondition(ctx, 12, 2)) goto loc_0x80004014;
    /* 0x8000402c: 0x4e800020 */ return;
}

// Registrazione della Global Dispatch Table
void InitializeGameDispatchTable() {
    g_Memory.RegisterFunction(0x80004000, &Func_0x80004000);
}

int StartRecompiledGame() {
    InitializeGameDispatchTable();
    PPCContext ctx;
    ctx.Reset();
    std::cout << "[NATIVE] Avvio eseguibile nativo all'EntryPoint: 0x80004000...\n";
    Func_0x80004000(&ctx);
    return 0;
}
