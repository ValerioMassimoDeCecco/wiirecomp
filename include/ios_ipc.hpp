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

enum class IOSCommand : uint32_t {
    Open   = 1,
    Close  = 2,
    Read   = 3,
    Write  = 4,
    Seek   = 5,
    IOCtl  = 6,
    IOCtlV = 7
};

struct IOVector {
    uint32_t data;
    uint32_t len;
};

#pragma pack(push, 1)
struct IPCRequest {
    uint32_t cmd;
    int32_t  ret;
    int32_t  fd;

    union {
        struct {
            uint32_t path;
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
            uint32_t vec_ptr;
        } ioctlv;
    };
    uint8_t padding[0x40 - 28];
};
#pragma pack(pop)

class IOSDevice {
public:
    virtual ~IOSDevice() = default;
    virtual int32_t Open(const std::string& path, uint32_t mode) = 0;
    virtual int32_t Close(int32_t fd) = 0;
    virtual int32_t Read(int32_t fd, uint8_t* dest, uint32_t len) = 0;
    virtual int32_t Write(int32_t fd, const uint8_t* src, uint32_t len) = 0;
    virtual int32_t Seek(int32_t fd, int32_t offset, int32_t origin) = 0;
    virtual int32_t IOCtl(int32_t fd, uint32_t cmd, const uint8_t* in, uint32_t inLen, uint8_t* out, uint32_t outLen) = 0;
    virtual int32_t IOCtlV(int32_t fd, uint32_t cmd, uint32_t inCount, uint32_t outCount, const std::vector<uint8_t*>& ptrs, const std::vector<uint32_t>& lens) = 0;
};

class DeviceDiscInterface : public IOSDevice {
public:
    int32_t Open(const std::string&, uint32_t) override { return 1; }
    int32_t Close(int32_t) override { return 0; }
    int32_t Read(int32_t, uint8_t*, uint32_t len) override { return (int32_t)len; }
    int32_t Write(int32_t, const uint8_t*, uint32_t) override { return -1; }
    int32_t Seek(int32_t, int32_t offset, int32_t) override { return offset; }
    int32_t IOCtl(int32_t, uint32_t cmd, const uint8_t*, uint32_t, uint8_t* out, uint32_t outLen) override {
        if (cmd == 0x12 && out && outLen >= 4) {
            *reinterpret_cast<uint32_t*>(out) = 0;
        }
        return 0;
    }
    int32_t IOCtlV(int32_t, uint32_t, uint32_t, uint32_t, const std::vector<uint8_t*>&, const std::vector<uint32_t>&) override { return 0; }
};

class DeviceFileSystem : public IOSDevice {
public:
    int32_t Open(const std::string&, uint32_t) override { return 2; }
    int32_t Close(int32_t) override { return 0; }
    int32_t Read(int32_t, uint8_t* dest, uint32_t len) override {
        if (dest && len > 0) std::memset(dest, 0, len);
        return (int32_t)len;
    }
    int32_t Write(int32_t, const uint8_t*, uint32_t len) override { return (int32_t)len; }
    int32_t Seek(int32_t, int32_t offset, int32_t) override { return offset; }
    int32_t IOCtl(int32_t, uint32_t, const uint8_t*, uint32_t, uint8_t* out, uint32_t outLen) override {
        if (out && outLen > 0) std::memset(out, 0, outLen);
        return 0;
    }
    int32_t IOCtlV(int32_t, uint32_t, uint32_t, uint32_t, const std::vector<uint8_t*>&, const std::vector<uint32_t>&) override { return 0; }
};

