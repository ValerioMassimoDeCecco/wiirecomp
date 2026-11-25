# Wii Native Windows Porting System (WiiRecomp)

Un sistema professionale a **Ricompilazione Statica AOT (Ahead-of-Time)** e **Runtime Nativo Windows (DirectX 12 / XAudio2 / XInput)** per convertire ed eseguire file eseguibili per Nintendo Wii (`.dol`) direttamente su Windows come binari x86_64 nativi, con zero overhead di emulazione della CPU.

Il progetto include anche **WiiRecomp Studio**, un'interfaccia grafica desktop moderna per Windows con supporto Drag & Drop e console delle operazioni in tempo reale.

---

## 🌟 Caratteristiche Principali

* **Zero Overhead CPU (Ricompilazione Statica AOT):** Il bytecode PowerPC 750CL (Broadway) viene disassemblato, analizzato per flussi di controllo (CFG) e ricompilato direttamente in codice sorgente C++ altamente ottimizzato, compilato successivamente da MSVC con registri x86_64 fisici e istruzioni vettoriali AVX/SSE.
* **Interfaccia Grafica Dedicata (WiiRecomp Studio):**
  * Selezione del file `.dol` con esplora file o **Drag & Drop** diretto nella finestra.
  * Console di logging streaming in tempo reale integrata con visualizzazione a colori e avanzamento dettagliato.
  * Pipeline automatizzata completa: ricompilazione statica, setup dell'ambiente, build MSVC e pulsante di avvio immediato a 1 clic.
* **Pipeline Grafica DirectX 12 Nativa:**
  * Supporto nativo per **XFB (External FrameBuffer)** a risoluzione originale o scalata a risoluzioni arbitrarie (1080p, 1440p, 4K, Ultrawide 21:9 / 32:9).
  * Conversione hardware in pixel shader da spazio colore **YUYV 4:2:2** a **RGBA32** a 60 FPS con VSync.
  * Emulazione hardware dei registri del Video Interface (`VI_TFBL`, `VI_BFBL`) e pipeline di rendering geometrico GX FIFO (`WGPIPE` a `0xCC008000`).
* **Sottosistemi Hardware Wii Emulati & Bypass:**
  * **Memory Management:** Spazio di memoria continuo per MEM1 (24 MB fisici estendibili fino a 128 MB) e MEM2 (64 MB).
  * **Video Interface & I2C Bypass:** Emulazione dei registri video e bypass dei loop di bit-banging I2C verso gli encoder TV.
  * **Bootstrap & Subsys:** Bypass automatico delle routine di unwinding frame GCC e threading LWP non native su Windows.
  * **Audio XAudio2:** Streaming audio PCM 16-bit stereo a 48 kHz con buffer circolare a bassa latenza.
  * **Controlli (XInput & Mouse IR):** Mappatura immediata per controller Xbox e puntatore IR del telecomando Wii pilotabile via mouse.

---

## 🏗️ Struttura del Repository

```
wiirecomp/
├── include/
│   ├── wii_types.hpp             # Registri CPU PowerPC (r0-r31, f0-f31, gqr0-gqr7, cr, lr, ctr, xer)
│   ├── ppc_bitops.hpp            # Istruzioni bitfield e maschere PowerPC (rlwinm, rlwimi, rlwnm)
│   ├── ppc_lifter.hpp            # Analizzatore CFG e Traduttore AOT da bytecode Broadway a C++
│   ├── dol_loader.hpp            # Parser del formato eseguibile Nintendo (.DOL)
│   ├── rel_loader.hpp            # Parser e linker a runtime per moduli rilocabili (.REL)
│   ├── gx_fifo.hpp               # Command processor FIFO per la GPU Hollywood (WGPIPE 0xCC008000)
│   ├── ios_ipc.hpp               # Gestore richieste IPC verso Starlet/IOS (/dev/di, /dev/fs)
│   ├── iso_fs_reader.hpp         # Virtual File System (VFS) e parser delle immagini disco Wii
│   ├── symbol_map_importer.hpp   # Importatore mappe di simboli (.map da Ghidra, IDA Pro, Dolphin)
│   ├── mod_engine.hpp            # Sistema di hooking C++ per patch a runtime, ultrawide e modding
│   ├── config_manager.hpp        # Gestione configurazione runtime (config.ini)
│   ├── native_renderer_d3d12.hpp # Backend grafico DirectX 12 con shader fullscreen quad e TEV HLSL
│   ├── audio_xaudio2.hpp         # Backend audio streaming XAudio2 a 48 kHz
│   ├── input_manager.hpp         # Mappatura controller XInput e puntatore mouse IR
│   └── wii_runtime.hpp           # Memory Manager globale (MEM1, MEM2, MMIO e Dispatch Table)
├── src/
│   ├── gui/
│   │   └── main_gui.cpp          # Interfaccia grafica desktop Windows (WiiRecomp Studio)
│   ├── recompiler/
│   │   └── main.cpp              # Tool CLI per la ricompilazione statica AOT (.DOL -> C++)
│   └── runtime/
│       └── native_main.cpp       # Entry point nativo Windows (WinMain) e thread di rendering D3D12
├── tools/
│   ├── disasm.py                 # Disassemblatore rapido per istruzioni e funzioni PowerPC
│   ├── check_win.py              # Script di diagnostica finestre e thread
│   ├── test_emu.py               # Framework per il test di emulazione istruzioni PPC
│   ├── trace_fn.py               # Utilità di tracing chiamate di funzione
│   └── generate_test_dol.py      # Generatore sintetico di file .DOL di test
├── CMakeLists.txt                # Configurazione di build CMake per MSVC / Visual Studio 2022
├── config.ini                    # Configurazione grafica e controlli (risoluzione, VSync, fullscreen)
├── .gitignore                    # Regole di esclusione file compilati, binari e log
├── recompiled_game.cpp           # File C++ sorgente ricompilato di riferimento
└── README.md                     # Documentazione ufficiale del progetto
```

