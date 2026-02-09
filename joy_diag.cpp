#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#pragma comment(lib, "winmm.lib")

int main() {
    UINT numDevs = joyGetNumDevs();
    printf("joyGetNumDevs() = %u\n\n", numDevs);
    for (UINT i = 0; i < numDevs && i < 16; i++) {
        JOYINFOEX info = {};
        info.dwSize = sizeof(JOYINFOEX);
        info.dwFlags = JOY_RETURNALL;
        if (joyGetPosEx(i, &info) == JOYERR_NOERROR) {
            JOYCAPSA caps = {};
            joyGetDevCapsA(i, &caps, sizeof(caps));
            printf("=== Joystick %u ===\n", i);
            printf("  Name:    \"%s\"\n", caps.szPname);
            printf("  MID/PID: %u / %u\n", caps.wMid, caps.wPid);
            printf("  Buttons: %u, Axes: %u\n\n", caps.wNumButtons, caps.wNumAxes);

            // Interactive: show button presses
            printf("  Press buttons to see indices (Ctrl+C to exit)...\n");
            DWORD prev = info.dwButtons;
            while (1) {
                JOYINFOEX j = {};
                j.dwSize = sizeof(JOYINFOEX);
                j.dwFlags = JOY_RETURNBUTTONS;
                if (joyGetPosEx(i, &j) == JOYERR_NOERROR) {
                    DWORD newBtns = j.dwButtons & ~prev;
                    if (newBtns) {
                        for (int b = 0; b < 32; b++) {
                            if (newBtns & (1u << b))
                                printf("  >> Button %d pressed\n", b);
                        }
                    }
                    prev = j.dwButtons;
                }
                Sleep(10);
            }
        }
    }
    printf("No joysticks found.\n");
    printf("Press Enter to exit...");
    getchar();
    return 0;
}
