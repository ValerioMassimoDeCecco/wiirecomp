#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <iostream>
#include <fstream>
#include <cstring>
#include "wii_types.hpp"

namespace WiiPort {

// Comandi IPC supportati da IOS
enum class IOSCommand : uint32_t {
    Open   = 1,
    Close  = 2,
    Read   = 3,
    Write  = 4,
    Seek   = 5,
    IOCtl  = 6,
    IOCtlV = 7
};

// IOVector per chiamate vettoriali ioctlv
struct IOVector {
    uint32_t data; // Indirizzo di memoria Wii
    uint32_t len;  // Lunghezza del buffer in byte
};

#pragma pack(push, 1)
struct IPCRequest {
    uint32_t cmd;      // Tipo di comando (1..7)
    int32_t  ret;      // Codice di ritorno restituito da IOS
    int32_t  fd;       // File descriptor del device
    
    union {
        struct {
            uint32_t path; // Puntatore alla stringa del path in RAM Wii
            uint32_t mode;
        } open;

        struct {
            uint32_t data;
            uint32_t len;
        } read_write;

        struct {
            int32_t offset;
            int32_t origin;
        } seek;

        struct {
            uint32_t ioctl;
            uint32_t in_buf;
            uint32_t in_len;
            uint32_t out_buf;
            uint32_t out_len;
        } ioctl;

        struct {
            uint32_t ioctl;
            uint32_t in_count;
            uint32_t out_count;
            uint32_t vec_ptr; // Puntatore in RAM all'array di IOVector
        } ioctlv;
    };
    uint8_t padding[0x40 - 28]; // Pad a 64 byte
};
#pragma pack(pop)

// Interfaccia per i device virtuali (/dev/...)
class IOSDevice {
public:
    virtual ~IOSDevice() = default;
    virtual int32_t Open(const std::string& path, uint32_t mode) = 0;
    virtual int32_t Close(int32_t fd) = 0;
    virtual int32_t Read(int32_t fd, uint8_t* dest, uint32_t len) = 0;
    virtual int32_t Write(int32_t fd, const uint8_t* src, uint32_t len) = 0;
    virtual int32_t Seek(int32_t fd, int32_t offset, int32_t origin) = 0;
    virtual int32_t IOCtl(int32_t fd, uint32_t cmd, const uint8_t* in, uint32_t inLen, uint8_t* out, uint32_t outLen) = 0;
    virtual int32_t IOCtlV(int32_t fd, uint32_t cmd, uint32_t inCount, uint32_t outCount, IOVector* vectors) = 0;
};

// Implementazione di /dev/di (Disc Interface - Lettura Dati Gioco)
class DeviceDiscInterface : public IOSDevice {
private:
    std::string rootGameDirectory;

public:
    DeviceDiscInterface(const std::string& gameDir = "./game_data") : rootGameDirectory(gameDir) {}

    int32_t Open(const std::string& path, uint32_t mode) override {
        return 1; // File descriptor per il drive ottico
    }
    int32_t Close(int32_t fd) override { return 0; }
    int32_t Read(int32_t fd, uint8_t* dest, uint32_t len) override { return len; }
    int32_t Write(int32_t fd, const uint8_t* src, uint32_t len) override { return -1; }
    int32_t Seek(int32_t fd, int32_t offset, int32_t origin) override { return offset; }

    int32_t IOCtl(int32_t fd, uint32_t cmd, const uint8_t* in, uint32_t inLen, uint8_t* out, uint32_t outLen) override {
        // Esempio comandi DI:
        // 0x12: Inquire status
        // 0x88: Read Unencrypted Disc
        if (cmd == 0x12 && out && outLen >= 4) {
            *reinterpret_cast<uint32_t*>(out) = 0; // Disco presente e pronto
            return 0;
        }
        return 0;
    }

    int32_t IOCtlV(int32_t fd, uint32_t cmd, uint32_t inCount, uint32_t outCount, IOVector* vectors) override {
        return 0;
    }
};

// Implementazione di /dev/fs (NAND File System - Salvataggi e Configurazione)
class DeviceFileSystem : public IOSDevice {
private:
    std::string nandDirectory;

public:
    DeviceFileSystem(const std::string& nandDir = "./nand") : nandDirectory(nandDir) {}