---

## 🛠️ Requisiti di Sistema e Compilazione

### Prerequisiti
* **Sistema Operativo:** Windows 10 o Windows 11 (64-bit).
* **IDE / Compilatore:** Visual Studio 2022 (Community, Professional o Enterprise) con carico di lavoro *Sviluppo di applicazioni desktop con C++*.
* **Build System:** CMake 3.20 o superiore.
* **Grafica:** Scheda video con supporto per DirectX 12 (Direct3D 12 Feature Level 11_0 o superiore).

### Configurazione del Progetto con CMake
Apri un terminale (Prompt dei comandi o PowerShell) nella cartella root del progetto:

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
```

### Compilazione di Tutti i Target
Per compilare la toolchain, la GUI e l'eseguibile di gioco in configurazione **Release**:

```powershell
cmake --build build --config Release
```

Al termine della compilazione, nella directory `build/Release/` troverai:
1. `WiiRecomp_Studio.exe`: Interfaccia grafica completa per l'utente.
2. `wiirecomp.exe`: Eseguibile da riga di comando per la ricompilazione statica.
3. `Game_Native.exe`: Il gioco ricompilato ed eseguito nativamente con DirectX 12.

---

## 🖥️ Utilizzo dell'Interfaccia Grafica: WiiRecomp Studio

**WiiRecomp Studio** è progettato per semplificare l'intero processo di conversione da file `.dol` a file `.exe`:

1. Avvia `build/Release/WiiRecomp_Studio.exe`.
2. Trascina un file `.dol` all'interno della finestra oppure clicca sul pulsante **"Sfoglia..."**.
3. Clicca su **"Avvia Ricompilazione e Build"**:
   - Verrà eseguita la traduzione statica da PowerPC a C++.
   - Verrà richiamato il compilatore MSVC per generare il binario nativo Windows.
   - Tutti i passaggi e l'output della compilazione appariranno in tempo reale nel box console inferiore.
4. Una volta completata la procedura, clicca sul pulsante **"Avvia Gioco Ricompilato"** per eseguire direttamente il gioco.

---

## 💻 Utilizzo da Riga di Comando (CLI)

Se preferisci utilizzare gli strumenti da riga di comando o integrarli in script automatizzati:

### 1. Traduzione del file .DOL in codice C++
```powershell
.\build\Release\wiirecomp.exe percorso\del\gioco.dol recompiled_game.cpp
```

*Opzionale:* se disponi di un file di mappa dei simboli:
```powershell
.\build\Release\wiirecomp.exe percorso\del\gioco.dol recompiled_game.cpp --symbols simboli.map
```

### 2. Copia del file DOL per le sezioni statiche
```powershell
Copy-Item percorso\del\gioco.dol build\Release\boot.dol -Force
```

### 3. Compilazione dell'Eseguibile Nativo
```powershell
cmake --build build --config Release --target Game_Native
```

### 4. Esecuzione
```powershell
.\build\Release\Game_Native.exe
```

---

## ⚙️ Configurazione Runtime (`config.ini`)

Il file `config.ini` permette di personalizzare l'esperienza di esecuzione:

```ini
[Graphics]
WindowWidth=1920
WindowHeight=1080
Fullscreen=false
VSync=true
FPSUncap=false

[Audio]
Volume=1.0

[Input]
MouseSensitivity=1.0
InvertPitch=false
```

---

## 📄 Note Legali e Licenza

Questo progetto è rilasciato per scopi di ricerca, preservazione e compatibilità software. Il repository include esclusivamente il codice sorgente del ricompilatore e del runtime; non sono inclusi binari o risorse coperte da copyright di Nintendo o di editori terzi.

---

## 🩺 Risoluzione Problemi

### Schermata piena di rumore colorato (o scura) all'avvio di un `.dol`
Il runtime mostra come immagine la memoria puntata dall'indirizzo XFB. Se quell'indirizzo e' sbagliato si vede "rumore".
Cause tipiche e controlli:

1. **`recompiled_game.cpp` vecchio.** Va rigenerato per *ogni* `.dol` (WiiRecomp Studio lo fa a ogni build, oppure `wiirecomp.exe gioco.dol recompiled_game.cpp`). Un `recompiled_game.cpp` generato da un altro DOL contiene hook per indirizzi che in questo DOL non esistono.
2. **Funzioni libogc non riconosciute.** Il ricompilatore individua per *firma* (non per indirizzo) le funzioni da agganciare e stampa `[LIBOGC] Funzioni libogc riconosciute: N/M`. Se `VIDEO_SetNextFramebuffer` risulta non trovata, il DOL usa una versione di libogc diversa da quella di riferimento. Le firme si rigenerano con `python tools/gen_signatures.py recompiled_game.cpp include/libogc_signatures.hpp` partendo da un `recompiled_game.cpp` generato dal DOL di riferimento.
3. **Console del gioco.** All'avvio vengono stampate le righe `[HOOKS] ...` e `[VI] XFB (...) = 0x...`: indicano quali hook sono attivi e quale framebuffer e' stato registrato. Il log completo e' in `native_runtime.log`.

Il renderer non disegna piu' un XFB non valido (non allineato a 32 byte o fuori dalla RAM): in quel caso si vede solo lo sfondo scuro.
