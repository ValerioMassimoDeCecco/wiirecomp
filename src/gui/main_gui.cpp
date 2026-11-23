#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <queue>
#include <sstream>
#include <filesystem>
#include <iostream>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// Custom Window Messages
#define WM_APP_APPEND_LOG   (WM_APP + 1)
#define WM_APP_BUILD_DONE   (WM_APP + 2)

// UI IDs
#define IDC_EDIT_DOL_PATH   101
#define IDC_BTN_BROWSE      102
#define IDC_BTN_COMPILE     103
#define IDC_BTN_RUN         104
#define IDC_EDIT_CONSOLE    105
#define IDC_STATIC_STATUS   106

// Global State
HINSTANCE g_hInstance = nullptr;
HWND g_hWndMain = nullptr;
HWND g_hEditDol = nullptr;
HWND g_hBtnBrowse = nullptr;
HWND g_hBtnCompile = nullptr;
HWND g_hBtnRun = nullptr;
HWND g_hEditConsole = nullptr;
HWND g_hStaticStatus = nullptr;

HFONT g_hFontUI = nullptr;
HFONT g_hFontConsole = nullptr;
HBRUSH g_hConsoleBgBrush = nullptr;

std::mutex g_LogMutex;
std::queue<std::wstring> g_LogQueue;
std::thread g_WorkerThread;
std::wstring g_SelectedDolPath;
std::filesystem::path g_ProjectRoot;

void AppendConsoleLog(const std::wstring& line) {
    {
        std::lock_guard<std::mutex> lock(g_LogMutex);
        g_LogQueue.push(line);
    }
    if (g_hWndMain) {
        PostMessageW(g_hWndMain, WM_APP_APPEND_LOG, 0, 0);
    }
}

// Trova la root del progetto cercando CMakeLists.txt
std::filesystem::path FindProjectRoot() {
    std::filesystem::path cur = std::filesystem::current_path();
    for (int i = 0; i < 4; ++i) {
        if (std::filesystem::exists(cur / "CMakeLists.txt")) {
            return cur;
        }
        if (cur.has_parent_path() && cur.parent_path() != cur) {
            cur = cur.parent_path();
        } else {
            break;
        }
    }
    return std::filesystem::current_path();
}

// Esegue un comando da riga di comando e cattura stdout/stderr in streaming
bool RunProcessCapture(const std::wstring& cmdLine, const std::filesystem::path& workingDir) {
    SECURITY_ATTRIBUTES saAttr = {};
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = nullptr;

    HANDLE hChildStd_OUT_Rd = nullptr;
    HANDLE hChildStd_OUT_Wr = nullptr;

    if (!CreatePipe(&hChildStd_OUT_Rd, &hChildStd_OUT_Wr, &saAttr, 0)) {
        AppendConsoleLog(L"[ERRORE] Impossibile creare la pipe di output.");
        return false;
    }
    SetHandleInformation(hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(STARTUPINFOW);
    si.hStdError = hChildStd_OUT_Wr;
    si.hStdOutput = hChildStd_OUT_Wr;
    si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};

    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    std::wstring wDir = workingDir.wstring();

    BOOL success = CreateProcessW(
        nullptr,
        cmdBuf.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        wDir.c_str(),
        &si,
        &pi
    );

    CloseHandle(hChildStd_OUT_Wr);

    if (!success) {
        CloseHandle(hChildStd_OUT_Rd);
        AppendConsoleLog(L"[ERRORE] Impossibile avviare il processo: " + cmdLine);
        return false;
    }

    // Lettura dello stream in tempo reale
    CHAR chBuf[512];
    DWORD dwRead;
    std::string lineAccumulator;

    while (ReadFile(hChildStd_OUT_Rd, chBuf, sizeof(chBuf) - 1, &dwRead, nullptr) && dwRead > 0) {
        chBuf[dwRead] = '\0';
        for (DWORD i = 0; i < dwRead; ++i) {
            char c = chBuf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                int wlen = MultiByteToWideChar(CP_UTF8, 0, lineAccumulator.c_str(), -1, nullptr, 0);
                if (wlen > 0) {
                    std::wstring wline(wlen - 1, L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, lineAccumulator.c_str(), -1, &wline[0], wlen);
                    AppendConsoleLog(wline);
                }
                lineAccumulator.clear();
            } else {
                lineAccumulator.push_back(c);
            }
        }
    }

    if (!lineAccumulator.empty()) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, lineAccumulator.c_str(), -1, nullptr, 0);
        if (wlen > 0) {
            std::wstring wline(wlen - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, lineAccumulator.c_str(), -1, &wline[0], wlen);
            AppendConsoleLog(wline);
        }
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hChildStd_OUT_Rd);

    return (exitCode == 0);
}

