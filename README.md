# Wii Native Windows Porting System (WiiRecomp)

Un sistema universale a **Ricompilazione Statica AOT (Ahead-of-Time)** e **Runtime Nativo Windows (DirectX 12 / XAudio2 / XInput)** per eseguire giochi per Nintendo Wii direttamente su Windows come binari x86_64 nativi senza emulatori.

---

## 🏗️ Architettura del Sistema

```
wiirecomp/
├── include/
│   ├── wii_types.hpp             # Contesto CPU Broadway PPC 750CL, registri GQR e Paired-Single SIMD
│   ├── ppc_bitops.hpp            # Istruzioni bitfield e maschere PowerPC (rlwinm, rlwimi, rlwnm)
│   ├── ppc_lifter.hpp            # Analizzatore CFG e Traduttore AOT PowerPC -> C++
│   ├── dol_loader.hpp            # Parser del formato eseguibile Nintendo (.DOL)
│   ├── rel_loader.hpp            # Loader e patcher per moduli rilocabili (.REL)
│   ├── gx_fifo.hpp               # Processore FIFO geometrico della GPU Hollywood (WGPIPE 0xCC008000)
│   ├── ios_ipc.hpp               # Server IPC per il microkernel Starlet/IOS (/dev/di, /dev/fs)
│   ├── iso_fs_reader.hpp         # Virtual File System (VFS) e parser FST della Wii
│   ├── symbol_map_importer.hpp   # Importer mappe di simboli (.map) da Dolphin / Ghidra
│   ├── mod_engine.hpp            # Motore di hooking nativo, supporto Ultrawide (21:9) e FPS uncap
│   ├── config_manager.hpp        # Gestione configurazione (config.ini)
│   ├── native_renderer_d3d12.hpp # Backend grafico DirectX 12 con emulazione stadi TEV in HLSL
│   ├── audio_xaudio2.hpp         # Backend audio streaming a 16-bit PCM @ 48kHz via XAudio2
│   ├── input_manager.hpp         # Mappatura controller Xbox (XInput) e puntatore IR via Mouse
│   └── wii_runtime.hpp           # Memory Manager (MEM1 24MB, MEM2 64MB) e Global Dispatch Table
├── src/
│   ├── recompiler/
│   │   └── main.cpp              # Tool CLI per la ricompilazione statica di file .DOL
│   └── runtime/
│       └── native_main.cpp       # Entry point Windows (WinMain) e thread di rendering D3D12
├── tools/
│   └── generate_test_dol.py      # Generatore di file .DOL sintetici con istruzioni Broadway
├── CMakeLists.txt                # Configurazione di build per CMake / Visual Studio 2022
├── config.ini                    # File di configurazione runtime (risoluzione, VSync, FPS)
└── recompiled_game.cpp           # Esempio di codice C++ generato dal ricompilatore
```

---

## 🚀 Istruzioni di Build su Windows

### Prerequisiti
* **Windows 10 / 11 (64-bit)**
* **Visual Studio 2022** con carico di lavoro *Sviluppo di applicazioni desktop con C++*
* **CMake 3.20+**
* DirectX 12 / Windows SDK 10.0+ (incluso con Visual Studio)

### Passaggio 1: Generazione del Progetto Visual Studio
Apri un terminale (PowerShell o Prompt dei comandi per sviluppatori di VS) nella cartella `wiirecomp`:

```cmd
cmake -B build -G "Visual Studio 17 2022" -A x64
```

### Passaggio 2: Compilazione della Toolchain
```cmd
cmake --build build --config Release
```

Questo genererà:
1. `build/Release/wiirecomp.exe`: Il compilatore offline AOT.
2. `build/Release/Game_Native.exe`: L'eseguibile nativo pronto per essere giocato con DirectX 12.

---

## 🎮 Come effettuare il Porting di un Gioco Commerciale

1. **Estrai il file `main.dol` e la cartella risorse** dalla tua ISO o WBFS originale (usando strumenti come Wiimms ISO Tools o Dolphin: *Tasto destro sul gioco -> Proprietà -> File System -> Estrai tutto*).
2. **Esegui il ricompilatore AOT**:
   ```cmd
   wiirecomp.exe percorso/main.dol recompiled_game.cpp --symbols simboli_gioco.map
   ```
3. **Rigenera l'eseguibile di gioco**:
   ```cmd
   cmake --build build --config Release --target Game_Native
   ```
4. **Avvia il gioco nativo**:
   ```cmd
   build\Release\Game_Native.exe
   ```

---

## ⚡ Caratteristiche Uniche del Porting Nativo

* **Zero Overhead di Emulazione CPU**: Il codice PowerPC viene compilato come assembly x86_64 ottimizzato da MSVC con registri fisici e istruzioni SSE/AVX.
* **DirectX 12 Puro**: Risoluzioni arbitrarie native (1080p, 1440p, 4K), supporto display Ultrawide (21:9 / 32:9) e framerate sbloccato.
* **Supporto Controlli**: Puntamento Wiimote IR tramite il mouse di Windows e compatibilità istantanea con controller Xbox, PlayStation e volanti.
* **Modding Nascituro**: Gli hook C++ in `mod_engine.hpp` permettono di iniettare texture pack HD e gameplay hack direttamente a codice sorgente.