class DeviceGenericStub : public IOSDevice {
public:
    int32_t Open(const std::string&, uint32_t) override { return 3; }
    int32_t Close(int32_t) override { return 0; }
    int32_t Read(int32_t, uint8_t* dest, uint32_t len) override {
        if (dest && len > 0) std::memset(dest, 0, len);
        return (int32_t)len;
    }
    int32_t Write(int32_t, const uint8_t*, uint32_t len) override { return (int32_t)len; }
    int32_t Seek(int32_t, int32_t, int32_t) override { return 0; }
    int32_t IOCtl(int32_t, uint32_t, const uint8_t*, uint32_t, uint8_t* out, uint32_t outLen) override {
        if (out && outLen > 0) std::memset(out, 0, outLen);
        return 0;
    }
    int32_t IOCtlV(int32_t, uint32_t, uint32_t inCount, uint32_t outCount,
                   const std::vector<uint8_t*>& ptrs, const std::vector<uint32_t>& lens) override {
        for (uint32_t i = inCount; i < inCount + outCount && i < ptrs.size(); ++i) {
            if (ptrs[i] && lens[i] > 0) std::memset(ptrs[i], 0, lens[i]);
        }
        return 0;
    }
};

class DeviceSysconfFile : public IOSDevice {
    std::vector<uint8_t> data;
    std::unordered_map<int32_t, uint32_t> pos;
public:
    DeviceSysconfFile() {
        // Header SYSCONF minimo valido ("SCv0" + aspect ratio 16:9, lingua inglese, stereo)
        data.assign(0x4000, 0);
        data[0] = 'S'; data[1] = 'C'; data[2] = 'v'; data[3] = '0';
        data[4] = 0; data[5] = 0; // 0 entries nel fallback semplice
        data[0x3FFC] = 'S'; data[0x3FFD] = 'C'; data[0x3FFE] = 'e'; data[0x3FFF] = 'd';
    }
    int32_t Open(const std::string&, uint32_t) override { return 4; }
    int32_t Close(int32_t fd) override { pos.erase(fd); return 0; }
    int32_t Read(int32_t fd, uint8_t* dest, uint32_t len) override {
        uint32_t cur = pos[fd];
        uint32_t avail = (cur < data.size()) ? (uint32_t)(data.size() - cur) : 0;
        uint32_t n = (len < avail) ? len : avail;
        if (dest && n > 0) std::memcpy(dest, data.data() + cur, n);
        pos[fd] = cur + n;
        return (int32_t)n;
    }
    int32_t Write(int32_t, const uint8_t*, uint32_t len) override { return (int32_t)len; }
    int32_t Seek(int32_t fd, int32_t offset, int32_t origin) override {
        int64_t np = (origin == 0) ? offset : (origin == 1) ? ((int64_t)pos[fd] + offset) : ((int64_t)data.size() + offset);
        if (np < 0) np = 0;
        if (np > (int64_t)data.size()) np = (int64_t)data.size();
        pos[fd] = (uint32_t)np;
        return (int32_t)np;
    }
    int32_t IOCtl(int32_t, uint32_t, const uint8_t*, uint32_t, uint8_t* out, uint32_t outLen) override {
        if (out && outLen >= 8) {
            // ISFS_IOCTL_GETFILESTATS: len=0x4000, pos=0
            uint32_t* o = reinterpret_cast<uint32_t*>(out);
            o[0] = WII_BYTESWAP32((uint32_t)data.size());
            o[1] = 0;
        }
        return 0;
    }
    int32_t IOCtlV(int32_t, uint32_t, uint32_t, uint32_t, const std::vector<uint8_t*>&, const std::vector<uint32_t>&) override { return 0; }
};

class IOSIPCServer {
private:
    std::unordered_map<std::string, std::shared_ptr<IOSDevice>> devices;
    std::shared_ptr<IOSDevice> fallbackStub;
    std::unordered_map<int32_t, std::shared_ptr<IOSDevice>> openHandles;
    int32_t nextFd = 10;

