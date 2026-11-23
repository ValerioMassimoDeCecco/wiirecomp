#pragma once
#include <windows.h>
#include <xaudio2.h>
#include <cstdint>
#include <vector>
#include <iostream>
#include "wii_types.hpp"

#pragma comment(lib, "xaudio2.lib")

namespace WiiPort {

// Il sottosistema Audio Interface (AI) della Wii suona flussi PCM 16-bit Stereo a 32 kHz o 48 kHz
class XAudio2Manager {
private:
    IXAudio2* xaudio2 = nullptr;
    IXAudio2MasteringVoice* masteringVoice = nullptr;
    IXAudio2SourceVoice* sourceVoice = nullptr;
    WAVEFORMATEX waveFormat = {};

    static constexpr size_t AUDIO_BUFFER_SIZE = 4096;
    uint8_t doubleBuffer[2][AUDIO_BUFFER_SIZE];
    uint32_t currentBufferIndex = 0;

public:
    bool Initialize(uint32_t sampleRate = 48000) {
        if (FAILED(XAudio2Create(&xaudio2, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
            std::cerr << "[AUDIO] Errore inizializzazione XAudio2\n";
            return false;
        }

        if (FAILED(xaudio2->CreateMasteringVoice(&masteringVoice))) {
            std::cerr << "[AUDIO] Errore creazione Mastering Voice\n";
            return false;
        }

        // Formato audio standard Wii: 16-bit Signed PCM, Stereo, 48000 Hz
        waveFormat.wFormatTag = WAVE_FORMAT_PCM;
        waveFormat.nChannels = 2; // Stereo
        waveFormat.nSamplesPerSec = sampleRate;
        waveFormat.wBitsPerSample = 16;
        waveFormat.nBlockAlign = (waveFormat.nChannels * waveFormat.wBitsPerSample) / 8;
        waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;
        waveFormat.cbSize = 0;

        if (FAILED(xaudio2->CreateSourceVoice(&sourceVoice, &waveFormat))) {
            std::cerr << "[AUDIO] Errore creazione Source Voice\n";
            return false;
        }

        sourceVoice->Start(0);
        std::cout << "[AUDIO] XAudio2 pronto (PCM 16-bit Stereo @" << sampleRate << " Hz)\n";
        return true;
    }

    // Invia un blocco di campioni PCM letti dall'Audio Interface DMA in MEM1
    void StreamSamples(const int16_t* samples, size_t sampleCount) {
        if (!sourceVoice) return;

        XAUDIO2_VOICE_STATE state;
        sourceVoice->GetState(&state);
        if (state.BuffersQueued >= 2) return; // Evita latenza eccessiva se il buffer è già pieno

        size_t bytesToCopy = sampleCount * sizeof(int16_t);
        if (bytesToCopy > AUDIO_BUFFER_SIZE) bytesToCopy = AUDIO_BUFFER_SIZE;

        // I campioni audio della Wii sono Big-Endian -> swap a 16-bit per x86
        int16_t* dest = reinterpret_cast<int16_t*>(doubleBuffer[currentBufferIndex]);
        for (size_t i = 0; i < sampleCount; ++i) {
            dest[i] = (int16_t)WII_BYTESWAP16((uint16_t)samples[i]);
        }

        XAUDIO2_BUFFER buffer = {};
        buffer.AudioBytes = (UINT32)bytesToCopy;
        buffer.pAudioData = doubleBuffer[currentBufferIndex];
        buffer.Flags = 0;

        sourceVoice->SubmitSourceBuffer(&buffer);
        currentBufferIndex ^= 1; // Alterna double buffer
    }

    void Shutdown() {
        if (sourceVoice) {
            sourceVoice->Stop();
            sourceVoice->DestroyVoice();
            sourceVoice = nullptr;
        }
        if (masteringVoice) {
            masteringVoice->DestroyVoice();
            masteringVoice = nullptr;
        }
        if (xaudio2) {
            xaudio2->Release();
            xaudio2 = nullptr;
        }
    }
};

} // namespace WiiPort