// Procedura di compilazione asincrona
void CompilationTask(std::wstring dolPathStr) {
    std::filesystem::path dolPath(dolPathStr);
    std::filesystem::path root = g_ProjectRoot;

    AppendConsoleLog(L"============================================================");
    AppendConsoleLog(L"  AVVIO PIPELINE DI RICOMPILAZIONE WIIRECOMP");
    AppendConsoleLog(L"============================================================");
    AppendConsoleLog(L"[INFO] File DOL selezionato: " + dolPath.filename().wstring());
    AppendConsoleLog(L"[INFO] Directory radice progetto: " + root.wstring());

    // 1. Individuazione wiirecomp.exe
    std::filesystem::path wiirecompExe = root / "build" / "Release" / "wiirecomp.exe";
    if (!std::filesystem::exists(wiirecompExe)) {
        wiirecompExe = root / "wiirecomp.exe";
    }

    if (!std::filesystem::exists(wiirecompExe)) {
        AppendConsoleLog(L"[STEP 0] wiirecomp.exe non trovato. Avvio compilazione di wiirecomp...");
        std::wstring cmdBuildTool = L"cmake --build build --config Release --target wiirecomp";
        if (!RunProcessCapture(cmdBuildTool, root)) {
            AppendConsoleLog(L"[FATALE] Impossibile compilare wiirecomp.exe!");
            PostMessageW(g_hWndMain, WM_APP_BUILD_DONE, FALSE, 0);
            return;
        }
        wiirecompExe = root / "build" / "Release" / "wiirecomp.exe";
    }

    // 2. Esecuzione ricompilatore statico (PPC -> C++)
    AppendConsoleLog(L"");
    AppendConsoleLog(L"[STEP 1/3] Traduzione statica AOT del bytecode PowerPC -> C++...");
    std::filesystem::path recompiledCpp = root / "recompiled_game.cpp";
    std::wstring cmdRecomp = L"\"" + wiirecompExe.wstring() + L"\" \"" + dolPath.wstring() + L"\" \"" + recompiledCpp.wstring() + L"\"";

    if (!RunProcessCapture(cmdRecomp, root)) {
        AppendConsoleLog(L"[FATALE] Ricompilazione PowerPC fallita!");
        PostMessageW(g_hWndMain, WM_APP_BUILD_DONE, FALSE, 0);
        return;
    }
    AppendConsoleLog(L"[STEP 1/3] OK! File recompiled_game.cpp generato con successo.");

    // 3. Copia del file DOL in build/Release/boot.dol
    AppendConsoleLog(L"");
    AppendConsoleLog(L"[STEP 2/3] Copia delle sezioni statiche in build/Release/boot.dol...");
    try {
        std::filesystem::path destDol = root / "build" / "Release" / "boot.dol";
        std::filesystem::create_directories(destDol.parent_path());
        std::filesystem::copy_file(dolPath, destDol, std::filesystem::copy_options::overwrite_existing);
        AppendConsoleLog(L"[STEP 2/3] OK! File DOL aggiornato per il runtime.");
    } catch (const std::exception& e) {
        AppendConsoleLog(L"[ATTENZIONE] Copia boot.dol: " + std::wstring(e.what(), e.what() + strlen(e.what())));
    }

    // 4. Compilazione C++ con CMake / MSBuild in Release
    AppendConsoleLog(L"");
    AppendConsoleLog(L"[STEP 3/3] Compilazione eseguibile nativo Game_Native.exe con MSVC / DirectX 12...");
    std::wstring cmdBuildGame = L"cmake --build build --config Release --target Game_Native";

    if (!RunProcessCapture(cmdBuildGame, root)) {
        AppendConsoleLog(L"[FATALE] Compilazione C++ di Game_Native.exe fallita!");
        PostMessageW(g_hWndMain, WM_APP_BUILD_DONE, FALSE, 0);
        return;
    }

    AppendConsoleLog(L"");
    AppendConsoleLog(L"============================================================");
    AppendConsoleLog(L"  COMPILAZIONE COMPLETATA CON SUCCESSO! [100%]");
    AppendConsoleLog(L"  Eseguibile pronto: build/Release/Game_Native.exe");
    AppendConsoleLog(L"============================================================");

    PostMessageW(g_hWndMain, WM_APP_BUILD_DONE, TRUE, 0);
}