    static uint8_t* TranslatePtr(uint8_t* mem1, size_t m1Sz, uint8_t* mem2, size_t m2Sz, uint32_t addr, size_t len = 1) {
        if (addr == 0) return nullptr;
        uint8_t top1 = (addr >> 24) & 0xF8;
        if (top1 == 0x00 || top1 == 0x80 || top1 == 0xC0 || top1 == 0xE0) {
            uint32_t off = addr & (uint32_t)(m1Sz - 1);
            return (mem1 && off + len <= m1Sz) ? (mem1 + off) : nullptr;
        }
        uint8_t top2 = (addr >> 24) & 0xFC;
        if (top2 == 0x10 || top2 == 0x90 || top2 == 0xD0) {
            uint32_t off = addr & (uint32_t)(m2Sz - 1);
            return (mem2 && off + len <= m2Sz) ? (mem2 + off) : nullptr;
        }
        return nullptr;
    }

public:
    IOSIPCServer() {
        fallbackStub = std::make_shared<DeviceGenericStub>();
        devices["/dev/di"]            = std::make_shared<DeviceDiscInterface>();
        devices["/dev/fs"]            = std::make_shared<DeviceFileSystem>();
        devices["/shared2/sys/SYSCONF"] = std::make_shared<DeviceSysconfFile>();
        devices["/dev/es"]            = fallbackStub;
        devices["/dev/stm/immediate"] = fallbackStub;
        devices["/dev/stm/eventhook"] = fallbackStub;
        devices["/dev/usb/oh0"]       = fallbackStub;
        devices["/dev/usb/ven"]       = fallbackStub;
        devices["/dev/usb/hid"]       = fallbackStub;
        devices["/dev/net/kd/request"]= fallbackStub;
        devices["/dev/net/ip/top"]    = fallbackStub;
    }

    void ProcessRequest(uint8_t* wiiRam, uint32_t requestPhysicalAddr) {
        ProcessRequest(wiiRam, 128 * 1024 * 1024, nullptr, 0, requestPhysicalAddr);
    }

