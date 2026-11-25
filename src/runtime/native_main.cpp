#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <atomic>
#include "wii_runtime.hpp"
#include "native_renderer_d3d12.hpp"
#include "audio_xaudio2.hpp"
#include "input_manager.hpp"
#include "config_manager.hpp"
#include "mod_engine.hpp"

namespace WiiPort {
    NativeWiiRuntime g_Memory;
    D3D12Renderer    g_Renderer;
    XAudio2Manager   g_Audio;
    InputManager     g_Input;
    GameConfig       g_Config;
    ModEngine        g_ModEngine;
}

using namespace WiiPort;

std::atomic<bool> g_IsRunning = true;

// Prototipo della funzione di avvio del gioco ricompilato
extern int StartRecompiledGame();

// Win32 Window Procedure
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_ERASEBKGND:
            return 1; // Impedisce a Windows di ridipingere lo sfondo in bianco
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            g_IsRunning = false;
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                g_IsRunning = false;
                DestroyWindow(hwnd);
            }
            return 0;
        default:
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

static bool SaveXfbBmp(const uint8_t* xfb, const std::string& filename, uint32_t width = 640, uint32_t height = 480) {
    if (!xfb) return false;
    std::ofstream out(filename, std::ios::binary);
    if (!out.is_open()) return false;

    if (width < 16 || width > 720) width = 640;
    if (height < 16 || height > 576) height = 480;
    const uint32_t imageSize = width * height * 4;
    const uint32_t fileSize = 54 + imageSize;

    #pragma pack(push, 1)
    struct BMPHeader {
        uint16_t bfType{0x4D42}; // "BM"
        uint32_t bfSize{0};
        uint16_t bfReserved1{0};
        uint16_t bfReserved2{0};
        uint32_t bfOffBits{54};
        uint32_t biSize{40};
        int32_t  biWidth{0};
        int32_t  biHeight{0}; // negative for top-down
        uint16_t biPlanes{1};
        uint16_t biBitCount{32};
        uint32_t biCompression{0};
        uint32_t biSizeImage{0};
        int32_t  biXPelsPerMeter{2835};
        int32_t  biYPelsPerMeter{2835};
        uint32_t biClrUsed{0};
        uint32_t biClrImportant{0};
    } hdr;
    #pragma pack(pop)

    hdr.bfSize = fileSize;
    hdr.biWidth = (int32_t)width;
    hdr.biHeight = -(int32_t)height; // top-down
    hdr.biSizeImage = imageSize;

    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

    std::vector<uint32_t> bgra(width * height);
    const uint8_t* pSrc = xfb;
    uint32_t* pDst = bgra.data();

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; x += 2) {
            uint8_t y0 = pSrc[0];
            uint8_t u  = pSrc[1];
            uint8_t y1 = pSrc[2];
            uint8_t v  = pSrc[3];
            pSrc += 4;

            if ((y0 | u | y1 | v) == 0) {
                pDst[y * width + x]     = 0xFF000000u;
                pDst[y * width + x + 1] = 0xFF000000u;
                continue;
            }

            int c0 = (int)y0 - 16; if (c0 < 0) c0 = 0;
            int c1 = (int)y1 - 16; if (c1 < 0) c1 = 0;
            int d = (int)u - 128;
            int e = (int)v - 128;

            auto ToBgra = [](int c, int d, int e) -> uint32_t {
                int r = (298 * c + 409 * e + 128) >> 8;
                int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
                int b = (298 * c + 516 * d + 128) >> 8;
                if (r < 0) r = 0; else if (r > 255) r = 255;
                if (g < 0) g = 0; else if (g > 255) g = 255;
                if (b < 0) b = 0; else if (b > 255) b = 255;
                return (0xFF000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            };

            pDst[y * width + x]     = ToBgra(c0, d, e);
            pDst[y * width + x + 1] = ToBgra(c1, d, e);
        }
    }

    out.write(reinterpret_cast<const char*>(bgra.data()), imageSize);
    return true;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // Aggancia la console del processo genitore oppure alloca una nuova console companion
    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
        FILE* fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        freopen_s(&fp, "CONIN$", "r", stdin);
    }

    // Troncamento iniziale del log file
    FILE* fInit = fopen("native_runtime.log", "w");
    if (fInit) fclose(fInit);

    Log("[MAIN] Avvio Wii Native Windows Runtime (WiiRecomp)...");

    // 0. Caricamento Configurazione
    g_Config.LoadFromFile("config.ini");
    Log("[CONFIG] Caricata configurazione: " + std::to_string(g_Config.windowWidth) + "x" + std::to_string(g_Config.windowHeight));

    // 1. Creazione Finestra Win32 Nativa
    const wchar_t CLASS_NAME[] = L"WiiNativeGameWindow";

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);

    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, L"Wii Game - Native Windows Port (DirectX 12)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, g_Config.windowWidth, g_Config.windowHeight,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd) {
        Log("[MAIN] Impossibile creare la finestra principale.");
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // 2. Inizializzazione Sottosistemi Hardware Windows
    if (!g_Renderer.Initialize(hwnd, g_Config.windowWidth, g_Config.windowHeight)) {
        Log("[MAIN] Inizializzazione DirectX 12 non riuscita, prosecuzione in modalita headless.");
    } else {
        // Disegna subito il primo frame scuro per evitare qualsiasi flash bianco
        g_Renderer.RenderFrame(g_Memory.gx.vertexOutputBuffer, nullptr);
    }
    g_Audio.Initialize(48000);

    // 3. Avvio del thread CPU (Gioco Wii Ricompilato)
    std::thread gameThread([]() {
        Log("[NATIVE] Avvio del thread CPU ricompilata...");
        StartRecompiledGame();
    });

    // 4. Main Event Loop di Windows & Presenter Grafico / Input Polling
    MSG msg = {};
    while (g_IsRunning) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_IsRunning = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // Polling Input (Xbox Controller + Mouse IR)
        g_Input.Poll(hwnd);

        // Rendering continuo con D3D12: visualizza geometria GX e framebuffer XFB nativo
        if (g_Renderer.device) {
            // Il puntatore XFB e' gia' validato (allineamento + limiti RAM): se non c'e' un XFB valido
            // si mostra solo lo sfondo scuro, mai memoria casuale.
            g_Renderer.RenderFrame(g_Memory.gx.vertexOutputBuffer, g_Memory.GetXfbHost(), g_Memory.xfbWidth, g_Memory.xfbHeight);
            if (!g_Memory.gx.vertexOutputBuffer.empty()) {
                g_Memory.gx.vertexOutputBuffer.clear();
            }
            g_Memory.frameReady = false;
        }

        // Sleep minimale per cedere il quanto di tempo CPU
        Sleep(1);

        static uint32_t s_HeartbeatCounter = 0;
        if (++s_HeartbeatCounter % 60 == 0) {
            const uint8_t* xfbHost = g_Memory.GetXfbHost();
            const uint32_t fbW = g_Memory.xfbWidth;
            const uint32_t fbH = g_Memory.xfbHeight;
            const size_t xfbBytes = (size_t)fbW * fbH * 2;
            uint32_t nonBlackPixels = 0;
            if (xfbHost) {
                for (size_t i = 0; i + 3 < xfbBytes; i += 4) {
                    uint8_t y0 = xfbHost[i];
                    uint8_t u  = xfbHost[i+1];
                    uint8_t y1 = xfbHost[i+2];
                    uint8_t v  = xfbHost[i+3];
                    if (y0 != 0x10 || u != 0x80 || y1 != 0x10 || v != 0x80) {
                        nonBlackPixels += 2;
                    }
                }
                SaveXfbBmp(xfbHost, "native_screenshot.bmp", fbW, fbH);
                {
                    std::ofstream raw("raw_xfb.bin", std::ios::binary);
                    if (raw.is_open()) raw.write(reinterpret_cast<const char*>(xfbHost), xfbBytes);
                }
                const char* envPath = std::getenv("WII_SCREENSHOT_PATH");
                if (envPath) {
                    SaveXfbBmp(xfbHost, envPath, fbW, fbH);
                }
            }
            Log("[HEARTBEAT] CPU PC: " + HexStr(g_GameContext.pc) + 
                ", LR: " + HexStr(g_GameContext.lr) + 
                ", FB: " + HexStr(g_Memory.currentFramebufferAddress) + 
                ", NonBlackPixels: " + std::to_string(nonBlackPixels));
        }
    }

    if (g_Memory.GetXfbHost()) {
        const uint32_t fbW = g_Memory.xfbWidth;
        const uint32_t fbH = g_Memory.xfbHeight;
        const size_t xfbBytes = (size_t)fbW * fbH * 2;
        SaveXfbBmp(g_Memory.GetXfbHost(), "native_screenshot.bmp", fbW, fbH);
        {
            std::ofstream raw("raw_xfb.bin", std::ios::binary);
            if (raw.is_open()) raw.write(reinterpret_cast<const char*>(g_Memory.GetXfbHost()), xfbBytes);
        }
        const char* envPath = std::getenv("WII_SCREENSHOT_PATH");
        if (envPath) {
            SaveXfbBmp(g_Memory.GetXfbHost(), envPath, fbW, fbH);
        }
    }

    if (gameThread.joinable()) {
        gameThread.detach(); // Termina thread di gioco all'uscita
    }

    g_Audio.Shutdown();
    Log("[MAIN] Chiusura completata con successo.");
    return 0;
}
