cmake_minimum_required(VERSION 3.20)
project(WiiUniversalRecompiler CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

# 1. Tool CLI di Ricompilazione Statica AOT (eseguibile da sviluppatore)
add_executable(wiirecomp
    src/recompiler/main.cpp
)

# 2. Libreria Runtime Nativo Windows
add_library(WiiNativeRuntime STATIC
    include/wii_types.hpp
    include/wii_runtime.hpp
    include/ppc_bitops.hpp
    include/gx_fifo.hpp
    include/ios_ipc.hpp
    include/rel_loader.hpp
    include/iso_fs_reader.hpp
    include/symbol_map_importer.hpp
    include/mod_engine.hpp
    include/config_manager.hpp
    include/native_renderer_d3d12.hpp
    include/audio_xaudio2.hpp
    include/input_manager.hpp
)

target_include_directories(WiiNativeRuntime PUBLIC include)

# 3. Target del Gioco Finale Nativo (Windows Win32 / DirectX 12)
# Quando 'recompiled_game.cpp' viene generato da wiirecomp, questo target produce Game_Native.exe
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/recompiled_game.cpp")
    add_executable(Game_Native WIN32
        src/runtime/native_main.cpp
        recompiled_game.cpp
    )

    target_link_libraries(Game_Native PRIVATE 
        WiiNativeRuntime 
        d3d12 
        dxgi 
        d3dcompiler 
        xaudio2 
        xinput
    )
endif()