// Procedura finestra principale
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hWndMain = hwnd;

            // Attivazione Dark Mode sulla barra del titolo Windows
            BOOL darkMode = TRUE;
            DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

            // Font moderni
            g_hFontUI = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            g_hFontConsole = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");

            g_hConsoleBgBrush = CreateSolidBrush(RGB(18, 20, 24));

            // Label File DOL
            HWND hLbl = CreateWindowW(L"STATIC", L"File Eseguibile Wii (.dol):",
                WS_CHILD | WS_VISIBLE, 20, 18, 200, 22, hwnd, nullptr, g_hInstance, nullptr);
            SendMessageW(hLbl, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

            // Edit Path DOL
            g_hEditDol = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 20, 42, 650, 30, hwnd, (HMENU)IDC_EDIT_DOL_PATH, g_hInstance, nullptr);
            SendMessageW(g_hEditDol, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

            // Tasto Sfoglia
            g_hBtnBrowse = CreateWindowW(L"BUTTON", L"Sfoglia...",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 680, 41, 130, 32, hwnd, (HMENU)IDC_BTN_BROWSE, g_hInstance, nullptr);
            SendMessageW(g_hBtnBrowse, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

            // Tasto Compila
            g_hBtnCompile = CreateWindowW(L"BUTTON", L"Avvia Ricompilazione e Build",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 20, 85, 260, 38, hwnd, (HMENU)IDC_BTN_COMPILE, g_hInstance, nullptr);
            SendMessageW(g_hBtnCompile, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

            // Tasto Esegui Gioco
            g_hBtnRun = CreateWindowW(L"BUTTON", L"Avvia Gioco Ricompilato",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 295, 85, 230, 38, hwnd, (HMENU)IDC_BTN_RUN, g_hInstance, nullptr);
            SendMessageW(g_hBtnRun, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            EnableWindow(g_hBtnRun, FALSE);

            // Label Stato
            g_hStaticStatus = CreateWindowW(L"STATIC", L"Stato: Pronto. Seleziona o trascina un file .dol nella finestra.",
                WS_CHILD | WS_VISIBLE, 540, 95, 270, 24, hwnd, (HMENU)IDC_STATIC_STATUS, g_hInstance, nullptr);
            SendMessageW(g_hStaticStatus, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

            // Label Console
            HWND hLblConsole = CreateWindowW(L"STATIC", L"Console Output Operazioni in Tempo Reale:",
                WS_CHILD | WS_VISIBLE, 20, 135, 400, 22, hwnd, nullptr, g_hInstance, nullptr);
            SendMessageW(hLblConsole, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

            // Edit Multiline Console Box
            g_hEditConsole = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                20, 160, 790, 410, hwnd, (HMENU)IDC_EDIT_CONSOLE, g_hInstance, nullptr);
            SendMessageW(g_hEditConsole, WM_SETFONT, (WPARAM)g_hFontConsole, TRUE);

            // Abilita Drag and Drop
            DragAcceptFiles(hwnd, TRUE);

            // Verifica se esiste già un boot.dol nella root
            std::filesystem::path defaultDol = g_ProjectRoot / "boot.dol";
            if (std::filesystem::exists(defaultDol)) {
                g_SelectedDolPath = defaultDol.wstring();
                SetWindowTextW(g_hEditDol, g_SelectedDolPath.c_str());
            }

            // Verifica se Game_Native.exe esiste già
            std::filesystem::path gameExe = g_ProjectRoot / "build" / "Release" / "Game_Native.exe";
            if (std::filesystem::exists(gameExe)) {
                EnableWindow(g_hBtnRun, TRUE);
            }

            return 0;
        }

        case WM_SIZE: {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            if (width > 300 && height > 250) {
                SetWindowPos(g_hEditDol, nullptr, 20, 42, width - 180, 30, SWP_NOZORDER);
                SetWindowPos(g_hBtnBrowse, nullptr, width - 150, 41, 130, 32, SWP_NOZORDER);
                SetWindowPos(g_hEditConsole, nullptr, 20, 160, width - 40, height - 180, SWP_NOZORDER);
            }
            return 0;
        }

        case WM_DROPFILES: {
            HDROP hDrop = (HDROP)wParam;
            wchar_t filePath[MAX_PATH];
            if (DragQueryFileW(hDrop, 0, filePath, MAX_PATH)) {
                g_SelectedDolPath = filePath;
                SetWindowTextW(g_hEditDol, filePath);
                SetWindowTextW(g_hStaticStatus, L"Stato: File caricato con successo!");
            }
            DragFinish(hDrop);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdcStatic = (HDC)wParam;
            HWND hStatic = (HWND)lParam;
            if (hStatic == g_hEditConsole) {
                SetTextColor(hdcStatic, RGB(0, 240, 160)); // Cyan-Green terminal text
                SetBkColor(hdcStatic, RGB(18, 20, 24));    // Dark terminal background
                return (LRESULT)g_hConsoleBgBrush;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_BTN_BROWSE) {
                wchar_t szFile[MAX_PATH] = L"";
                OPENFILENAMEW ofn = {};
                ofn.lStructSize = sizeof(OPENFILENAMEW);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = L"File Nintendo Wii (*.dol)\0*.dol\0Tutti i file (*.*)\0*.*\0";
                ofn.lpstrFile = szFile;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER;

                if (GetOpenFileNameW(&ofn)) {
                    g_SelectedDolPath = szFile;
                    SetWindowTextW(g_hEditDol, szFile);
                    SetWindowTextW(g_hStaticStatus, L"Stato: File DOL pronto per la compilazione.");
                }
            } else if (id == IDC_BTN_COMPILE) {
                wchar_t curPath[MAX_PATH] = L"";
                GetWindowTextW(g_hEditDol, curPath, MAX_PATH);
                g_SelectedDolPath = curPath;

                if (g_SelectedDolPath.empty() || !std::filesystem::exists(g_SelectedDolPath)) {
                    MessageBoxW(hwnd, L"Seleziona un file .dol valido prima di avviare la compilazione!", L"Attenzione", MB_OK | MB_ICONWARNING);
                    return 0;
                }

                // Disabilita controlli durante la compilazione
                EnableWindow(g_hBtnCompile, FALSE);
                EnableWindow(g_hBtnBrowse, FALSE);
                EnableWindow(g_hBtnRun, FALSE);
                SetWindowTextW(g_hStaticStatus, L"Stato: Ricompilazione e compilazione in corso...");
                SetWindowTextW(g_hEditConsole, L""); // Pulisci console

                if (g_WorkerThread.joinable()) {
                    g_WorkerThread.join();
                }

                g_WorkerThread = std::thread(CompilationTask, g_SelectedDolPath);
            } else if (id == IDC_BTN_RUN) {
                std::filesystem::path gameExe = g_ProjectRoot / "build" / "Release" / "Game_Native.exe";
                if (!std::filesystem::exists(gameExe)) {
                    gameExe = g_ProjectRoot / "Game_Native.exe";
                }

                if (std::filesystem::exists(gameExe)) {
                    ShellExecuteW(nullptr, L"open", gameExe.c_str(), nullptr, gameExe.parent_path().c_str(), SW_SHOW);
                    SetWindowTextW(g_hStaticStatus, L"Stato: Gioco avviato con successo!");
                } else {
                    MessageBoxW(hwnd, L"Game_Native.exe non trovato! Esegui prima la compilazione.", L"Errore", MB_OK | MB_ICONERROR);
                }
            }
            return 0;
        }

        case WM_APP_APPEND_LOG: {
            std::wstring textToAppend;
            {
                std::lock_guard<std::mutex> lock(g_LogMutex);
                while (!g_LogQueue.empty()) {
                    textToAppend += g_LogQueue.front() + L"\r\n";
                    g_LogQueue.pop();
                }
            }

            if (!textToAppend.empty() && g_hEditConsole) {
                int len = GetWindowTextLengthW(g_hEditConsole);
                SendMessageW(g_hEditConsole, EM_SETSEL, (WPARAM)len, (LPARAM)len);
                SendMessageW(g_hEditConsole, EM_REPLACESEL, FALSE, (LPARAM)textToAppend.c_str());
                SendMessageW(g_hEditConsole, EM_SCROLLCARET, 0, 0);
            }
            return 0;
        }

        case WM_APP_BUILD_DONE: {
            BOOL ok = (BOOL)wParam;
            EnableWindow(g_hBtnCompile, TRUE);
            EnableWindow(g_hBtnBrowse, TRUE);
            if (ok) {
                EnableWindow(g_hBtnRun, TRUE);
                SetWindowTextW(g_hStaticStatus, L"Stato: Compilazione completata con successo! Pronto.");
            } else {
                SetWindowTextW(g_hStaticStatus, L"Stato: Errore durante la compilazione. Controlla la console.");
            }
            return 0;
        }

        case WM_DESTROY: {
            if (g_WorkerThread.joinable()) {
                g_WorkerThread.detach();
            }
            if (g_hFontUI) DeleteObject(g_hFontUI);
            if (g_hFontConsole) DeleteObject(g_hFontConsole);
            if (g_hConsoleBgBrush) DeleteObject(g_hConsoleBgBrush);
            PostQuitMessage(0);
            return 0;
        }

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    g_hInstance = hInstance;
    g_ProjectRoot = FindProjectRoot();

    INITCOMMONCONTROLSEX icex = {};
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    const wchar_t CLASS_NAME[] = L"WiiRecompStudioGUI";

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);

    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        CLASS_NAME,
        L"Wii Universal AOT Recompiler - Studio GUI",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 860, 640,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}
