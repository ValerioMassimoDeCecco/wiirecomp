# Specifica Architetturale Completa: Ricompilatore Universale Wii per Windows

Questa specifica mappa in modo esaustivo tutti i componenti hardware, formati binari, registri MMIO, protocolli di sistema e istruzioni della console Nintendo Wii necessari per realizzare un **ricompilatore statico universale (AOT)** e un **runtime nativo Windows a basso livello**.

---

## 1. Mappa della Memoria e Registri MMIO

La Wii (Broadway) adotta uno spazio di indirizzamento a 32-bit big-endian. La memoria fisica è mappata sia in modalità *cached* (tramite BAT) che in modalità *uncached* (con bit 30 impostato su 1, prefisso `0xC0...`).

### 1.1 Layout Globale della Memoria

| Indirizzo Virtuale (Cached) | Indirizzo Virtuale (Uncached) | Indirizzo Fisico | Dimensione | Descrizione |
| :--- | :--- | :--- | :--- | :--- |
| `0x80000000` - `0x817FFFFF` | `0xC0000000` - `0xC17FFFFF` | `0x00000000` | 24 MB | **MEM1**: 1T-SRAM principale ad alta velocità (condivisa tra CPU e GPU) |
| `0x90000000` - `0x93FFFFFF` | `0xD0000000` - `0xD3FFFFFF` | `0x10000000` | 64 MB | **MEM2**: GDDR3 aggiuntiva (accessibile via bus Hollywood) |
| — | `0xCC000000` - `0xCC008003` | `0x0C000000` | 32 KB | **MMIO Legacy (GameCube / Flipper)**: Registri hardware GPU/Audio/Interrupt |
| — | `0xCD000000` - `0xCD000100` | `0x0D000000` | ~1 KB | **MMIO Hollywood / IPC**: Interfaccia di comunicazione con Starlet (IOS) |

---

### 1.2 Mappa dei Registri Hardware MMIO Legacy (`0x0C000000` / `0xCC000000`)

Ogni registro MMIO deve essere intercettato durante le operazioni di `lwz` / `stw` del binario ricompilato.

```
0xCC000000 ----------------------- CP (Command Processor / GPU FIFO)
0xCC001000 ----------------------- PE (Pixel Engine & Z-Buffer)
0xCC002000 ----------------------- VI (Video Interface & Framebuffer)
0xCC003000 ----------------------- PI (Processor Interface & IRQ Controller)
0xCC004000 ----------------------- MI (Memory Interface & Protection)
0xCC005000 ----------------------- DSP / Audio DMA
0xCC006400 ----------------------- AI (Audio Interface & DAC)
0xCC006800 ----------------------- EXI (External Interface Channels 0-2)
0xCC008000 ----------------------- WGPIPE (Write-Gather Pipe / Direct GPU FIFO)
```

#### Dettaglio dei Registri Chiave da Emulare

