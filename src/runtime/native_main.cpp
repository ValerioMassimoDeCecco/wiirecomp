#include <windows.h>
#include <iostream>
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
        g_Renderer.RenderFrame(g_Memory.gx.vertexOutputBuffer, g_Memory.mem1, 0);
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
            g_Renderer.RenderFrame(
                g_Memory.gx.vertexOutputBuffer,
                g_Memory.mem1,
                g_Memory.currentFramebufferAddress
            );
            if (!g_Memory.gx.vertexOutputBuffer.empty()) {
                g_Memory.gx.vertexOutputBuffer.clear();
            }
            g_Memory.frameReady = false;
        }

        // Sleep minimale per cedere il quanto di tempo CPU
        Sleep(1);

        static uint32_t s_HeartbeatCounter = 0;
        if (++s_HeartbeatCounter % 60 == 0) {
            Log("[HEARTBEAT] CPU PC: " + HexStr(g_GameContext.pc) + 
                ", LR: " + HexStr(g_GameContext.lr) + 
                ", FB: " + HexStr(g_Memory.currentFramebufferAddress) + 
                ", frameReady: " + (g_Memory.frameReady ? "true" : "false"));
        }
    }

    if (gameThread.joinable()) {
        gameThread.detach(); // Termina thread di gioco all'uscita
    }

    g_Audio.Shutdown();
    Log("[MAIN] Chiusura completata con successo.");
    return 0;
}
