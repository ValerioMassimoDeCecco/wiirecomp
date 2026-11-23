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
add_library(WiiNativeRuntime INTERFACE)
target_include_directories(WiiNativeRuntime INTERFACE include)

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

# 4. Interfaccia Grafica con Console in Tempo Reale (Studio GUI)
add_executable(WiiRecomp_Studio WIN32
    src/gui/main_gui.cpp
)
target_link_libraries(WiiRecomp_Studio PRIVATE
    comctl32
    dwmapi
    shell32
)
