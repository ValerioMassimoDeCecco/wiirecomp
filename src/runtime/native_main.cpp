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

using namespace WiiPort;

// Istanze globali di sistema
NativeWiiRuntime g_Memory;
D3D12Renderer    g_Renderer;
XAudio2Manager   g_Audio;
InputManager     g_Input;
GameConfig       g_Config;
ModEngine        g_ModEngine;

std::atomic<bool> g_IsRunning = true;

// Prototipo della funzione di avvio del gioco ricompilato
extern int StartRecompiledGame();

// Win32 Window Procedure
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
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
    // 0. Caricamento Configurazione
    g_Config.LoadFromFile("config.ini");

    // 1. Creazione Finestra Win32 Nativa
    const wchar_t CLASS_NAME[] = L"WiiNativeGameWindow";

    WNDCLASS wc = {};
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);

    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(
        0, CLASS_NAME, L"Wii Game - Native Windows Port (DirectX 12)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, g_Config.windowWidth, g_Config.windowHeight,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd) {
        std::cerr << "Impossibile creare la finestra principale.\n";
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);

    // 2. Inizializzazione Sottosistemi Hardware Windows
    if (!g_Renderer.Initialize(hwnd, g_Config.windowWidth, g_Config.windowHeight)) {
        std::cerr << "Inizializzazione DirectX 12 fallita.\n";
        return 1;
    }
    g_Audio.Initialize(48000);

    // 3. Avvio del thread CPU (Gioco Wii Ricompilato)
    std::thread gameThread([]() {
        std::cout << "[NATIVE] Avvio del loop di esecuzione CPU ricompilata...\n";
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

        // Se la GPU ha generato nuova geometria via WGPIPE o VI Swapchain
        if (g_Memory.frameReady || !g_Memory.gx.vertexOutputBuffer.empty()) {
            g_Renderer.RenderFrame(g_Memory.gx.vertexOutputBuffer);
            g_Memory.gx.vertexOutputBuffer.clear();
            g_Memory.frameReady = false;
        }

        // Sleep minimale per cedere il quanto di tempo CPU
        Sleep(1);
    }

    if (gameThread.joinable()) {
        gameThread.detach(); // Termina thread di gioco all'uscita
    }

    g_Audio.Shutdown();
    return 0;
}
