#pragma once
#include <windows.h>
#include <xinput.h>
#include <cstdint>
#include <iostream>

#pragma comment(lib, "xinput.lib")

namespace WiiPort {

// Stato virtuale unificato di un controller Wii (Wiimote + Nunchuk)
struct VirtualWiimoteState {
    // Puntatore Infrarossi (IR Pointing su schermo, normalizzato 0.0f .. 1.0f)
    float irX = 0.5f;
    float irY = 0.5f;

    // Tasti Wiimote
    bool btnA     = false;
    bool btnB     = false;
    bool btn1     = false;
    bool btn2     = false;
    bool btnPlus  = false;
    bool btnMinus = false;
    bool btnHome  = false;
    bool dpadUp   = false;
    bool dpadDown = false;
    bool dpadLeft = false;
    bool dpadRight= false;

    // Nunchuk (Stick analogico [-1.0f .. 1.0f] e pulsanti dorsali C, Z)
    float nunchukStickX = 0.0f;
    float nunchukStickY = 0.0f;
    bool  btnC = false;
    bool  btnZ = false;

    // Dati accelerometro / inclinazione (G-Sensor)
    float accelX = 0.0f;
    float accelY = 1.0f; // 1G verso il basso a riposo
    float accelZ = 0.0f;
};

class InputManager {
public:
    VirtualWiimoteState controllerState[4];

    // Aggiorna lo stato combinando Controller Xbox (XInput) e Mouse di Windows
    void Poll(HWND hwnd) {
        // 1. Controller 0 (Giocatore 1) via XInput
        XINPUT_STATE xstate;
        ZeroMemory(&xstate, sizeof(XINPUT_STATE));

        if (XInputGetState(0, &xstate) == ERROR_SUCCESS) {
            WORD buttons = xstate.Gamepad.wButtons;

            controllerState[0].btnA     = (buttons & XINPUT_GAMEPAD_A) != 0;
            controllerState[0].btnB     = (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0 || (xstate.Gamepad.bRightTrigger > 30);
            controllerState[0].btn1     = (buttons & XINPUT_GAMEPAD_X) != 0;
            controllerState[0].btn2     = (buttons & XINPUT_GAMEPAD_Y) != 0;
            controllerState[0].btnPlus  = (buttons & XINPUT_GAMEPAD_START) != 0;
            controllerState[0].btnMinus = (buttons & XINPUT_GAMEPAD_BACK) != 0;

            controllerState[0].dpadUp   = (buttons & XINPUT_GAMEPAD_DPAD_UP) != 0;
            controllerState[0].dpadDown = (buttons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
            controllerState[0].dpadLeft = (buttons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
            controllerState[0].dpadRight= (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;

            // Stick sinistro -> Nunchuk Stick
            float lx = xstate.Gamepad.sThumbLX;
            float ly = xstate.Gamepad.sThumbLY;
            controllerState[0].nunchukStickX = (lx >= 0 ? lx / 32767.0f : lx / 32768.0f);
            controllerState[0].nunchukStickY = (ly >= 0 ? ly / 32767.0f : ly / 32768.0f);

            controllerState[0].btnC = (buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
            controllerState[0].btnZ = (xstate.Gamepad.bLeftTrigger > 30);

            // Stick destro -> Simulazione puntatore IR con gamepad
            float rx = (float)xstate.Gamepad.sThumbRX / 32768.0f;
            float ry = (float)xstate.Gamepad.sThumbRY / 32768.0f;
            if (fabs(rx) > 0.2f || fabs(ry) > 0.2f) {
                controllerState[0].irX += rx * 0.02f;
                controllerState[0].irY -= ry * 0.02f; // Inversione asse Y
            }
        }

        // 2. Mouse di Windows per il puntamento di precisione IR
        POINT mousePos;
        if (GetCursorPos(&mousePos) && ScreenToClient(hwnd, &mousePos)) {
            RECT rect;
            GetClientRect(hwnd, &rect);
            if (rect.right > 0 && rect.bottom > 0) {
                float mx = (float)mousePos.x / (float)rect.right;
                float my = (float)mousePos.y / (float)rect.bottom;

                // Se il mouse è all'interno della finestra di gioco, sovrascrive il puntatore IR
                if (mx >= 0.0f && mx <= 1.0f && my >= 0.0f && my <= 1.0f) {
                    controllerState[0].irX = mx;
                    controllerState[0].irY = my;
                }
            }
        }

        // Clamp del puntatore su schermo [0.0f, 1.0f]
        if (controllerState[0].irX < 0.0f) controllerState[0].irX = 0.0f;
        if (controllerState[0].irX > 1.0f) controllerState[0].irX = 1.0f;
        if (controllerState[0].irY < 0.0f) controllerState[0].irY = 0.0f;
        if (controllerState[0].irY > 1.0f) controllerState[0].irY = 1.0f;
    }
};

} // namespace WiiPort