    int32_t Open(const std::string& path, uint32_t mode) override {
        return 2; // FD virtuale
    }
    int32_t Close(int32_t fd) override { return 0; }
    int32_t Read(int32_t fd, uint8_t* dest, uint32_t len) override { return len; }
    int32_t Write(int32_t fd, const uint8_t* src, uint32_t len) override { return len; }
    int32_t Seek(int32_t fd, int32_t offset, int32_t origin) override { return 0; }
    int32_t IOCtl(int32_t fd, uint32_t cmd, const uint8_t* in, uint32_t inLen, uint8_t* out, uint32_t outLen) override { return 0; }
    int32_t IOCtlV(int32_t fd, uint32_t cmd, uint32_t inCount, uint32_t outCount, IOVector* vectors) override { return 0; }
};

// Server Centrale IOS IPC
class IOSIPCServer {
private:
    std::unordered_map<std::string, std::shared_ptr<IOSDevice>> devices;
    std::unordered_map<int32_t, std::shared_ptr<IOSDevice>> openHandles;
    int32_t nextFd = 10;

public:
    IOSIPCServer() {
        // Registrazione dei device di default
        devices["/dev/di"] = std::make_shared<DeviceDiscInterface>();
        devices["/dev/fs"] = std::make_shared<DeviceFileSystem>();
    }

    // Riceve ed elabora il pacchetto da 64-byte dalla memoria Wii
    void ProcessRequest(uint8_t* wiiRam, uint32_t requestPhysicalAddr) {
        IPCRequest* req = reinterpret_cast<IPCRequest*>(wiiRam + (requestPhysicalAddr & 0x01FFFFFF));

        uint32_t cmd = WII_BYTESWAP32(req->cmd);
        int32_t fd = (int32_t)WII_BYTESWAP32((uint32_t)req->fd);

        switch (static_cast<IOSCommand>(cmd)) {
            case IOSCommand::Open: {
                uint32_t pathPtr = WII_BYTESWAP32(req->open.path);
                const char* pathStr = reinterpret_cast<const char*>(wiiRam + (pathPtr & 0x01FFFFFF));
                uint32_t mode = WII_BYTESWAP32(req->open.mode);

                std::string path(pathStr);
                auto it = devices.find(path);
                if (it != devices.end()) {
                    int32_t handle = nextFd++;
                    openHandles[handle] = it->second;
                    it->second->Open(path, mode);
                    req->ret = (int32_t)WII_BYTESWAP32((uint32_t)handle);
                } else {
                    req->ret = (int32_t)WII_BYTESWAP32((uint32_t)-1); // Non trovato
                }
                break;
            }

            case IOSCommand::Close: {
                auto it = openHandles.find(fd);
                if (it != openHandles.end()) {
                    it->second->Close(fd);
                    openHandles.erase(it);
                    req->ret = 0;
                } else {
                    req->ret = (int32_t)WII_BYTESWAP32((uint32_t)-1);
                }
                break;
            }

            case IOSCommand::IOCtl: {
                auto it = openHandles.find(fd);
                if (it != openHandles.end()) {
                    uint32_t ioctlCode = WII_BYTESWAP32(req->ioctl.ioctl);
                    uint32_t inPtr = WII_BYTESWAP32(req->ioctl.in_buf);
                    uint32_t inLen = WII_BYTESWAP32(req->ioctl.in_len);
                    uint32_t outPtr = WII_BYTESWAP32(req->ioctl.out_buf);
                    uint32_t outLen = WII_BYTESWAP32(req->ioctl.out_len);

                    uint8_t* inBuf = (inPtr && inLen) ? (wiiRam + (inPtr & 0x01FFFFFF)) : nullptr;
                    uint8_t* outBuf = (outPtr && outLen) ? (wiiRam + (outPtr & 0x01FFFFFF)) : nullptr;

                    int32_t result = it->second->IOCtl(fd, ioctlCode, inBuf, inLen, outBuf, outLen);
                    req->ret = (int32_t)WII_BYTESWAP32((uint32_t)result);
                }
                break;
            }

            default:
                req->ret = 0;
                break;
        }
    }
};

} // namespace WiiPort