    void ProcessRequest(uint8_t* mem1, size_t m1Sz, uint8_t* mem2, size_t m2Sz, uint32_t requestAddr) {
        uint8_t* reqBytes = TranslatePtr(mem1, m1Sz, mem2, m2Sz, requestAddr, sizeof(IPCRequest));
        if (!reqBytes) return;
        IPCRequest* req = reinterpret_cast<IPCRequest*>(reqBytes);

        uint32_t cmd = WII_BYTESWAP32(req->cmd);
        int32_t fd = (int32_t)WII_BYTESWAP32((uint32_t)req->fd);

        switch (static_cast<IOSCommand>(cmd)) {
            case IOSCommand::Open: {
                uint32_t pathPtr = WII_BYTESWAP32(req->open.path);
                const char* pathStr = reinterpret_cast<const char*>(TranslatePtr(mem1, m1Sz, mem2, m2Sz, pathPtr, 1));
                uint32_t mode = WII_BYTESWAP32(req->open.mode);
                std::string path = pathStr ? std::string(pathStr) : "";
                auto it = devices.find(path);
                std::shared_ptr<IOSDevice> dev = (it != devices.end()) ? it->second : fallbackStub;
                int32_t handle = nextFd++;
                openHandles[handle] = dev;
                dev->Open(path, mode);
                req->ret = (int32_t)WII_BYTESWAP32((uint32_t)handle);
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
            case IOSCommand::Read: {
                auto it = openHandles.find(fd);
                if (it != openHandles.end()) {
                    uint32_t dstPtr = WII_BYTESWAP32(req->read_write.data);
                    uint32_t len    = WII_BYTESWAP32(req->read_write.len);
                    uint8_t* dst    = TranslatePtr(mem1, m1Sz, mem2, m2Sz, dstPtr, len);
                    int32_t res     = it->second->Read(fd, dst, len);
                    req->ret = (int32_t)WII_BYTESWAP32((uint32_t)res);
                } else {
                    req->ret = (int32_t)WII_BYTESWAP32((uint32_t)-1);
                }
                break;
            }
            case IOSCommand::Write: {
                auto it = openHandles.find(fd);
                if (it != openHandles.end()) {
                    uint32_t srcPtr = WII_BYTESWAP32(req->read_write.data);
                    uint32_t len    = WII_BYTESWAP32(req->read_write.len);
                    const uint8_t* src = TranslatePtr(mem1, m1Sz, mem2, m2Sz, srcPtr, len);
                    int32_t res = it->second->Write(fd, src, len);
                    req->ret = (int32_t)WII_BYTESWAP32((uint32_t)res);
                } else {
                    req->ret = (int32_t)WII_BYTESWAP32((uint32_t)-1);
                }
                break;
            }
            case IOSCommand::Seek: {
                auto it = openHandles.find(fd);
                if (it != openHandles.end()) {
                    int32_t off = (int32_t)WII_BYTESWAP32((uint32_t)req->seek.offset);
                    int32_t org = (int32_t)WII_BYTESWAP32((uint32_t)req->seek.origin);
                    int32_t res = it->second->Seek(fd, off, org);
                    req->ret = (int32_t)WII_BYTESWAP32((uint32_t)res);
                } else {
                    req->ret = (int32_t)WII_BYTESWAP32((uint32_t)-1);
                }
                break;
            }
            case IOSCommand::IOCtl: {
                auto it = openHandles.find(fd);
                if (it != openHandles.end()) {
                    uint32_t ioctlCode = WII_BYTESWAP32(req->ioctl.ioctl);
                    uint32_t inPtr  = WII_BYTESWAP32(req->ioctl.in_buf);
                    uint32_t inLen  = WII_BYTESWAP32(req->ioctl.in_len);
                    uint32_t outPtr = WII_BYTESWAP32(req->ioctl.out_buf);
                    uint32_t outLen = WII_BYTESWAP32(req->ioctl.out_len);
                    uint8_t* inBuf  = TranslatePtr(mem1, m1Sz, mem2, m2Sz, inPtr, inLen);
                    uint8_t* outBuf = TranslatePtr(mem1, m1Sz, mem2, m2Sz, outPtr, outLen);
                    int32_t result = it->second->IOCtl(fd, ioctlCode, inBuf, inLen, outBuf, outLen);
                    req->ret = (int32_t)WII_BYTESWAP32((uint32_t)result);
                } else {
                    req->ret = (int32_t)WII_BYTESWAP32((uint32_t)-1);
                }
                break;
            }
            case IOSCommand::IOCtlV: {
                auto it = openHandles.find(fd);
                if (it != openHandles.end()) {
                    uint32_t ioctlCode = WII_BYTESWAP32(req->ioctlv.ioctl);
                    uint32_t inCnt  = WII_BYTESWAP32(req->ioctlv.in_count);
                    uint32_t outCnt = WII_BYTESWAP32(req->ioctlv.out_count);
                    uint32_t vecPtr = WII_BYTESWAP32(req->ioctlv.vec_ptr);
                    uint32_t tot    = inCnt + outCnt;
                    std::vector<uint8_t*> ptrs;
                    std::vector<uint32_t> lens;
                    if (tot <= 32) {
                        IOVector* vecs = reinterpret_cast<IOVector*>(TranslatePtr(mem1, m1Sz, mem2, m2Sz, vecPtr, tot * sizeof(IOVector)));
                        if (vecs) {
                            for (uint32_t i = 0; i < tot; ++i) {
                                uint32_t d = WII_BYTESWAP32(vecs[i].data);
                                uint32_t l = WII_BYTESWAP32(vecs[i].len);
                                ptrs.push_back(TranslatePtr(mem1, m1Sz, mem2, m2Sz, d, l));
                                lens.push_back(l);
                            }
                        }
                    }
                    int32_t result = it->second->IOCtlV(fd, ioctlCode, inCnt, outCnt, ptrs, lens);
                    req->ret = (int32_t)WII_BYTESWAP32((uint32_t)result);
                } else {
                    req->ret = (int32_t)WII_BYTESWAP32((uint32_t)-1);
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