```cpp
// Registri Command Processor (CP) - Gestione del FIFO dei comandi grafici
#define CP_STATUS_REG        0xCC000000  // Stato del FIFO (overflow, underflow, pronto)
#define CP_CONTROL_REG       0xCC000004  // Abilitazione FIFO e interrupt
#define CP_FIFO_BASE_LO      0xCC000020  // Indirizzo base FIFO in RAM (parte bassa)
#define CP_FIFO_BASE_HI      0xCC000022  // Indirizzo base FIFO in RAM (parte alta)
#define CP_FIFO_END_LO       0xCC000024  // Limite finale del buffer circolare FIFO
#define CP_FIFO_END_HI       0xCC000026
#define CP_FIFO_COUNT_LO     0xCC000028  // Numero di byte pendenti nel buffer
#define CP_FIFO_WRITE_PTR_LO 0xCC00002C  // Puntatore di scrittura della CPU
#define CP_FIFO_READ_PTR_LO  0xCC000030  // Puntatore di lettura della GPU

// Registri Pixel Engine (PE)
#define PE_Z_CONFIG          0xCC001000  // Z-buffer mode (Invert, Near, Far)
#define PE_ALPHA_CONFIG      0xCC001002  // Alpha test / blending equation
#define PE_STATUS_REG        0xCC00100A  // Token interrupt e stato frame

// Registri Video Interface (VI)
#define VI_VERTICAL_TIMING   0xCC002000  // Timing verticale del quadro TV
#define VI_DISPLAY_CONFIG    0xCC002002  // Configurazione display (NTSC/PAL/Progressive)
#define VI_FB_TOP_LO         0xCC00201C  // Puntatore al Framebuffer superiore in MEM1
#define VI_FB_TOP_HI         0xCC00201E
#define VI_FB_BOTTOM_LO      0xCC002024  // Puntatore al Framebuffer inferiore (interlacciato)
#define VI_FB_BOTTOM_HI      0xCC002026

// Registri Processor Interface (PI)
#define PI_INT_CAUSE         0xCC003000  // Registro delle cause degli interrupt (R/W)
#define PI_INT_MASK          0xCC003004  // Maschera di abilitazione interrupt
#define PI_FIFO_BASE         0xCC00300C  // Base del FIFO diretto
#define PI_FIFO_END          0xCC003010  // Fine del FIFO diretto
#define PI_FIFO_WRITE_PTR    0xCC003014  // Puntatore FIFO CPU

// Registri Audio Interface (AI)
#define AI_CONTROL           0xCC006400  // Controllo campionamento (48kHz/32kHz) e volume
#define AI_DMA_ADDR          0xCC00500A  // Indirizzo buffer circolare PCM in MEM1
#define AI_DMA_LEN           0xCC00500C  // Lunghezza del blocco audio pendente
```

---

### 1.3 Mappa dei Registri Hollywood IPC (`0x0D000000` / `0xCD000000`)

La CPU Broadway comunica con il coprocessore ARM (**Starlet**, che esegue il microkernel IOS) attraverso il protocollo **IPC Mailbox**:

| Indirizzo Virtuale | Nome Registro | Direzione | Funzione |
| :--- | :--- | :--- | :--- |
| `0xCD800000` | `HW_IPC_PPCMSG` | Broadway $\to$ Starlet | Puntatore all'indirizzo in RAM della struttura `IPCRequest` (0x40 byte) |
| `0xCD800004` | `HW_IPC_PPCCTRL` | Broadway $\to$ Starlet | Flag di controllo: bit 0 = Invia richiesta, bit 1 = ACK ricevuto |
| `0xCD800008` | `HW_IPC_ARMMSG` | Starlet $\to$ Broadway | Puntatore restituito da Starlet al termine dell'operazione |
| `0xCD80000C` | `HW_IPC_ARMCTRL` | Starlet $\to$ Broadway | Notifica di completamento (genera l'interrupt IPC su Broadway) |

---

## 2. Protocollo Inter-Process Communication (IOS IPC)

Per garantire la compatibilità con **tutti i giochi Wii senza eccezioni**, il runtime Windows deve implementare il ricevitore dei comandi IPC di Hollywood. Quando il gioco vuole leggere un file, salvare dati sulla NAND, interrogare la memoria o sincronizzare il Wiimote, invia un pacchetto da **64 byte (`0x40`)** allineato a 32 byte.

### 2.1 Struttura del Pacchetto IPC (`IPCRequest`)

```cpp
#pragma pack(push, 1)
struct IOVector {
    uint32_t data; // Puntatore all'array di dati in memoria Wii
    uint32_t len;  // Lunghezza del buffer in byte
};

struct IPCRequest {
    uint32_t cmd;      // Tipo di comando (1=Open, 2=Close, 3=Read, 4=Write, 5=Seek, 6=IOCtl, 7=IOCtlV)
    int32_t  ret;      // Valore di ritorno (impostato dal runtime nativo)
    int32_t  fd;       // File Descriptor del device aperto
    
    union {
        // cmd == 1: Open
        struct {
            uint32_t path; // Puntatore a stringa (es. "/dev/di", "/dev/fs", "/shared2/sys/SYSCONF")
            uint32_t mode; // Modalità di apertura (Read, Write, R/W)
        } open;

        // cmd == 3 o 4: Read / Write
        struct {
            uint32_t data; // Buffer memoria
            uint32_t len;  // Dimensione in byte
        } read_write;

        // cmd == 5: Seek
        struct {
            int32_t  offset;
            int32_t  origin;
        } seek;

        // cmd == 6: IOCtl
        struct {
            uint32_t ioctl;
            uint32_t in_buf;
            uint32_t in_len;
            uint32_t out_buf;
            uint32_t out_len;
        } ioctl;

        // cmd == 7: IOCtlV (Vectorial I/O)
        struct {
            uint32_t ioctl;
            uint32_t in_count;   // Numero di puntatori IOVector in input
            uint32_t out_count;  // Numero di puntatori IOVector in output
            uint32_t vec_ptr;    // Puntatore in RAM all'array di IOVector
        } ioctlv;
    };
    uint8_t padding[0x40 - 28]; // Pad fino a 64 byte totali
};
#pragma pack(pop)
```

### 2.2 Tabella dei Device Driver di Sistema (`/dev/`)

Ogni chiamata `open` su un device deve essere gestita dal dispatcher nativo:

| Nome Device | Scopo | Traduzione su Windows |
| :--- | :--- | :--- |
| `/dev/di` | **Disc Interface** (Lettura ISO/WBFS, cryptazione FST, TOC) | Mappato sul parser ISO/WBFS locale sul disco rigido |
| `/dev/fs` | **NAND File System** (Salvataggi di gioco, impostazioni) | Mappato su una cartella locale (es. `%APPDATA%/WiiPort/nand/`) |
| `/dev/usb/oh0` & `/dev/usb/ven` | **Bluetooth / Wiimote Subsystem** | Mappato su Win32 Bluetooth API o emulazione XInput/Mouse |
| `/dev/net/kd/request` | **WiiConnect24 / Wi-Fi** | Restituisce codice offline o emula socket WinSock2 |
| `/dev/aes` & `/dev/sha` | **Acceleratore Crittografico Hardware** | Risolto nativamente con Windows Cryptography API (BCrypt) |

---

## 3. PowerPC 750CL "Broadway": Set Istruzioni e Paired-Single SIMD

La CPU Broadway estende lo standard PowerPC 750 con **istruzioni vettoriali SIMD a doppia precisione singola** (*Paired-Singles*) e registri di quantizzazione grafica (*GQR*).

### 3.1 I Registri Graphics Quantization (GQR0 – GQR7)

I registri GQR controllano come i dati vengono dequantizzati durante il caricamento da memoria (`psq_l`) e quantizzati durante il salvataggio (`psq_st`).

```
Bits [31:28] : L_Scale (Fattore di scala signed per Load: 2^Scale)
Bits [27:24] : L_Type  (Tipo di dato in memoria per Load)
Bits [15:12] : S_Scale (Fattore di scala signed per Store: 2^Scale)
Bits [11:8]  : S_Type  (Tipo di dato convertito in memoria per Store)
```

#### Codifica dei Tipi di Quantizzazione (`Type`)

| Valore Tipo | Formato in Memoria | Formula di Conversione in Float (Dequantizzazione) |
| :--- | :--- | :--- |
| `0` | `float32` standard | Nessuna conversione, copia diretta |
| `4` | `uint8_t` | $\text{float} = \frac{\text{valore}}{2^{\text{L\_Scale}}}$ |
| `5` | `uint16_t` | $\text{float} = \frac{\text{valore}}{2^{\text{L\_Scale}}}$ |
| `6` | `int8_t` (signed) | $\text{float} = \frac{\text{valore}}{2^{\text{L\_Scale}}}$ |
| `7` | `int16_t` (signed) | $\text{float} = \frac{\text{valore}}{2^{\text{L\_Scale}}}$ |

---

### 3.2 Mappa degli Opcode Paired-Single

Tutte le operazioni Paired-Single utilizzano registri a 64-bit (`f0`–`f31`) contenenti due float: `ps0` (parte alta a 32-bit) e `ps1` (parte bassa a 32-bit).

```
Registro FPR 64-bit:
+-----------------------------------+-----------------------------------+
|         ps0 (float 32-bit)        |         ps1 (float 32-bit)        |
+-----------------------------------+-----------------------------------+
 63                               32 31                                0
```

#### A. Opcodes Primari: Caricamento e Salvataggio con Quantizzazione

| Istruzione | Opcode Primario | Tipo | Sintassi Assembly | Traduzione C++ / SSE2 |
| :--- | :--- | :--- | :--- | :--- |
| `psq_l` | **56** (`0x38`) | D-Form | `psq_l fD, d(rA), W, I` | Legge memoria, scala con `GQR[I]`, scrive su `fD.ps0` (e `fD.ps1` se W=0; se W=1 `fD.ps1 = 1.0f`) |
| `psq_lu` | **57** (`0x39`) | D-Form | `psq_lu fD, d(rA), W, I` | `psq_l` con aggiornamento del registro base `rA = rA + d` |
| `psq_st` | **60** (`0x3C`) | D-Form | `psq_st fS, d(rA), W, I` | Quantizza con `GQR[I]` e scrive in memoria (1 o 2 elementi in base a W) |
| `psq_stu` | **61** (`0x3D`) | D-Form | `psq_stu fS, d(rA), W, I`| `psq_st` con aggiornamento `rA = rA + d` |

#### B. Opcodes Aritmetici: Primario 4 (Compute Paired-Singles)

Tutte le operazioni matematiche Paired-Single condividono l'**Opcode primario 4** e sono differenziate dall'Extended Opcode (`XO`, bit [5:1]):

| Mnemonic | Extended Opcode (`XO`) | Operazione Eseguita |
| :--- | :--- | :--- |
| `ps_add` | `10` | $fD.ps0 = fA.ps0 + fB.ps0$; $fD.ps1 = fA.ps1 + fB.ps1$ |
| `ps_sub` | `21` | $fD.ps0 = fA.ps0 - fB.ps0$; $fD.ps1 = fA.ps1 - fB.ps1$ |
| `ps_mul` | `25` | $fD.ps0 = fA.ps0 \times fC.ps0$; $fD.ps1 = fA.ps1 \times fC.ps1$ |
| `ps_madd` | `29` | $fD.ps0 = (fA.ps0 \times fC.ps0) + fB.ps0$; $fD.ps1 = (fA.ps1 \times fC.ps1) + fB.ps1$ |
| `ps_msub` | `28` | $fD.ps0 = (fA.ps0 \times fC.ps0) - fB.ps0$; $fD.ps1 = (fA.ps1 \times fC.ps1) - fB.ps1$ |
| `ps_merge00` | `560` | $fD.ps0 = fA.ps0$; $fD.ps1 = fB.ps0$ |
| `ps_merge01` | `592` | $fD.ps0 = fA.ps0$; $fD.ps1 = fB.ps1$ |
| `ps_merge10` | `624` | $fD.ps0 = fA.ps1$; $fD.ps1 = fB.ps0$ |
| `ps_merge11` | `656` | $fD.ps0 = fA.ps1$; $fD.ps1 = fB.ps1$ |
| `ps_muls0` | `12` | $fD.ps0 = fA.ps0 \times fC.ps0$; $fD.ps1 = fA.ps1 \times fC.ps0$ (prodotto per scalare) |
| `ps_muls1` | `14` | $fD.ps0 = fA.ps0 \times fC.ps1$; $fD.ps1 = fA.ps1 \times fC.ps1$ |

---

## 4. Motore di Rilocazione per Moduli Dinamici (.REL)

I file `.rel` contengono una tabella di rilocazione compressa per differenze relative (`delta-encoded`). Ciascuna entry occupa esattamente **8 byte**.

### 4.1 Struttura Entry di Rilocazione (8 Byte)

```cpp
#pragma pack(push, 1)
struct RelRelocationEntry {
    uint16_t offset;   // Offset in byte rispetto all'indirizzo dell'ultima rilocazione
    uint8_t  type;     // Tipo di rilocazione o codice di controllo (202 / 203)
    uint8_t  section;  // ID della sezione di destinazione (0..N)
    uint32_t addend;   // Addend / Simbolo (se riferito al main.dol, è l'indirizzo assoluto)
};
#pragma pack(pop)
```

### 4.2 Tabella dei Tipi di Rilocazione PowerPC (`type`)

```cpp
enum RelType : uint8_t {
    R_PPC_NONE      = 0,
    R_PPC_ADDR32    = 1,   // Scrittura puntatore assoluto a 32-bit (*(u32*)addr = target)
    R_PPC_ADDR24    = 2,   // Istruzione di salto assoluto a 24-bit (b, bl)
    R_PPC_ADDR16    = 3,   // 16-bit assoluti
    R_PPC_ADDR16_LO = 4,   // Parte bassa di un indirizzo (ori rA, rB, LO16)
    R_PPC_ADDR16_HI = 5,   // Parte alta di un indirizzo (lis rA, HI16)
    R_PPC_ADDR16_HA = 6,   // Parte alta con aggiustamento di segno per addi (lis rA, HA16)
    R_PPC_ADDR14    = 7,   // Salto condizionale assoluto
    R_PPC_REL24     = 10,  // Salto relativo a 24-bit (b, bl relativo al PC corrente)
    R_PPC_REL14     = 11,  // Salto condizionale relativo al PC

    // Codici speciali di controllo della lista:
    R_REL_SECTION_SWITCH = 202, // 0xCA: Cambia la sezione corrente su cui applicare le rilocazioni
    R_REL_END_OF_LIST    = 203  // 0xCB: Fine della lista di rilocazioni per la sezione
};
```

---

## 5. Implementazione del Core di Dispatch e Memoria Nativa

Ecco il codice C++ fondamentale del **Controller di Memoria e della Global Dispatch Table** che costituisce la colonna portante del runtime:

```cpp
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <intrin.h>

struct PPCContext;
using NativeCodeBlock = void(*)(PPCContext*);

// Dimensione MEM1 (24MB) e MEM2 (64MB)
constexpr size_t MEM1_SIZE = 24 * 1024 * 1024;
constexpr size_t MEM2_SIZE = 64 * 1024 * 1024;

class UniversalWiiMemory {
private:
    uint8_t* mem1;
    uint8_t* mem2;
    NativeCodeBlock* dispatchTable; // Tabella di puntatori diretti per ogni istruzione (4 byte)

public:
    UniversalWiiMemory() {
        mem1 = (uint8_t*)_aligned_malloc(MEM1_SIZE, 64);
        mem2 = (uint8_t*)_aligned_malloc(MEM2_SIZE, 64);
        // Indicizzazione per 24MB di codice / 4 byte per istruzione = 6.291.456 entry
        dispatchTable = (NativeCodeBlock*)calloc(MEM1_SIZE / 4, sizeof(NativeCodeBlock));
    }

    ~UniversalWiiMemory() {
        _aligned_free(mem1);
        _aligned_free(mem2);
        free(dispatchTable);
    }

    // Registrazione statica di ogni funzione ricompilata
    inline void BindFunction(uint32_t ppcAddr, NativeCodeBlock func) {
        if ((ppcAddr & 0xFE000000) == 0x80000000) {
            uint32_t idx = (ppcAddr & 0x017FFFFF) >> 2;
            dispatchTable[idx] = func;
        }
    }

    // Risoluzione salti indiretti (bctr, bctrl, blr)
    inline void DispatchIndirectBranch(PPCContext* ctx, uint32_t targetAddress) {
        uint32_t idx = (targetAddress & 0x017FFFFF) >> 2;
        NativeCodeBlock target = dispatchTable[idx];
        if (target) {
            target(ctx);
        } else {
            printf("[FATAL] Salto indiretto non registrato: 0x%08X\n", targetAddress);
            abort();
        }
    }

    // Lettura universale con intercettazione MMIO
    inline uint32_t Read32(uint32_t addr) {
        // Intercetta MMIO Legacy (0xCC...) e Hollywood (0xCD...)
        if ((addr & 0xFF000000) == 0xCC000000 || (addr & 0xFF000000) == 0xCD000000) {
            return HandleMMIO_Read32(addr);
        }
        // MEM1 (0x80... / 0xC0...)
        if ((addr & 0x7E000000) == 0x00000000 || (addr & 0x7E000000) == 0x80000000) {
            uint32_t offset = addr & (MEM1_SIZE - 1);
            return _byteswap_ulong(*reinterpret_cast<uint32_t*>(mem1 + offset));
        }
        // MEM2 (0x90... / 0xD0...)
        if ((addr & 0x70000000) == 0x10000000 || (addr & 0x70000000) == 0x90000000) {
            uint32_t offset = (addr - 0x10000000) & (MEM2_SIZE - 1);
            return _byteswap_ulong(*reinterpret_cast<uint32_t*>(mem2 + offset));
        }
        return 0;
    }

    inline void Write32(uint32_t addr, uint32_t value) {
        if ((addr & 0xFF000000) == 0xCC000000 || (addr & 0xFF000000) == 0xCD000000) {
            HandleMMIO_Write32(addr, value);
            return;
        }
        if ((addr & 0x7E000000) == 0x00000000 || (addr & 0x7E000000) == 0x80000000) {
            uint32_t offset = addr & (MEM1_SIZE - 1);
            *reinterpret_cast<uint32_t*>(mem1 + offset) = _byteswap_ulong(value);
            return;
        }
        if ((addr & 0x70000000) == 0x10000000 || (addr & 0x70000000) == 0x90000000) {
            uint32_t offset = (addr - 0x10000000) & (MEM2_SIZE - 1);
            *reinterpret_cast<uint32_t*>(mem2 + offset) = _byteswap_ulong(value);
            return;
        }
    }

private:
    uint32_t HandleMMIO_Read32(uint32_t addr) {
        // Esempio: Lettura dello stato FIFO della GPU o dei timer
        if (addr == 0xCC000000) return 0x00000001; // CP Pronto
        return 0;
    }

    void HandleMMIO_Write32(uint32_t addr, uint32_t val) {
        // Esempio: Invio comando alla Mailbox Starlet/IOS
        if (addr == 0xCD800000) {
            // val = puntatore alla struttura IPCRequest
            ProcessIOS_IPC_Message(val);
        }
    }

    void ProcessIOS_IPC_Message(uint32_t msgPtr) {
        // Esegue il dispatch del comando (Open/Read/Write/IOCtl) verso il filesystem Windows
    }
};
```

---

## 6. Riepilogo dei Requisiti per il Lifter AOT

Per completare la generazione del codice C++ da PowerPC a x86_64, il traduttore AOT deve gestire:
1. **Integer Unit**: 32 GPR (`r0`-`r31`), flag di Condition Register (`cr0`-`cr7`), `xer`, `lr`, `ctr`.
2. **Paired-Single SIMD**: Caricamenti e scritture quantizzate basate su `GQR0`-`GQR7`, calcolo scalare e vettoriale su `f0`-`f31`.
3. **Infrastruttura di Branch**: Mappatura diretta dei branch condizionali locali (`bc`, `bdnz`) e reindirizzamento dei branch indiretti (`bctr`, `bctrl`) attraverso la `GlobalDispatchTable`.
4. **Scrittura Diretta su WGPIPE (`0xCC008000`)**: Trasformazione delle sequenze di invio vertici in comandi stream diretti verso il backend DirectX 12 / Vulkan.
