#pragma once
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdint>

namespace WiiPort {

// Gestione configurazione grafica, controlli e sistema salvata su file INI
struct GameConfig {
    // Grafica
    uint32_t windowWidth  = 1920;
    uint32_t windowHeight = 1080;
    bool     fullscreen   = false;
    bool     vsync        = true;
    float    aspectRatio  = 16.0f / 9.0f; // 1.777f (16:9) o 2.333f (21:9 Ultrawide)
    uint32_t targetFps    = 60;          // 60, 120, 144, 240

    // Audio
    float    masterVolume = 1.0f;        // 0.0f .. 1.0f

    // Input & Wiimote
    float    irSensitivity = 1.0f;
    float    stickDeadzone = 0.15f;

    // File System
    std::string nandDirectory = "./nand/";
    std::string dataDirectory = "./game_data/";

    bool LoadFromFile(const std::string& path = "config.ini") {
        std::vector<std::string> searchPaths = { path, "config.ini", "../config.ini", "../../config.ini", "C:/Users/Valerio/Desktop/wiirecomp/config.ini" };
        std::ifstream file;
        for (const auto& p : searchPaths) {
            file.open(p);
            if (file.is_open()) break;
            file.clear();
        }
        if (!file.is_open()) return false;

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);

            // Trim
            while (!key.empty() && key.back() == ' ') key.pop_back();
            while (!val.empty() && val.front() == ' ') val.erase(val.begin());

            if (key == "width") windowWidth = std::stoul(val);
            else if (key == "height") windowHeight = std::stoul(val);
            else if (key == "fullscreen") fullscreen = (val == "1" || val == "true");
            else if (key == "vsync") vsync = (val == "1" || val == "true");
            else if (key == "aspectRatio") aspectRatio = std::stof(val);
            else if (key == "targetFps") targetFps = std::stoul(val);
            else if (key == "masterVolume") masterVolume = std::stof(val);
        }

        std::cout << "[CONFIG] Caricata configurazione: " << windowWidth << "x" << windowHeight 
                  << " (VSync: " << (vsync ? "ON" : "OFF") << ")\n";
        return true;
    }

    void SaveToFile(const std::string& path = "config.ini") {
        std::ofstream file(path);
        if (!file.is_open()) return;

        file << "# Wii Native Windows Port - Configurazione\n";
        file << "width=" << windowWidth << "\n";
        file << "height=" << windowHeight << "\n";
        file << "fullscreen=" << (fullscreen ? "1" : "0") << "\n";
        file << "vsync=" << (vsync ? "1" : "0") << "\n";
        file << "aspectRatio=" << aspectRatio << "\n";
        file << "targetFps=" << targetFps << "\n";
        file << "masterVolume=" << masterVolume << "\n";
    }
};

extern GameConfig g_Config;

} // namespace WiiPort
