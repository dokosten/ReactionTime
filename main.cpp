#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <shellapi.h>
#include <xinput.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "xinput.lib")

// Game states
enum GameState {
    STATE_START,        // Start menu, waiting for user to begin
    STATE_WAITING,      // Green screen, waiting for random delay
    STATE_READY,        // Red screen, measuring reaction time
    STATE_RESULT,       // Showing result, waiting for click to restart
    STATE_TOO_EARLY,    // Clicked too early, showing message
    STATE_MENU,         // ESC menu overlay
    STATE_KEYBINDS,     // Keybinds configuration screen
    STATE_ABOUT,                // About screen
    STATE_BENCHMARK_MENU,       // Benchmark sub-menu
    STATE_BENCHMARK_CPU,        // Running CPU single-core benchmark
    STATE_BENCHMARK_GPU,        // Running GPU benchmark
    STATE_BENCHMARK_MULTICORE,  // Running CPU multi-core benchmark
    STATE_BENCHMARK_RESULT      // Showing benchmark results
};

// UI Button
struct UIButton {
    RECT rect;
    int id;
    char text[64];
    bool active;
};

// Global variables
static HWND g_hwnd = NULL;
static GameState g_state = STATE_START;
static GameState g_stateBeforeMenu = STATE_START;
static LARGE_INTEGER g_perfFreq;
static LARGE_INTEGER g_startTime;
static LARGE_INTEGER g_flashTime;
static LARGE_INTEGER g_tooEarlyTime;
static double g_reactionTime = 0.0;
static double g_scores[5] = {0};
static int g_scoreCount = 0;
static int g_scoreIndex = 0;
static DWORD g_randomDelay = 0;
static bool g_timerStarted = false;

// Input binding types
enum InputType { BIND_KEYBOARD = 0, BIND_MOUSE = 1, BIND_GAMEPAD = 2 };
struct InputBinding { InputType type; int code; };

// Keybindings
static InputBinding g_bindReset = { BIND_KEYBOARD, 'R' };
static InputBinding g_bindClick = { BIND_MOUSE, 0 };  // mouse: 0=left, 1=right, 2=middle
static int g_rebindingAction = -1;     // -1=none, 0=rebinding reset, 1=rebinding click
static LARGE_INTEGER g_rebindStartTime = {}; // timestamp when rebind mode was entered
static char g_configPath[MAX_PATH] = {0};

// Benchmark state
static HANDLE g_benchThread = NULL;
static volatile bool g_benchDone = false;
static volatile bool g_benchCancel = false;
static volatile LONGLONG g_benchOps = 0;
static DWORD g_benchStartTick = 0;
static double g_lastBenchScore = 0.0;  // result of last benchmark (Mops/s)
static int g_lastBenchType = 0;        // 0=cpu, 1=gpu, 2=multicore
static const DWORD BENCH_DURATION_MS = 10000;

// Benchmark history
static char g_benchHistoryPath[MAX_PATH] = {0};
struct BenchHistoryEntry { char date[12]; double score; };
static BenchHistoryEntry g_benchHistory[20] = {};
static int g_benchHistoryCount = 0;

// Multi-core benchmark
#define MAX_BENCH_THREADS 64
static HANDLE g_benchThreads[MAX_BENCH_THREADS] = {};
static int g_benchThreadCount = 0;
// Per-thread counters, padded to avoid false sharing (64-byte cache lines)
struct alignas(64) PaddedCounter { volatile LONGLONG ops; };
static PaddedCounter g_threadOps[MAX_BENCH_THREADS] = {};

// Gamepad state (joyGetPosEx — works with PS5, Xbox, Switch Pro, etc.)
static int g_joyId = -1;           // cached joystick ID, -1 = needs scan
static DWORD g_joyScanTime = 0;    // last scan timestamp (GetTickCount)
static DWORD g_prevJoyButtons = 0;
static int g_prevJoyPOVDir = -1;   // -1=centered, 0=up, 1=right, 2=down, 3=left
static int g_prevStickDir = 0;     // 0=center, -1=up, 1=down (edge detection for thumbsticks)
static int g_joyStartButton = -1;  // detected Start/Menu/Options button index
// Controller type enum for button name display
enum JoyType { JOY_GENERIC = 0, JOY_XBOX = 1, JOY_PLAYSTATION = 2, JOY_SWITCH = 3 };
static JoyType g_joyType = JOY_GENERIC;

// XInput state (for Steam-wrapped controllers and native Xbox)
static bool g_useXInput = false;
static int g_xinputPlayer = -1;
static DWORD g_prevXInputButtons = 0;  // mapped to joyGetPosEx-compatible indices
static int g_prevXInputPOVDir = -1;
static int g_prevXInputStickDir = 0;

// Gamepad POV (D-pad) sentinel codes
static const int GAMEPAD_POV_UP    = 0x100;
static const int GAMEPAD_POV_RIGHT = 0x101;
static const int GAMEPAD_POV_DOWN  = 0x102;
static const int GAMEPAD_POV_LEFT  = 0x103;

// Get config file path (next to executable)
static void InitConfigPath() {
    GetModuleFileNameA(NULL, g_configPath, MAX_PATH);
    // Replace .exe with .cfg
    char* dot = strrchr(g_configPath, '.');
    if (dot) strcpy(dot, ".cfg");
    else strcat(g_configPath, ".cfg");

    // Benchmark history path (next to executable)
    GetModuleFileNameA(NULL, g_benchHistoryPath, MAX_PATH);
    dot = strrchr(g_benchHistoryPath, '.');
    if (dot) strcpy(dot, ".benchmarks");
    else strcat(g_benchHistoryPath, ".benchmarks");
}

// Save keybinds to config file
static void SaveKeybinds() {
    FILE* f = fopen(g_configPath, "w");
    if (!f) return;
    fprintf(f, "resetType=%d\n", (int)g_bindReset.type);
    fprintf(f, "resetCode=%d\n", g_bindReset.code);
    fprintf(f, "clickType=%d\n", (int)g_bindClick.type);
    fprintf(f, "clickCode=%d\n", g_bindClick.code);
    fclose(f);
}

// Load keybinds from config file
static void LoadKeybinds() {
    FILE* f = fopen(g_configPath, "r");
    if (!f) return;
    char line[128];
    bool hasNewFormat = false;
    int resetType = -1, resetCode = 0, clickType = -1, clickCode = 0;
    int legacyKeyReset = -1, legacyClickButton = -1;
    while (fgets(line, sizeof(line), f)) {
        int val;
        if (sscanf(line, "resetType=%d", &val) == 1) {
            resetType = val; hasNewFormat = true;
        } else if (sscanf(line, "resetCode=%d", &val) == 1) {
            resetCode = val; hasNewFormat = true;
        } else if (sscanf(line, "clickType=%d", &val) == 1) {
            clickType = val; hasNewFormat = true;
        } else if (sscanf(line, "clickCode=%d", &val) == 1) {
            clickCode = val; hasNewFormat = true;
        } else if (sscanf(line, "keyReset=%d", &val) == 1) {
            legacyKeyReset = val;
        } else if (sscanf(line, "clickButton=%d", &val) == 1) {
            legacyClickButton = val;
        }
    }
    fclose(f);

    if (hasNewFormat) {
        if (resetType >= 0 && resetType <= 2) {
            g_bindReset.type = (InputType)resetType;
            g_bindReset.code = resetCode;
        }
        if (clickType >= 0 && clickType <= 2) {
            g_bindClick.type = (InputType)clickType;
            g_bindClick.code = clickCode;
        }
    } else {
        // Legacy format migration
        if (legacyKeyReset >= 0) {
            g_bindReset.type = BIND_KEYBOARD;
            g_bindReset.code = legacyKeyReset;
        }
        if (legacyClickButton >= 0 && legacyClickButton <= 2) {
            g_bindClick.type = BIND_MOUSE;
            g_bindClick.code = legacyClickButton;
        }
    }
}

// Save a benchmark result to history file
static void SaveBenchResult(int type, double score) {
    FILE* f = fopen(g_benchHistoryPath, "a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "%d,%02d/%02d %02d:%02d,%.6f\n",
        type, st.wMonth, st.wDay, st.wHour, st.wMinute, score);
    fclose(f);
}

// Load benchmark history for a specific type (last 20, newest first)
static void LoadBenchHistory(int type) {
    g_benchHistoryCount = 0;
    FILE* f = fopen(g_benchHistoryPath, "r");
    if (!f) return;

    // Read all matching entries into a temp buffer
    BenchHistoryEntry all[1024];
    int total = 0;
    char line[256];
    while (fgets(line, sizeof(line), f) && total < 1024) {
        int t;
        char date[12];
        double score;
        if (sscanf(line, "%d,%11[^,],%lf", &t, date, &score) == 3 && t == type) {
            strncpy(all[total].date, date, sizeof(all[total].date) - 1);
            all[total].date[sizeof(all[total].date) - 1] = '\0';
            all[total].score = score;
            total++;
        }
    }
    fclose(f);

    // Take last 20, store newest first
    int start = total > 20 ? total - 20 : 0;
    int count = total - start;
    for (int i = 0; i < count; i++) {
        g_benchHistory[i] = all[start + count - 1 - i];
    }
    g_benchHistoryCount = count;
}

// UI state
static POINT g_mousePos = {0, 0};
static int g_hoveredButton = -1;
static int g_selectedButton = -1;  // keyboard/gamepad selected button, -1 = none
static UIButton g_buttons[16];
static int g_buttonCount = 0;

// Button IDs
enum ButtonID {
    BTN_KEYBINDS = 1,
    BTN_ABOUT,
    BTN_QUIT,
    BTN_BACK,
    BTN_REBIND_RESET,
    BTN_REBIND_CLICK,
    BTN_EMAIL,
    BTN_COPY_EMAIL,
    BTN_CLOSE,
    BTN_BENCHMARK,
    BTN_BENCH_CPU,
    BTN_BENCH_GPU,
    BTN_BENCH_MULTICORE
};

// Colors
static const COLORREF COLOR_GREEN = RGB(0, 180, 0);
static const COLORREF COLOR_RED = RGB(220, 0, 0);
static const COLORREF COLOR_YELLOW = RGB(255, 200, 0);
static const COLORREF COLOR_WHITE = RGB(255, 255, 255);
static const COLORREF COLOR_BLACK = RGB(0, 0, 0);
static const COLORREF COLOR_DARK_BG = RGB(25, 25, 30);
static const COLORREF COLOR_BUTTON = RGB(55, 55, 65);
static const COLORREF COLOR_BUTTON_HOVER = RGB(75, 75, 90);
static const COLORREF COLOR_ACCENT = RGB(220, 60, 60);

// Get current time in milliseconds since start
static double GetElapsedMs(LARGE_INTEGER start) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - start.QuadPart) * 1000.0 / (double)g_perfFreq.QuadPart;
}

// Generate random delay between 1000-5000ms
static void GenerateRandomDelay() {
    g_randomDelay = 1000 + (rand() % 4001);
}

// Start the waiting phase
static void StartWaiting() {
    g_state = STATE_WAITING;
    g_timerStarted = true;
    QueryPerformanceCounter(&g_startTime);
    GenerateRandomDelay();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// Add a score to the circular buffer
static void AddScore(double score) {
    g_scores[g_scoreIndex] = score;
    g_scoreIndex = (g_scoreIndex + 1) % 5;
    if (g_scoreCount < 5) {
        g_scoreCount++;
    }
}

// Calculate average of stored scores
static double GetAverageScore() {
    if (g_scoreCount == 0) return 0.0;
    double sum = 0.0;
    for (int i = 0; i < g_scoreCount; i++) {
        sum += g_scores[i];
    }
    return sum / g_scoreCount;
}

// Reset all scores
static void ResetScores() {
    for (int i = 0; i < 5; i++) {
        g_scores[i] = 0.0;
    }
    g_scoreCount = 0;
    g_scoreIndex = 0;
    g_reactionTime = 0.0;
    g_timerStarted = false;
    g_state = STATE_START;
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// Handle a game action: 0=reset scores, 1=game click
static void HandleAction(int action) {
    if (action == 0) {
        // Reset scores — only from game states
        if (g_state != STATE_MENU && g_state != STATE_KEYBINDS && g_state != STATE_ABOUT) {
            ResetScores();
        }
    } else if (action == 1) {
        // Game click
        switch (g_state) {
            case STATE_START:
                StartWaiting();
                break;
            case STATE_WAITING:
                g_state = STATE_TOO_EARLY;
                QueryPerformanceCounter(&g_tooEarlyTime);
                InvalidateRect(g_hwnd, NULL, FALSE);
                break;
            case STATE_READY:
            {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                g_reactionTime = (double)(now.QuadPart - g_flashTime.QuadPart) * 1000.0 / (double)g_perfFreq.QuadPart;
                AddScore(g_reactionTime);
                g_state = STATE_RESULT;
                InvalidateRect(g_hwnd, NULL, FALSE);
            }
            break;
            case STATE_RESULT:
                StartWaiting();
                break;
            case STATE_TOO_EARLY:
                // Ignore during penalty
                break;
            default:
                break;
        }
    }
}

// Capture a rebind input
static void CaptureRebind(InputType type, int code) {
    InputBinding* target = NULL;
    if (g_rebindingAction == 0) target = &g_bindReset;
    else if (g_rebindingAction == 1) target = &g_bindClick;
    if (target) {
        target->type = type;
        target->code = code;
    }
    g_rebindingAction = -1;
    SaveKeybinds();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// Check if binding matches given input
static bool BindingMatches(const InputBinding& binding, InputType type, int code) {
    return binding.type == type && binding.code == code;
}

// Get display name for a virtual key code
static const char* GetKeyDisplayName(int vk, char* buf, int bufSize) {
    if (vk >= 'A' && vk <= 'Z') {
        snprintf(buf, bufSize, "%c", (char)vk);
        return buf;
    }
    if (vk >= '0' && vk <= '9') {
        snprintf(buf, bufSize, "%c", (char)vk);
        return buf;
    }
    switch (vk) {
        case VK_SPACE:  return "SPACE";
        case VK_RETURN: return "ENTER";
        case VK_TAB:    return "TAB";
        case VK_BACK:   return "BACKSPACE";
        case VK_DELETE: return "DELETE";
        case VK_INSERT: return "INSERT";
        case VK_HOME:   return "HOME";
        case VK_END:    return "END";
        case VK_PRIOR:  return "PAGE UP";
        case VK_NEXT:   return "PAGE DOWN";
        case VK_UP:     return "UP";
        case VK_DOWN:   return "DOWN";
        case VK_LEFT:   return "LEFT";
        case VK_RIGHT:  return "RIGHT";
        case VK_OEM_1:       return ";";
        case VK_OEM_PLUS:    return "=";
        case VK_OEM_COMMA:   return ",";
        case VK_OEM_MINUS:   return "-";
        case VK_OEM_PERIOD:  return ".";
        case VK_OEM_2:       return "/";
        case VK_OEM_3:       return "`";
        case VK_OEM_4:       return "[";
        case VK_OEM_5:       return "\\";
        case VK_OEM_6:       return "]";
        case VK_OEM_7:       return "'";
        default:
            snprintf(buf, bufSize, "KEY 0x%02X", vk);
            return buf;
    }
}

// Get display name for a mouse button
static const char* GetMouseButtonName(int btn) {
    switch (btn) {
        case 0: return "Left Click";
        case 1: return "Right Click";
        case 2: return "Middle Click";
        default: return "Unknown";
    }
}

// Convert XInput wButtons to joyGetPosEx-compatible button bitmask (Xbox layout)
static DWORD XInputToJoyButtons(WORD xb) {
    DWORD j = 0;
    if (xb & XINPUT_GAMEPAD_A)              j |= (1u << 0);
    if (xb & XINPUT_GAMEPAD_B)              j |= (1u << 1);
    if (xb & XINPUT_GAMEPAD_X)              j |= (1u << 2);
    if (xb & XINPUT_GAMEPAD_Y)              j |= (1u << 3);
    if (xb & XINPUT_GAMEPAD_LEFT_SHOULDER)  j |= (1u << 4);
    if (xb & XINPUT_GAMEPAD_RIGHT_SHOULDER) j |= (1u << 5);
    if (xb & XINPUT_GAMEPAD_BACK)           j |= (1u << 6);
    if (xb & XINPUT_GAMEPAD_START)          j |= (1u << 7);
    if (xb & XINPUT_GAMEPAD_LEFT_THUMB)     j |= (1u << 8);
    if (xb & XINPUT_GAMEPAD_RIGHT_THUMB)    j |= (1u << 9);
    return j;
}

// Convert XInput D-pad buttons to POV direction (-1=centered, 0-3=up/right/down/left)
static int XInputDpadToDirection(WORD xb) {
    if (xb & XINPUT_GAMEPAD_DPAD_UP)    return 0;
    if (xb & XINPUT_GAMEPAD_DPAD_RIGHT) return 1;
    if (xb & XINPUT_GAMEPAD_DPAD_DOWN)  return 2;
    if (xb & XINPUT_GAMEPAD_DPAD_LEFT)  return 3;
    return -1;
}

// Convert POV hat angle to cardinal direction (-1=centered, 0-3=up/right/down/left)
static int POVToDirection(DWORD pov) {
    if (LOWORD(pov) == 0xFFFF) return -1;  // centered
    if (pov >= 31500 || pov < 4500)  return 0;  // up
    if (pov >= 4500  && pov < 13500) return 1;  // right
    if (pov >= 13500 && pov < 22500) return 2;  // down
    if (pov >= 22500 && pov < 31500) return 3;  // left
    return -1;
}

// Get display name for a gamepad button code
static const char* GetGamepadButtonName(int code, char* buf, int bufSize) {
    // D-pad directions (same across all controllers)
    switch (code) {
        case GAMEPAD_POV_UP:    return "D-pad Up";
        case GAMEPAD_POV_RIGHT: return "D-pad Right";
        case GAMEPAD_POV_DOWN:  return "D-pad Down";
        case GAMEPAD_POV_LEFT:  return "D-pad Left";
    }

    // Digital button names per controller type
    // Xbox (DirectInput): A B X Y LB RB Back Start LS RS
    static const char* xboxNames[] = {
        "A", "B", "X", "Y", "LB", "RB", "Back", "Start", "LS", "RS"
    };
    // PlayStation (DirectInput): Square Cross Circle Triangle L1 R1 L2 R2 Share Options L3 R3 PS Touchpad
    static const char* psNames[] = {
        "Square", "Cross", "Circle", "Triangle", "L1", "R1", "L2", "R2",
        "Share", "Options", "L3", "R3", "PS", "Touchpad"
    };
    // Switch Pro (DirectInput): B A X Y L R ZL ZR - + LS RS Home Capture
    static const char* switchNames[] = {
        "B", "A", "X", "Y", "L", "R", "ZL", "ZR", "-", "+", "LS", "RS", "Home", "Capture"
    };

    const char** names = NULL;
    int nameCount = 0;
    switch (g_joyType) {
        case JOY_XBOX:
            names = xboxNames;
            nameCount = sizeof(xboxNames) / sizeof(xboxNames[0]);
            break;
        case JOY_PLAYSTATION:
            names = psNames;
            nameCount = sizeof(psNames) / sizeof(psNames[0]);
            break;
        case JOY_SWITCH:
            names = switchNames;
            nameCount = sizeof(switchNames) / sizeof(switchNames[0]);
            break;
        default:
            break;
    }

    if (names && code >= 0 && code < nameCount) {
        return names[code];
    }

    // Fallback for unknown controller type or out-of-range button
    if (code >= 0 && code < 32) {
        snprintf(buf, bufSize, "Button %d", code + 1);
        return buf;
    }
    snprintf(buf, bufSize, "Button ?%d", code);
    return buf;
}

// Get display name for a unified input binding
static const char* GetBindingDisplayName(const InputBinding& binding, char* buf, int bufSize) {
    switch (binding.type) {
        case BIND_KEYBOARD:
            return GetKeyDisplayName(binding.code, buf, bufSize);
        case BIND_MOUSE:
            return GetMouseButtonName(binding.code);
        case BIND_GAMEPAD:
            return GetGamepadButtonName(binding.code, buf, bufSize);
        default:
            return "Unknown";
    }
}

// Get RAWINPUT button flag for configured button
static USHORT GetRawButtonFlag(int btn) {
    switch (btn) {
        case 0: return RI_MOUSE_LEFT_BUTTON_DOWN;
        case 1: return RI_MOUSE_RIGHT_BUTTON_DOWN;
        case 2: return RI_MOUSE_MIDDLE_BUTTON_DOWN;
        default: return RI_MOUSE_LEFT_BUTTON_DOWN;
    }
}

// Check if mouse cursor is inside the window client area (with margin)
static bool IsMouseInsideWindow(int margin = 10) {
    POINT cursorPos;
    if (!GetCursorPos(&cursorPos)) return false;

    RECT clientRect;
    GetClientRect(g_hwnd, &clientRect);

    POINT topLeft = { clientRect.left, clientRect.top };
    POINT bottomRight = { clientRect.right, clientRect.bottom };
    ClientToScreen(g_hwnd, &topLeft);
    ClientToScreen(g_hwnd, &bottomRight);

    return (cursorPos.x >= topLeft.x + margin &&
            cursorPos.x <= bottomRight.x - margin &&
            cursorPos.y >= topLeft.y + margin &&
            cursorPos.y <= bottomRight.y - margin);
}

// Draw text centered horizontally at given Y position
static void DrawCenteredText(HDC hdc, const char* text, int y, HFONT font, COLORREF color) {
    RECT clientRect;
    GetClientRect(g_hwnd, &clientRect);

    SelectObject(hdc, font);
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);

    SIZE textSize;
    GetTextExtentPoint32A(hdc, text, (int)strlen(text), &textSize);
    int x = (clientRect.right - textSize.cx) / 2;
    TextOutA(hdc, x, y, text, (int)strlen(text));
}

// Reset buttons array before each paint
static void ResetButtons() {
    g_buttonCount = 0;
    memset(g_buttons, 0, sizeof(g_buttons));
}

// Draw a button and register it for hit-testing
static void DrawButton(HDC hdc, int centerX, int y, int width, int height,
                       const char* text, int id, HFONT font) {
    RECT btnRect;
    btnRect.left = centerX - width / 2;
    btnRect.top = y;
    btnRect.right = centerX + width / 2;
    btnRect.bottom = y + height;

    // Register button for hit-testing
    if (g_buttonCount < 16) {
        g_buttons[g_buttonCount].rect = btnRect;
        g_buttons[g_buttonCount].id = id;
        strncpy(g_buttons[g_buttonCount].text, text, 63);
        g_buttons[g_buttonCount].active = true;
        g_buttonCount++;
    }

    // Determine if hovered (mouse) or selected (keyboard/gamepad)
    bool hovered = (g_hoveredButton == id) || (g_selectedButton == id);

    // Draw rounded rect background
    COLORREF btnColor = hovered ? COLOR_BUTTON_HOVER : COLOR_BUTTON;
    HBRUSH btnBrush = CreateSolidBrush(btnColor);
    HPEN btnPen = CreatePen(PS_SOLID, 1, hovered ? COLOR_ACCENT : RGB(80, 80, 95));
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, btnBrush);
    HPEN oldPen = (HPEN)SelectObject(hdc, btnPen);
    RoundRect(hdc, btnRect.left, btnRect.top, btnRect.right, btnRect.bottom, 12, 12);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(btnBrush);
    DeleteObject(btnPen);

    // Draw text centered in button
    SelectObject(hdc, font);
    SetTextColor(hdc, COLOR_WHITE);
    SetBkMode(hdc, TRANSPARENT);

    SIZE textSize;
    GetTextExtentPoint32A(hdc, text, (int)strlen(text), &textSize);
    int tx = btnRect.left + (width - textSize.cx) / 2;
    int ty = btnRect.top + (height - textSize.cy) / 2;
    TextOutA(hdc, tx, ty, text, (int)strlen(text));
}

// Hit-test buttons at given screen position, returns button id or -1
static int HitTestButtons(int screenX, int screenY) {
    POINT clientPt = { screenX, screenY };
    ScreenToClient(g_hwnd, &clientPt);

    for (int i = 0; i < g_buttonCount; i++) {
        if (g_buttons[i].active && PtInRect(&g_buttons[i].rect, clientPt)) {
            return g_buttons[i].id;
        }
    }
    return -1;
}

// Update hovered button from current mouse position
static void UpdateHoveredButton() {
    POINT clientPt = g_mousePos;
    ScreenToClient(g_hwnd, &clientPt);

    int newHovered = -1;
    for (int i = 0; i < g_buttonCount; i++) {
        if (g_buttons[i].active && PtInRect(&g_buttons[i].rect, clientPt)) {
            newHovered = g_buttons[i].id;
            break;
        }
    }

    if (newHovered != g_hoveredButton) {
        g_hoveredButton = newHovered;
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

// Forward declarations
static void OnButtonClick(int id);
static void CancelBenchmark();

// Get ordered list of button IDs for the current menu state
static int GetMenuButtonIds(int* ids, int maxIds) {
    int count = 0;
    switch (g_state) {
        case STATE_MENU:
            if (count < maxIds) ids[count++] = BTN_BENCHMARK;
            if (count < maxIds) ids[count++] = BTN_KEYBINDS;
            if (count < maxIds) ids[count++] = BTN_ABOUT;
            if (count < maxIds) ids[count++] = BTN_QUIT;
            if (count < maxIds) ids[count++] = BTN_CLOSE;
            break;
        case STATE_KEYBINDS:
            if (count < maxIds) ids[count++] = BTN_REBIND_RESET;
            if (count < maxIds) ids[count++] = BTN_REBIND_CLICK;
            if (count < maxIds) ids[count++] = BTN_BACK;
            break;
        case STATE_ABOUT:
        case STATE_BENCHMARK_RESULT:
            if (count < maxIds) ids[count++] = BTN_BACK;
            break;
        case STATE_BENCHMARK_MENU:
            if (count < maxIds) ids[count++] = BTN_BENCH_CPU;
            if (count < maxIds) ids[count++] = BTN_BENCH_MULTICORE;
            if (count < maxIds) ids[count++] = BTN_BENCH_GPU;
            if (count < maxIds) ids[count++] = BTN_BACK;
            break;
        default:
            break;
    }
    return count;
}

// Navigate menu selection up (-1) or down (+1)
static void NavigateMenu(int direction) {
    int ids[16];
    int count = GetMenuButtonIds(ids, 16);
    if (count == 0) return;

    if (g_selectedButton == -1) {
        // Nothing selected: pick first (down) or last (up)
        g_selectedButton = (direction > 0) ? ids[0] : ids[count - 1];
    } else {
        // Find current index
        int idx = -1;
        for (int i = 0; i < count; i++) {
            if (ids[i] == g_selectedButton) { idx = i; break; }
        }
        if (idx < 0) {
            g_selectedButton = ids[0];
        } else {
            int newIdx = idx + direction;
            if (newIdx < 0) newIdx = 0;
            if (newIdx >= count) newIdx = count - 1;
            g_selectedButton = ids[newIdx];
        }
    }
    g_hoveredButton = -1;  // clear mouse hover to avoid dual-highlight
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// Activate the currently selected button
static void ActivateSelectedButton() {
    if (g_selectedButton > 0) {
        OnButtonClick(g_selectedButton);
    }
}

// Toggle menu open/close (same behavior as ESC key)
static void ToggleMenu() {
    if (g_state == STATE_BENCHMARK_CPU || g_state == STATE_BENCHMARK_GPU || g_state == STATE_BENCHMARK_MULTICORE) {
        CancelBenchmark();
        return;
    }
    if (g_state == STATE_BENCHMARK_MENU) {
        g_state = STATE_MENU;
        g_selectedButton = -1;
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    if (g_state == STATE_BENCHMARK_RESULT) {
        g_state = STATE_BENCHMARK_MENU;
        g_selectedButton = -1;
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    if (g_state == STATE_KEYBINDS || g_state == STATE_ABOUT) {
        g_state = STATE_MENU;
        g_selectedButton = -1;
        g_rebindingAction = -1;
    } else if (g_state == STATE_MENU) {
        if (g_stateBeforeMenu == STATE_WAITING || g_stateBeforeMenu == STATE_READY) {
            g_state = STATE_START;
            g_timerStarted = false;
        } else {
            g_state = g_stateBeforeMenu;
        }
    } else {
        g_stateBeforeMenu = g_state;
        g_state = STATE_MENU;
        g_selectedButton = -1;
        g_timerStarted = false;
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// Check if a gamepad button index is the Start/Menu/Options button
static bool IsGamepadStartButton(int btn) {
    return g_joyStartButton >= 0 && btn == g_joyStartButton;
}

// Create app icon using GDI (red circle with white "RT")
static HICON CreateAppIcon(int size) {
    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);

    // Create color bitmap
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* colorBits = NULL;
    HBITMAP colorBmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &colorBits, NULL, 0);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, colorBmp);

    // Fill transparent
    RECT rc = {0, 0, size, size};
    HBRUSH blackBrush = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(memDC, &rc, blackBrush);
    DeleteObject(blackBrush);

    // Draw red circle
    HBRUSH redBrush = CreateSolidBrush(RGB(40, 100, 220));
    HPEN noPen = CreatePen(PS_NULL, 0, 0);
    SelectObject(memDC, redBrush);
    SelectObject(memDC, noPen);
    Ellipse(memDC, 0, 0, size, size);
    DeleteObject(redBrush);
    DeleteObject(noPen);

    // Draw "RT" text
    int fontSize = -(size * 48 / 100);
    HFONT font = CreateFontA(fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    SelectObject(memDC, font);
    SetTextColor(memDC, RGB(255, 255, 255));
    SetBkMode(memDC, TRANSPARENT);
    DrawTextA(memDC, "RT", 2, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DeleteObject(font);

    // Set alpha channel — pixels inside circle get alpha=255, outside stay 0
    DWORD* pixels = (DWORD*)colorBits;
    int cx = size / 2, cy = size / 2;
    float radius = size / 2.0f;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float dx = (float)(x - cx + 0.5f);
            float dy = (float)(y - cy + 0.5f);
            float dist = sqrtf(dx * dx + dy * dy);
            int idx = y * size + x;
            if (dist <= radius) {
                // Set alpha to 255 (premultiplied — keep RGB as-is since GDI already wrote them)
                pixels[idx] |= 0xFF000000;
            } else {
                pixels[idx] = 0x00000000;
            }
        }
    }

    SelectObject(memDC, oldBmp);

    // Create mask bitmap (all black = all opaque with the alpha channel)
    HBITMAP maskBmp = CreateBitmap(size, size, 1, 1, NULL);
    HDC maskDC = CreateCompatibleDC(screenDC);
    HBITMAP oldMask = (HBITMAP)SelectObject(maskDC, maskBmp);
    RECT maskRc = {0, 0, size, size};
    FillRect(maskDC, &maskRc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SelectObject(maskDC, oldMask);
    DeleteDC(maskDC);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = maskBmp;
    ii.hbmColor = colorBmp;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(colorBmp);
    DeleteObject(maskBmp);
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);

    return icon;
}

// CPU benchmark thread: tight math loop (single-core)
static DWORD WINAPI BenchmarkCPUThread(LPVOID) {
    volatile double x = 1.0;
    LONGLONG ops = 0;
    while (true) {
        x = sin(x) * cos(x) + sqrt(x + 1.0);
        ops++;
        if ((ops & 0xFFFF) == 0) {
            g_benchOps = ops;
            if (g_benchCancel) return 0;
            if (GetTickCount() - g_benchStartTick >= BENCH_DURATION_MS) break;
        }
    }
    g_benchOps = ops;
    g_benchDone = true;
    return 0;
}

// CPU multi-core benchmark thread: each thread writes to its own padded counter
static DWORD WINAPI BenchmarkMulticoreThread(LPVOID param) {
    int idx = (int)(intptr_t)param;
    volatile double x = 1.0 + idx;
    LONGLONG ops = 0;
    while (true) {
        x = sin(x) * cos(x) + sqrt(x + 1.0);
        ops++;
        if ((ops & 0xFFFF) == 0) {
            g_threadOps[idx].ops = ops;
            if (g_benchCancel) return 0;
            if (GetTickCount() - g_benchStartTick >= BENCH_DURATION_MS) break;
        }
    }
    g_threadOps[idx].ops = ops;
    return 0;
}

// Multi-core coordinator thread: waits for all worker threads, then sums
static DWORD WINAPI BenchmarkMulticoreCoordinator(LPVOID) {
    WaitForMultipleObjects((DWORD)g_benchThreadCount, g_benchThreads, TRUE, INFINITE);
    LONGLONG total = 0;
    for (int i = 0; i < g_benchThreadCount; i++) {
        total += g_threadOps[i].ops;
        CloseHandle(g_benchThreads[i]);
        g_benchThreads[i] = NULL;
    }
    g_benchOps = total;
    g_benchDone = true;
    return 0;
}

// GPU benchmark thread: D3D11 compute shader
static DWORD WINAPI BenchmarkGPUThread(LPVOID) {
    // HLSL compute shader — 512 iterations of sin*cos+sqrt per thread
    static const char shaderSrc[] =
        "RWStructuredBuffer<float> output : register(u0);\n"
        "[numthreads(256,1,1)]\n"
        "void CSMain(uint3 id : SV_DispatchThreadID) {\n"
        "    float x = (float)id.x * 0.001f;\n"
        "    float acc = 0.0f;\n"
        "    [loop] for (int i = 0; i < 512; i++) {\n"
        "        acc += sin(x) * cos(x) + sqrt(abs(x) + 1.0f);\n"
        "        x += 0.01f;\n"
        "    }\n"
        "    output[id.x] = acc;\n"
        "}\n";

    const UINT NUM_GROUPS = 256;
    const UINT THREADS_PER_GROUP = 256;
    const UINT TOTAL_THREADS = NUM_GROUPS * THREADS_PER_GROUP;
    const UINT OPS_PER_THREAD = 512;
    const int BATCHES_PER_DISPATCH = 8;
    const LONGLONG OPS_PER_DISPATCH = (LONGLONG)BATCHES_PER_DISPATCH * NUM_GROUPS * THREADS_PER_GROUP * OPS_PER_THREAD;

    ID3D11Device* device = NULL;
    ID3D11DeviceContext* ctx = NULL;
    ID3D11ComputeShader* cs = NULL;
    ID3D11Buffer* buf = NULL;
    ID3D11UnorderedAccessView* uav = NULL;
    ID3D11Query* query = NULL;
    ID3DBlob* blob = NULL;
    ID3DBlob* errBlob = NULL;

    // Create D3D11 device (hardware GPU)
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
        NULL, 0, D3D11_SDK_VERSION, &device, &fl, &ctx);
    if (FAILED(hr)) goto fail;

    // Compile compute shader
    hr = D3DCompile(shaderSrc, sizeof(shaderSrc), "gpu_bench", NULL, NULL,
        "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errBlob);
    if (errBlob) errBlob->Release();
    if (FAILED(hr)) goto fail;

    hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &cs);
    blob->Release();
    blob = NULL;
    if (FAILED(hr)) goto fail;

    // Create structured buffer + UAV
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = TOTAL_THREADS * sizeof(float);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = sizeof(float);
        hr = device->CreateBuffer(&bd, NULL, &buf);
        if (FAILED(hr)) goto fail;

        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = TOTAL_THREADS;
        ud.Format = DXGI_FORMAT_UNKNOWN;
        hr = device->CreateUnorderedAccessView(buf, &ud, &uav);
        if (FAILED(hr)) goto fail;
    }

    // Create event query for GPU sync
    {
        D3D11_QUERY_DESC qd = {};
        qd.Query = D3D11_QUERY_EVENT;
        hr = device->CreateQuery(&qd, &query);
        if (FAILED(hr)) goto fail;
    }

    // Benchmark loop
    {
        ctx->CSSetShader(cs, NULL, 0);
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, NULL);

        LONGLONG ops = 0;
        while (true) {
            // Dispatch multiple batches before syncing
            for (int b = 0; b < BATCHES_PER_DISPATCH; b++)
                ctx->Dispatch(NUM_GROUPS, 1, 1);

            // Wait for GPU to finish
            ctx->End(query);
            BOOL queryData = FALSE;
            while (ctx->GetData(query, &queryData, sizeof(queryData), 0) == S_FALSE) {
                if (g_benchCancel) { g_benchOps = 0; goto cleanup; }
                Sleep(0);
            }

            ops += OPS_PER_DISPATCH;
            g_benchOps = ops;

            if (g_benchCancel) { g_benchOps = 0; goto cleanup; }
            if (GetTickCount() - g_benchStartTick >= BENCH_DURATION_MS) break;
        }
        g_benchOps = ops;
    }
    goto cleanup;

fail:
    g_benchOps = 0;

cleanup:
    if (uav) uav->Release();
    if (buf) buf->Release();
    if (cs) cs->Release();
    if (query) query->Release();
    if (ctx) ctx->Release();
    if (device) device->Release();

    g_benchDone = true;
    return 0;
}

// Start a specific benchmark (0=CPU, 1=GPU, 2=Multicore)
static void StartBenchmarkType(int type) {
    g_lastBenchType = type;
    g_lastBenchScore = 0.0;
    g_benchOps = 0;
    g_benchDone = false;
    g_benchCancel = false;
    g_benchStartTick = GetTickCount();

    if (type == 0) {
        g_state = STATE_BENCHMARK_CPU;
        g_benchThread = CreateThread(NULL, 0, BenchmarkCPUThread, NULL, 0, NULL);
    } else if (type == 1) {
        g_state = STATE_BENCHMARK_GPU;
        g_benchThread = CreateThread(NULL, 0, BenchmarkGPUThread, NULL, 0, NULL);
    } else {
        g_state = STATE_BENCHMARK_MULTICORE;
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        g_benchThreadCount = (int)si.dwNumberOfProcessors;
        if (g_benchThreadCount > MAX_BENCH_THREADS) g_benchThreadCount = MAX_BENCH_THREADS;
        for (int i = 0; i < g_benchThreadCount; i++) {
            g_threadOps[i].ops = 0;
            g_benchThreads[i] = CreateThread(NULL, 0, BenchmarkMulticoreThread, (LPVOID)(intptr_t)i, 0, NULL);
        }
        // Coordinator thread waits for all workers and sets g_benchDone
        g_benchThread = CreateThread(NULL, 0, BenchmarkMulticoreCoordinator, NULL, 0, NULL);
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// Cancel a running benchmark and return to benchmark menu
static void CancelBenchmark() {
    g_benchCancel = true;
    if (g_benchThread) {
        WaitForSingleObject(g_benchThread, 2000);
        CloseHandle(g_benchThread);
        g_benchThread = NULL;
    }
    // Clean up any multicore worker handles that coordinator didn't close
    for (int i = 0; i < g_benchThreadCount; i++) {
        if (g_benchThreads[i]) {
            WaitForSingleObject(g_benchThreads[i], 500);
            CloseHandle(g_benchThreads[i]);
            g_benchThreads[i] = NULL;
        }
    }
    g_benchThreadCount = 0;
    g_benchCancel = false;
    g_benchDone = false;
    g_benchOps = 0;
    g_state = STATE_BENCHMARK_MENU;
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// Paint the window
static void OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    int cw = clientRect.right;
    int ch = clientRect.bottom;

    // Create back buffer for flicker-free drawing
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBitmap = CreateCompatibleBitmap(hdc, cw, ch);
    HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

    // Reset buttons for this frame
    ResetButtons();

    // Determine background color
    COLORREF bgColor;
    switch (g_state) {
        case STATE_START:
        case STATE_MENU:
        case STATE_KEYBINDS:
        case STATE_ABOUT:
        case STATE_BENCHMARK_MENU:
        case STATE_BENCHMARK_CPU:
        case STATE_BENCHMARK_GPU:
        case STATE_BENCHMARK_MULTICORE:
        case STATE_BENCHMARK_RESULT:
            bgColor = COLOR_DARK_BG;
            break;
        case STATE_WAITING:
            bgColor = COLOR_GREEN;
            break;
        case STATE_READY:
            bgColor = COLOR_RED;
            break;
        case STATE_RESULT:
            bgColor = COLOR_GREEN;
            break;
        case STATE_TOO_EARLY:
            bgColor = COLOR_YELLOW;
            break;
        default:
            bgColor = COLOR_GREEN;
    }

    // Fill background
    HBRUSH bgBrush = CreateSolidBrush(bgColor);
    FillRect(memDC, &clientRect, bgBrush);
    DeleteObject(bgBrush);

    // Create fonts
    HFONT titleFont = CreateFontA(56, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT largeFont = CreateFontA(48, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT mediumFont = CreateFontA(32, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT smallFont = CreateFontA(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT btnFont = CreateFontA(28, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");

    int centerX = cw / 2;

    // Draw based on state
    switch (g_state) {
        case STATE_MENU:
        {
            // Title
            DrawCenteredText(memDC, "ReactionTime", ch / 5 - 100, titleFont, COLOR_ACCENT);

            // Menu buttons
            int btnW = 280, btnH = 56, gap = 20;
            int startY = ch / 3 + 20 - 100;
            DrawButton(memDC, centerX, startY, btnW, btnH, "BENCHMARK", BTN_BENCHMARK, btnFont);
            DrawButton(memDC, centerX, startY + btnH + gap, btnW, btnH, "KEYBINDS", BTN_KEYBINDS, btnFont);
            DrawButton(memDC, centerX, startY + 2 * (btnH + gap), btnW, btnH, "ABOUT", BTN_ABOUT, btnFont);
            DrawButton(memDC, centerX, startY + 3 * (btnH + gap), btnW, btnH, "QUIT", BTN_QUIT, btnFont);
            DrawButton(memDC, centerX, startY + 4 * (btnH + gap), btnW, btnH, "CLOSE", BTN_CLOSE, btnFont);

            // Hint
            DrawCenteredText(memDC, "ESC = Return  |  Arrows/D-pad = Navigate  |  Enter/Gamepad = Select", ch - 60, smallFont, RGB(120, 120, 130));
        }
        break;

        case STATE_KEYBINDS:
        {
            DrawCenteredText(memDC, "Keybinds", ch / 6, titleFont, COLOR_ACCENT);

            int startY = ch / 3;
            int btnW = 400, btnH = 56, gap = 24;

            // Reset binding
            char resetLabel[128];
            if (g_rebindingAction == 0) {
                snprintf(resetLabel, sizeof(resetLabel), "Reset Scores:  [ press any input... ]");
            } else {
                char bindBuf[32];
                const char* name = GetBindingDisplayName(g_bindReset, bindBuf, sizeof(bindBuf));
                snprintf(resetLabel, sizeof(resetLabel), "Reset Scores:  [ %s ]", name);
            }
            DrawButton(memDC, centerX, startY, btnW, btnH, resetLabel, BTN_REBIND_RESET, btnFont);

            // Click binding
            char clickLabel[128];
            if (g_rebindingAction == 1) {
                snprintf(clickLabel, sizeof(clickLabel), "Game Click:  [ press any input... ]");
            } else {
                char bindBuf[32];
                const char* name = GetBindingDisplayName(g_bindClick, bindBuf, sizeof(bindBuf));
                snprintf(clickLabel, sizeof(clickLabel), "Game Click:  [ %s ]", name);
            }
            DrawButton(memDC, centerX, startY + btnH + gap, btnW, btnH, clickLabel, BTN_REBIND_CLICK, btnFont);

            // Back button
            DrawButton(memDC, centerX, startY + 2 * (btnH + gap) + 20, 200, btnH, "BACK", BTN_BACK, btnFont);

            // Hint
            if (g_rebindingAction >= 0) {
                DrawCenteredText(memDC, "ESC to cancel", ch - 60, smallFont, RGB(120, 120, 130));
            } else {
                DrawCenteredText(memDC, "Click or press Enter to change a binding", ch - 60, smallFont, RGB(120, 120, 130));
            }
        }
        break;

        case STATE_ABOUT:
        {
            int y = ch / 4;
            DrawCenteredText(memDC, "ReactionTime", y, titleFont, COLOR_ACCENT);
            DrawCenteredText(memDC, "Made by Thomas Wollbekk", y + 90, mediumFont, COLOR_WHITE);
            DrawCenteredText(memDC, "MELD LABS", y + 135, mediumFont, RGB(180, 180, 190));
            DrawCenteredText(memDC, "(Oslo, Norway)", y + 180, smallFont, RGB(130, 130, 140));

            // Email link
            {
                const char* email = "thomas@wollbekk.com";
                HFONT linkFont = CreateFontA(24, 0, 0, 0, FW_NORMAL, FALSE, TRUE, FALSE,
                    ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
                COLORREF linkColor = (g_hoveredButton == BTN_EMAIL) ? RGB(130, 180, 255) : RGB(100, 150, 230);
                SelectObject(memDC, linkFont);
                SIZE emailSize;
                GetTextExtentPoint32A(memDC, email, (int)strlen(email), &emailSize);
                int ex = centerX - emailSize.cx / 2;
                int ey = y + 220;

                // Register as clickable
                if (g_buttonCount < 16) {
                    g_buttons[g_buttonCount].rect = { ex, ey, ex + emailSize.cx, ey + emailSize.cy };
                    g_buttons[g_buttonCount].id = BTN_EMAIL;
                    strncpy(g_buttons[g_buttonCount].text, email, 63);
                    g_buttons[g_buttonCount].active = true;
                    g_buttonCount++;
                }

                SetTextColor(memDC, linkColor);
                SetBkMode(memDC, TRANSPARENT);
                TextOutA(memDC, ex, ey, email, (int)strlen(email));
                DeleteObject(linkFont);

                // Copy-to-clipboard icon button (small square right of email)
                int iconSize = emailSize.cy;
                int iconX = ex + emailSize.cx + 8;
                int iconY = ey;
                RECT copyRect = { iconX, iconY, iconX + iconSize, iconY + iconSize };

                if (g_buttonCount < 16) {
                    g_buttons[g_buttonCount].rect = copyRect;
                    g_buttons[g_buttonCount].id = BTN_COPY_EMAIL;
                    strncpy(g_buttons[g_buttonCount].text, "copy", 63);
                    g_buttons[g_buttonCount].active = true;
                    g_buttonCount++;
                }

                bool copyHovered = (g_hoveredButton == BTN_COPY_EMAIL);
                COLORREF copyBg = copyHovered ? RGB(70, 70, 85) : RGB(50, 50, 60);
                HBRUSH copyBrush = CreateSolidBrush(copyBg);
                HPEN copyPen = CreatePen(PS_SOLID, 1, copyHovered ? COLOR_ACCENT : RGB(80, 80, 95));
                SelectObject(memDC, copyBrush);
                SelectObject(memDC, copyPen);
                RoundRect(memDC, copyRect.left, copyRect.top, copyRect.right, copyRect.bottom, 6, 6);
                DeleteObject(copyBrush);
                DeleteObject(copyPen);

                // Draw clipboard icon (two overlapping rectangles)
                int pad = iconSize / 5;
                int rw = iconSize / 3;
                int rh = iconSize * 2 / 5;
                HPEN iconPen = CreatePen(PS_SOLID, 1, copyHovered ? COLOR_WHITE : RGB(180, 180, 195));
                HBRUSH hollowBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
                SelectObject(memDC, iconPen);
                SelectObject(memDC, hollowBrush);
                // Back rectangle
                Rectangle(memDC, iconX + pad + 3, iconY + pad, iconX + pad + 3 + rw, iconY + pad + rh);
                // Front rectangle
                Rectangle(memDC, iconX + pad, iconY + pad + 3, iconX + pad + rw, iconY + pad + 3 + rh);
                DeleteObject(iconPen);
            }

            DrawButton(memDC, centerX, y + 280, 200, 56, "BACK", BTN_BACK, btnFont);
        }
        break;

        case STATE_BENCHMARK_MENU:
        {
            DrawCenteredText(memDC, "Benchmark", ch / 5 - 100, titleFont, COLOR_ACCENT);

            int btnW = 280, btnH = 56, gap = 20;
            int startY = ch / 3 + 20 - 100;
            DrawButton(memDC, centerX, startY, btnW, btnH, "CPU", BTN_BENCH_CPU, btnFont);
            DrawButton(memDC, centerX, startY + btnH + gap, btnW, btnH, "CPU MULTICORE", BTN_BENCH_MULTICORE, btnFont);
            DrawButton(memDC, centerX, startY + 2 * (btnH + gap), btnW, btnH, "GPU", BTN_BENCH_GPU, btnFont);
            DrawButton(memDC, centerX, startY + 3 * (btnH + gap), btnW, btnH, "BACK", BTN_BACK, btnFont);

            DrawCenteredText(memDC, "Each benchmark runs for 10 seconds", ch - 60, smallFont, RGB(120, 120, 130));
        }
        break;

        case STATE_BENCHMARK_CPU:
        case STATE_BENCHMARK_GPU:
        case STATE_BENCHMARK_MULTICORE:
        {
            const char* title = "Testing CPU...";
            if (g_state == STATE_BENCHMARK_GPU) title = "Testing GPU...";
            else if (g_state == STATE_BENCHMARK_MULTICORE) title = "Testing CPU (all cores)...";
            DrawCenteredText(memDC, title, ch / 4, titleFont, COLOR_ACCENT);

            // Progress bar
            DWORD elapsed = GetTickCount() - g_benchStartTick;
            float progress = (float)elapsed / (float)BENCH_DURATION_MS;
            if (progress > 1.0f) progress = 1.0f;
            int barW = 400, barH = 30;
            int barX = centerX - barW / 2;
            int barY = ch / 2 - barH / 2;
            // Bar background
            HBRUSH barBgBrush = CreateSolidBrush(RGB(50, 50, 60));
            RECT barBgRect = { barX, barY, barX + barW, barY + barH };
            FillRect(memDC, &barBgRect, barBgBrush);
            DeleteObject(barBgBrush);
            // Bar fill
            int fillW = (int)(barW * progress);
            if (fillW > 0) {
                HBRUSH barFillBrush = CreateSolidBrush(COLOR_ACCENT);
                RECT barFillRect = { barX, barY, barX + fillW, barY + barH };
                FillRect(memDC, &barFillRect, barFillBrush);
                DeleteObject(barFillBrush);
            }
            // Bar border
            HPEN barPen = CreatePen(PS_SOLID, 1, RGB(80, 80, 95));
            HBRUSH nullBr = (HBRUSH)GetStockObject(NULL_BRUSH);
            SelectObject(memDC, barPen);
            SelectObject(memDC, nullBr);
            Rectangle(memDC, barX, barY, barX + barW, barY + barH);
            DeleteObject(barPen);

            // Animated spinner: 8 dots, one highlighted
            {
                int dotCount = 8;
                int dotSize = 10;
                int dotGap = 18;
                int totalW = dotCount * dotSize + (dotCount - 1) * (dotGap - dotSize);
                int dotStartX = centerX - totalW / 2;
                int dotY = barY + barH + 30;
                int activeIdx = (int)(GetTickCount() / 150) % dotCount;
                for (int i = 0; i < dotCount; i++) {
                    COLORREF dotColor = (i == activeIdx) ? COLOR_ACCENT : RGB(70, 70, 80);
                    HBRUSH dotBrush = CreateSolidBrush(dotColor);
                    HPEN dotPen = CreatePen(PS_NULL, 0, 0);
                    SelectObject(memDC, dotBrush);
                    SelectObject(memDC, dotPen);
                    int dx = dotStartX + i * dotGap;
                    Ellipse(memDC, dx, dotY, dx + dotSize, dotY + dotSize);
                    DeleteObject(dotBrush);
                    DeleteObject(dotPen);
                }
            }

            // Stats
            char statsBuf[128];
            DWORD secs = elapsed / 1000;
            snprintf(statsBuf, sizeof(statsBuf), "%d / %d seconds", secs > 10 ? 10 : secs, BENCH_DURATION_MS / 1000);
            DrawCenteredText(memDC, statsBuf, ch / 2 + 80, mediumFont, COLOR_WHITE);

            // Show live ops count (for multicore, sum all thread counters)
            LONGLONG ops = g_benchOps;
            if (g_state == STATE_BENCHMARK_MULTICORE) {
                ops = 0;
                for (int i = 0; i < g_benchThreadCount; i++)
                    ops += g_threadOps[i].ops;
            }
            char opsBuf[128];
            if (ops > 1000000) {
                snprintf(opsBuf, sizeof(opsBuf), "%.1f M operations", (double)ops / 1000000.0);
            } else {
                snprintf(opsBuf, sizeof(opsBuf), "%lld operations", ops);
            }
            DrawCenteredText(memDC, opsBuf, ch / 2 + 120, smallFont, RGB(180, 180, 190));

            // Show core count for multicore
            if (g_state == STATE_BENCHMARK_MULTICORE) {
                char coresBuf[64];
                snprintf(coresBuf, sizeof(coresBuf), "%d threads", g_benchThreadCount);
                DrawCenteredText(memDC, coresBuf, ch / 2 + 155, smallFont, RGB(150, 150, 160));
            }

            // Hint
            DrawCenteredText(memDC, "ESC = Cancel", ch - 60, smallFont, RGB(120, 120, 130));
        }
        break;

        case STATE_BENCHMARK_RESULT:
        {
            const char* label = "CPU";
            if (g_lastBenchType == 1) label = "GPU";
            else if (g_lastBenchType == 2) label = "CPU Multicore";

            DrawCenteredText(memDC, "Benchmark Result", ch / 4, titleFont, COLOR_ACCENT);

            char scoreBuf[128];
            if (g_lastBenchScore >= 1.0) {
                snprintf(scoreBuf, sizeof(scoreBuf), "%s:  %.2f Mops/s", label, g_lastBenchScore);
            } else {
                snprintf(scoreBuf, sizeof(scoreBuf), "%s:  %.2f Kops/s", label, g_lastBenchScore * 1000.0);
            }
            DrawCenteredText(memDC, scoreBuf, ch / 2 - 20, mediumFont, COLOR_WHITE);

            DrawButton(memDC, centerX, ch / 2 + 60, 200, 56, "BACK", BTN_BACK, btnFont);

            // Draw benchmark history (top-left, ~35% opacity like version text)
            if (g_benchHistoryCount > 0) {
                SelectObject(memDC, smallFont);
                SetBkMode(memDC, TRANSPARENT);

                // Measure line height
                SIZE lineSize;
                GetTextExtentPoint32A(memDC, "X", 1, &lineSize);
                int lineH = lineSize.cy + 2;
                int histX = 12, histY = 10;

                // Measure max width for the temp bitmap
                int maxW = 0;
                char histLines[20][64];
                for (int i = 0; i < g_benchHistoryCount; i++) {
                    if (g_benchHistory[i].score >= 1.0)
                        snprintf(histLines[i], 64, "%s  %.2f Mops/s", g_benchHistory[i].date, g_benchHistory[i].score);
                    else
                        snprintf(histLines[i], 64, "%s  %.2f Kops/s", g_benchHistory[i].date, g_benchHistory[i].score * 1000.0);
                    SIZE s;
                    GetTextExtentPoint32A(memDC, histLines[i], (int)strlen(histLines[i]), &s);
                    if (s.cx > maxW) maxW = s.cx;
                }

                int totalW = maxW + 16;
                int totalH = lineH * g_benchHistoryCount + 8;

                HDC hDC = CreateCompatibleDC(memDC);
                BITMAPINFO bi = {};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = totalW;
                bi.bmiHeader.biHeight = -totalH;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                void* hBits = NULL;
                HBITMAP hBmp = CreateDIBSection(hDC, &bi, DIB_RGB_COLORS, &hBits, NULL, 0);
                HBITMAP hOld = (HBITMAP)SelectObject(hDC, hBmp);
                BitBlt(hDC, 0, 0, totalW, totalH, memDC, histX, histY, SRCCOPY);

                SelectObject(hDC, smallFont);
                SetTextColor(hDC, RGB(255, 255, 255));
                SetBkMode(hDC, TRANSPARENT);
                for (int i = 0; i < g_benchHistoryCount; i++) {
                    TextOutA(hDC, 8, 4 + i * lineH, histLines[i], (int)strlen(histLines[i]));
                }

                BLENDFUNCTION hBf = {};
                hBf.BlendOp = AC_SRC_OVER;
                hBf.SourceConstantAlpha = 90;
                AlphaBlend(memDC, histX, histY, totalW, totalH, hDC, 0, 0, totalW, totalH, hBf);
                SelectObject(hDC, hOld);
                DeleteObject(hBmp);
                DeleteDC(hDC);
            }
        }
        break;

        default:
        {
            // Game states — draw header with dynamic keybind display
            char headerBuf[256];
            char bindBuf[32];
            const char* resetName = GetBindingDisplayName(g_bindReset, bindBuf, sizeof(bindBuf));
            snprintf(headerBuf, sizeof(headerBuf), "ESC = Menu | F11 = Fullscreen | %s = Reset Scores", resetName);
            COLORREF instructionColor = (g_state == STATE_TOO_EARLY) ? COLOR_BLACK : COLOR_WHITE;
            DrawCenteredText(memDC, headerBuf, 20, smallFont, instructionColor);

            int contentY = ch / 3;
            char buffer[256];

            switch (g_state) {
                case STATE_START:
                {
                    DrawCenteredText(memDC, "Reaction Time Tester", contentY, largeFont, COLOR_WHITE);
                    char clickBuf[64];
                    if (g_bindClick.type == BIND_MOUSE) {
                        snprintf(clickBuf, sizeof(clickBuf), "Click to Start");
                    } else {
                        char nb[32];
                        snprintf(clickBuf, sizeof(clickBuf), "Press %s to Start", GetBindingDisplayName(g_bindClick, nb, sizeof(nb)));
                    }
                    DrawCenteredText(memDC, clickBuf, contentY + 80, mediumFont, COLOR_GREEN);
                    DrawCenteredText(memDC, "When the screen turns GREEN, wait...", contentY + 140, smallFont, COLOR_WHITE);
                    DrawCenteredText(memDC, "When it turns RED, react as fast as you can!", contentY + 170, smallFont, COLOR_WHITE);
                }
                break;

                case STATE_WAITING:
                {
                    DrawCenteredText(memDC, "Wait for RED...", contentY, largeFont, COLOR_WHITE);
                    char waitBuf[64];
                    if (g_bindClick.type == BIND_MOUSE) {
                        snprintf(waitBuf, sizeof(waitBuf), "Click when the screen turns RED");
                    } else {
                        char nb[32];
                        snprintf(waitBuf, sizeof(waitBuf), "Press %s when the screen turns RED", GetBindingDisplayName(g_bindClick, nb, sizeof(nb)));
                    }
                    DrawCenteredText(memDC, waitBuf, contentY + 70, mediumFont, COLOR_WHITE);
                }
                break;

                case STATE_READY:
                {
                    char readyBuf[64];
                    if (g_bindClick.type == BIND_MOUSE) {
                        snprintf(readyBuf, sizeof(readyBuf), "CLICK NOW!");
                    } else {
                        char nb[32];
                        snprintf(readyBuf, sizeof(readyBuf), "PRESS %s NOW!", GetBindingDisplayName(g_bindClick, nb, sizeof(nb)));
                    }
                    DrawCenteredText(memDC, readyBuf, contentY + 20, largeFont, COLOR_WHITE);
                }
                break;

                case STATE_RESULT:
                {
                    int resultY = 100;
                    snprintf(buffer, sizeof(buffer), "Reaction Time: %.1f ms", g_reactionTime);
                    DrawCenteredText(memDC, buffer, resultY, largeFont, COLOR_WHITE);
                    char retryBuf[64];
                    if (g_bindClick.type == BIND_MOUSE) {
                        snprintf(retryBuf, sizeof(retryBuf), "Click to try again");
                    } else {
                        char nb[32];
                        snprintf(retryBuf, sizeof(retryBuf), "Press %s to try again", GetBindingDisplayName(g_bindClick, nb, sizeof(nb)));
                    }
                    DrawCenteredText(memDC, retryBuf, resultY + 60, mediumFont, COLOR_WHITE);

                    if (g_scoreCount > 0) {
                        DrawCenteredText(memDC, "Last scores:", resultY + 130, mediumFont, COLOR_WHITE);
                        int y = resultY + 170;
                        for (int i = 0; i < g_scoreCount; i++) {
                            int idx = (g_scoreIndex - 1 - i + 5) % 5;
                            snprintf(buffer, sizeof(buffer), "%d. %.1f ms", i + 1, g_scores[idx]);
                            DrawCenteredText(memDC, buffer, y, smallFont, COLOR_WHITE);
                            y += 30;
                        }
                        snprintf(buffer, sizeof(buffer), "Average: %.1f ms", GetAverageScore());
                        DrawCenteredText(memDC, buffer, y + 10, mediumFont, COLOR_WHITE);
                    }
                }
                break;

                case STATE_TOO_EARLY:
                    DrawCenteredText(memDC, "TOO EARLY!", contentY, largeFont, COLOR_BLACK);
                    DrawCenteredText(memDC, "Wait for the red screen!", contentY + 70, mediumFont, COLOR_BLACK);
                    break;

                default:
                    break;
            }
        }
        break;
    }

    // Version text (top-right corner, subtle)
    {
        const char* version = "v0.5";
        SelectObject(memDC, smallFont);
        SetTextColor(memDC, RGB(255, 255, 255));
        SetBkMode(memDC, TRANSPARENT);
        SIZE vs;
        GetTextExtentPoint32A(memDC, version, (int)strlen(version), &vs);
        // Use AlphaBlend trick: draw to a tiny temp bitmap with alpha
        // Simpler approach: just use a dim color that blends with any background
        COLORREF versionColor = (g_state == STATE_TOO_EARLY) ? RGB(80, 70, 0) : RGB(255, 255, 255);
        SetTextColor(memDC, versionColor);
        // Draw with 30% opacity via a separate alpha-blended bitmap
        HDC tmpDC = CreateCompatibleDC(memDC);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = vs.cx + 16;
        bmi.bmiHeader.biHeight = -(vs.cy + 8);
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        void* bits = NULL;
        HBITMAP tmpBmp = CreateDIBSection(tmpDC, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        HBITMAP oldTmp = (HBITMAP)SelectObject(tmpDC, tmpBmp);
        // Copy background region so text blends correctly
        int vx = cw - vs.cx - 16, vy = 8;
        BitBlt(tmpDC, 0, 0, vs.cx + 16, vs.cy + 8, memDC, vx, vy, SRCCOPY);
        // Draw text onto temp surface
        SelectObject(tmpDC, smallFont);
        SetTextColor(tmpDC, versionColor);
        SetBkMode(tmpDC, TRANSPARENT);
        TextOutA(tmpDC, 8, 4, version, (int)strlen(version));
        // Alpha blend back at ~35% opacity
        BLENDFUNCTION bf = {};
        bf.BlendOp = AC_SRC_OVER;
        bf.SourceConstantAlpha = 90;
        AlphaBlend(memDC, vx, vy, vs.cx + 16, vs.cy + 8, tmpDC, 0, 0, vs.cx + 16, vs.cy + 8, bf);
        SelectObject(tmpDC, oldTmp);
        DeleteObject(tmpBmp);
        DeleteDC(tmpDC);
    }

    // Copy back buffer to screen
    BitBlt(hdc, 0, 0, cw, ch, memDC, 0, 0, SRCCOPY);

    // Cleanup
    SelectObject(memDC, oldBitmap);
    DeleteObject(memBitmap);
    DeleteDC(memDC);
    DeleteObject(titleFont);
    DeleteObject(largeFont);
    DeleteObject(mediumFont);
    DeleteObject(smallFont);
    DeleteObject(btnFont);

    EndPaint(hwnd, &ps);
}

// Handle a UI button click by id
static void OnButtonClick(int id) {
    g_selectedButton = -1;  // reset selection on any button activation
    switch (id) {
        case BTN_KEYBINDS:
            g_state = STATE_KEYBINDS;
            g_rebindingAction = -1;
            InvalidateRect(g_hwnd, NULL, FALSE);
            break;
        case BTN_ABOUT:
            g_state = STATE_ABOUT;
            InvalidateRect(g_hwnd, NULL, FALSE);
            break;
        case BTN_CLOSE:
            // Same as pressing ESC from the menu
            if (g_stateBeforeMenu == STATE_WAITING || g_stateBeforeMenu == STATE_READY) {
                g_state = STATE_START;
                g_timerStarted = false;
            } else {
                g_state = g_stateBeforeMenu;
            }
            InvalidateRect(g_hwnd, NULL, FALSE);
            break;
        case BTN_QUIT:
            PostQuitMessage(0);
            break;
        case BTN_BENCHMARK:
            g_state = STATE_BENCHMARK_MENU;
            g_selectedButton = -1;
            InvalidateRect(g_hwnd, NULL, FALSE);
            break;
        case BTN_BENCH_CPU:
            StartBenchmarkType(0);
            break;
        case BTN_BENCH_GPU:
            StartBenchmarkType(1);
            break;
        case BTN_BENCH_MULTICORE:
            StartBenchmarkType(2);
            break;
        case BTN_BACK:
            if (g_state == STATE_KEYBINDS || g_state == STATE_ABOUT) {
                g_state = STATE_MENU;
                g_rebindingAction = -1;
                InvalidateRect(g_hwnd, NULL, FALSE);
            } else if (g_state == STATE_BENCHMARK_MENU || g_state == STATE_BENCHMARK_RESULT) {
                g_state = (g_state == STATE_BENCHMARK_RESULT) ? STATE_BENCHMARK_MENU : STATE_MENU;
                g_selectedButton = -1;
                InvalidateRect(g_hwnd, NULL, FALSE);
            }
            break;
        case BTN_REBIND_RESET:
            g_rebindingAction = 0;
            QueryPerformanceCounter(&g_rebindStartTime);
            InvalidateRect(g_hwnd, NULL, FALSE);
            break;
        case BTN_REBIND_CLICK:
            g_rebindingAction = 1;
            QueryPerformanceCounter(&g_rebindStartTime);
            InvalidateRect(g_hwnd, NULL, FALSE);
            break;
        case BTN_EMAIL:
            ShellExecuteA(NULL, "open", "mailto:thomas@wollbekk.com", NULL, NULL, SW_SHOWNORMAL);
            break;
        case BTN_COPY_EMAIL:
            if (OpenClipboard(g_hwnd)) {
                EmptyClipboard();
                const char* email = "thomas@wollbekk.com";
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, strlen(email) + 1);
                if (hMem) {
                    char* ptr = (char*)GlobalLock(hMem);
                    memcpy(ptr, email, strlen(email) + 1);
                    GlobalUnlock(hMem);
                    SetClipboardData(CF_TEXT, hMem);
                }
                CloseClipboard();
            }
            break;
    }
}

// Detect which mouse button was pressed from raw flags, returns -1 if none
static int DetectRawMouseButton(USHORT flags) {
    if (flags & RI_MOUSE_LEFT_BUTTON_DOWN)   return 0;
    if (flags & RI_MOUSE_RIGHT_BUTTON_DOWN)  return 1;
    if (flags & RI_MOUSE_MIDDLE_BUTTON_DOWN) return 2;
    return -1;
}

// Handle raw mouse input
static void OnRawInput(HWND hwnd, HRAWINPUT hRawInput) {
    UINT size = 0;
    GetRawInputData(hRawInput, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER));

    if (size == 0) return;

    BYTE* buffer = (BYTE*)_alloca(size);
    if (GetRawInputData(hRawInput, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) != size) {
        return;
    }

    RAWINPUT* raw = (RAWINPUT*)buffer;

    if (raw->header.dwType != RIM_TYPEMOUSE) return;

    USHORT flags = raw->data.mouse.usButtonFlags;

    // Only process clicks if window is focused and mouse is inside
    if (GetForegroundWindow() != hwnd || !IsMouseInsideWindow(10)) {
        return;
    }

    int btn = DetectRawMouseButton(flags);
    if (btn < 0) return;  // No button-down event

    // If rebinding, capture mouse input (but left-click on UI buttons still navigates)
    if (g_rebindingAction >= 0) {
        if (btn == 0) {
            // Left-click: check if clicking a DIFFERENT rebind button or Back
            int btnId = HitTestButtons(g_mousePos.x, g_mousePos.y);
            if (btnId == BTN_REBIND_RESET && g_rebindingAction != 0) {
                g_rebindingAction = 0;
                QueryPerformanceCounter(&g_rebindStartTime);
                InvalidateRect(hwnd, NULL, FALSE);
                return;
            }
            if (btnId == BTN_REBIND_CLICK && g_rebindingAction != 1) {
                g_rebindingAction = 1;
                QueryPerformanceCounter(&g_rebindStartTime);
                InvalidateRect(hwnd, NULL, FALSE);
                return;
            }
            if (btnId == BTN_BACK) {
                g_rebindingAction = -1;
                OnButtonClick(BTN_BACK);
                return;
            }
        }
        CaptureRebind(BIND_MOUSE, btn);
        return;
    }

    // Block mouse during benchmarks (ESC key is the only way out)
    if (g_state == STATE_BENCHMARK_CPU || g_state == STATE_BENCHMARK_GPU || g_state == STATE_BENCHMARK_MULTICORE) return;

    // In menu/keybinds/about/benchmark screens — only left-click for UI navigation
    if (g_state == STATE_MENU || g_state == STATE_KEYBINDS || g_state == STATE_ABOUT
        || g_state == STATE_BENCHMARK_MENU || g_state == STATE_BENCHMARK_RESULT) {
        if (btn == 0) {
            int btnId = HitTestButtons(g_mousePos.x, g_mousePos.y);
            if (btnId > 0) {
                OnButtonClick(btnId);
            }
        }
        return;
    }

    // Game states — check both bindings against mouse input
    if (BindingMatches(g_bindReset, BIND_MOUSE, btn))
        HandleAction(0);
    if (BindingMatches(g_bindClick, BIND_MOUSE, btn))
        HandleAction(1);
}

// Toggle fullscreen mode
static bool g_fullscreen = false;
static WINDOWPLACEMENT g_wpPrev = { sizeof(g_wpPrev) };

static void ToggleFullscreen(HWND hwnd) {
    DWORD style = GetWindowLongW(hwnd, GWL_STYLE);

    if (!g_fullscreen) {
        MONITORINFO mi = { sizeof(mi) };
        if (GetWindowPlacement(hwnd, &g_wpPrev) &&
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongW(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_TOP,
                mi.rcMonitor.left, mi.rcMonitor.top,
                mi.rcMonitor.right - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top,
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        g_fullscreen = true;
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &g_wpPrev);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = false;
    }
}

// Window procedure
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            // Register for raw mouse input
            {
                RAWINPUTDEVICE rid;
                rid.usUsagePage = 0x01;
                rid.usUsage = 0x02;
                rid.dwFlags = RIDEV_INPUTSINK;
                rid.hwndTarget = hwnd;
                RegisterRawInputDevices(&rid, 1, sizeof(rid));
            }
            return 0;

        case WM_PAINT:
            OnPaint(hwnd);
            return 0;

        case WM_INPUT:
            OnRawInput(hwnd, (HRAWINPUT)lParam);
            return 0;

        case WM_MOUSEMOVE:
        {
            // Track mouse position in screen coords for hover
            POINT pt;
            pt.x = (short)LOWORD(lParam);
            pt.y = (short)HIWORD(lParam);
            ClientToScreen(hwnd, &pt);
            g_mousePos = pt;
            g_selectedButton = -1;  // mouse takes over highlight
            UpdateHoveredButton();
        }
        return 0;

        case WM_KEYDOWN:
            // If rebinding, capture keypress (any action)
            if (g_rebindingAction >= 0) {
                if (wParam == VK_ESCAPE) {
                    // Cancel rebinding (always immediate)
                    g_rebindingAction = -1;
                    InvalidateRect(hwnd, NULL, FALSE);
                } else if (wParam != VK_F11) {
                    // Debounce: ignore keyboard for 200ms after entering rebind mode
                    // to prevent spurious key events from stealing mouse/gamepad rebinds
                    double rebindElapsed = GetElapsedMs(g_rebindStartTime);
                    if (rebindElapsed >= 200.0) {
                        CaptureRebind(BIND_KEYBOARD, (int)wParam);
                    }
                }
                return 0;
            }

            if (wParam == VK_ESCAPE) {
                ToggleMenu();
            } else if (wParam == VK_F11) {
                ToggleFullscreen(hwnd);
            } else if (g_state == STATE_BENCHMARK_CPU || g_state == STATE_BENCHMARK_GPU || g_state == STATE_BENCHMARK_MULTICORE) {
                // Block all keys during benchmarks (only ESC/F11 above)
            } else if ((g_state == STATE_MENU || g_state == STATE_KEYBINDS || g_state == STATE_ABOUT
                        || g_state == STATE_BENCHMARK_MENU || g_state == STATE_BENCHMARK_RESULT) &&
                       (wParam == VK_UP || wParam == VK_DOWN || wParam == VK_RETURN)) {
                // Menu navigation with arrow keys and Enter
                if (wParam == VK_UP) {
                    NavigateMenu(-1);
                } else if (wParam == VK_DOWN) {
                    NavigateMenu(1);
                } else if (wParam == VK_RETURN) {
                    ActivateSelectedButton();
                }
            } else {
                // Check both bindings against keyboard input
                if (BindingMatches(g_bindReset, BIND_KEYBOARD, (int)wParam))
                    HandleAction(0);
                if (BindingMatches(g_bindClick, BIND_KEYBOARD, (int)wParam))
                    HandleAction(1);
            }
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    // Initialize performance counter
    QueryPerformanceFrequency(&g_perfFreq);

    // Load persistent keybinds
    InitConfigPath();
    LoadKeybinds();

    // Seed random number generator
    srand((unsigned int)time(NULL));

    // Create runtime icons
    HICON iconLarge = CreateAppIcon(48);
    HICON iconSmall = CreateAppIcon(16);

    // Register window class
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = iconLarge;
    wc.hIconSm = iconSmall;
    wc.lpszClassName = L"ReactionTimeClass";

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"Failed to register window class", L"Error", MB_ICONERROR);
        return 1;
    }

    // Create window with 16:9 client area
    DWORD dwStyle = WS_OVERLAPPEDWINDOW;
    RECT wr = { 0, 0, 960, 540 };
    AdjustWindowRect(&wr, dwStyle, FALSE);
    g_hwnd = CreateWindowExW(
        0,
        L"ReactionTimeClass",
        L"Reaction Time Tester",
        dwStyle,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, hInstance, NULL
    );

    if (!g_hwnd) {
        MessageBoxW(NULL, L"Failed to create window", L"Error", MB_ICONERROR);
        return 1;
    }

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    // Request high timer resolution for accurate timing
    timeBeginPeriod(1);

    // Message loop with timer check
    MSG msg;
    while (true) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                timeEndPeriod(1);
                if (iconLarge) DestroyIcon(iconLarge);
                if (iconSmall) DestroyIcon(iconSmall);
                return (int)msg.wParam;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // Check timer for state transitions
        if (g_state == STATE_WAITING && g_timerStarted) {
            double elapsed = GetElapsedMs(g_startTime);
            if (elapsed >= g_randomDelay) {
                g_state = STATE_READY;
                QueryPerformanceCounter(&g_flashTime);
                InvalidateRect(g_hwnd, NULL, FALSE);
            }
        }

        // Check for too early timeout (show message for 2 seconds)
        if (g_state == STATE_TOO_EARLY) {
            double elapsed = GetElapsedMs(g_tooEarlyTime);
            if (elapsed >= 2000) {
                StartWaiting();
            }
        }

        // Benchmark progress: continuous repaint + completion check
        if (g_state == STATE_BENCHMARK_CPU || g_state == STATE_BENCHMARK_GPU || g_state == STATE_BENCHMARK_MULTICORE) {
            InvalidateRect(g_hwnd, NULL, FALSE);
            if (g_benchDone) {
                double elapsed = (double)BENCH_DURATION_MS / 1000.0;
                double score = (double)g_benchOps / elapsed / 1000000.0;
                CloseHandle(g_benchThread);
                g_benchThread = NULL;
                g_lastBenchScore = score;
                SaveBenchResult(g_lastBenchType, score);
                LoadBenchHistory(g_lastBenchType);
                g_state = STATE_BENCHMARK_RESULT;
                g_selectedButton = -1;
                InvalidateRect(g_hwnd, NULL, FALSE);
            }
        }

        // Poll gamepad — try XInput first (Steam-wrapped, native Xbox), then joyGetPosEx (PS5/Switch direct)
        {
            DWORD tickNow = GetTickCount();

            // Scan for a controller if none found (every 1 second)
            if (!g_useXInput && g_joyId < 0 && (tickNow - g_joyScanTime) > 1000) {
                g_joyScanTime = tickNow;

                // Try XInput first (covers Steam Input and native Xbox controllers)
                XINPUT_STATE xstate;
                for (DWORD p = 0; p < 4; p++) {
                    if (XInputGetState(p, &xstate) == ERROR_SUCCESS) {
                        g_useXInput = true;
                        g_xinputPlayer = (int)p;
                        g_prevXInputButtons = XInputToJoyButtons(xstate.Gamepad.wButtons);
                        g_prevXInputPOVDir = XInputDpadToDirection(xstate.Gamepad.wButtons);
                        g_joyType = JOY_XBOX;
                        g_joyStartButton = 7;
                        InvalidateRect(g_hwnd, NULL, FALSE);
                        break;
                    }
                }

                // If no XInput, try joyGetPosEx (PS5/Switch direct USB, generic DirectInput)
                if (!g_useXInput) {
                    UINT numDevs = joyGetNumDevs();
                    for (UINT i = 0; i < numDevs && i < 16; i++) {
                        JOYINFOEX probe = {};
                        probe.dwSize = sizeof(JOYINFOEX);
                        probe.dwFlags = JOY_RETURNBUTTONS;
                        if (joyGetPosEx(i, &probe) == JOYERR_NOERROR) {
                            g_joyId = (int)i;
                            g_prevJoyButtons = probe.dwButtons;
                            g_prevJoyPOVDir = -1;
                            JOYCAPSA caps = {};
                            g_joyType = JOY_GENERIC;
                            g_joyStartButton = 9;
                            if (joyGetDevCapsA(i, &caps, sizeof(caps)) == JOYERR_NOERROR) {
                                if (strstr(caps.szPname, "Xbox") || strstr(caps.szPname, "xbox") ||
                                    strstr(caps.szPname, "XBOX") || strstr(caps.szPname, "X-Box")) {
                                    g_joyType = JOY_XBOX;
                                    g_joyStartButton = 7;
                                } else if (strstr(caps.szPname, "Pro Controller") ||
                                           strstr(caps.szPname, "Nintendo") || strstr(caps.szPname, "Joy-Con")) {
                                    g_joyType = JOY_SWITCH;
                                } else {
                                    g_joyType = JOY_PLAYSTATION;
                                }
                            }
                            InvalidateRect(g_hwnd, NULL, FALSE);
                            break;
                        }
                    }
                }
            }

            // --- XInput polling ---
            if (g_useXInput && g_xinputPlayer >= 0) {
                XINPUT_STATE xstate;
                if (XInputGetState((DWORD)g_xinputPlayer, &xstate) == ERROR_SUCCESS) {
                    DWORD buttons = XInputToJoyButtons(xstate.Gamepad.wButtons);
                    DWORD newButtons = buttons & ~g_prevXInputButtons;
                    int povDir = XInputDpadToDirection(xstate.Gamepad.wButtons);
                    bool povEdge = (povDir >= 0 && povDir != g_prevXInputPOVDir);

                    int pressed = -1;
                    if (newButtons) {
                        for (int i = 0; i < 32; i++) {
                            if (newButtons & (1u << i)) { pressed = i; break; }
                        }
                    }
                    if (pressed < 0 && povEdge) {
                        pressed = GAMEPAD_POV_UP + povDir;
                    }

                    // Start button toggles menu (like ESC)
                    if ((newButtons & (1u << 7)) && g_rebindingAction < 0) {
                        ToggleMenu();
                        newButtons &= ~(1u << 7);
                    }

                    if (pressed >= 0) {
                        if (g_state == STATE_BENCHMARK_CPU || g_state == STATE_BENCHMARK_GPU || g_state == STATE_BENCHMARK_MULTICORE) {
                            // Block gamepad during benchmarks (Start/ESC handled above)
                        } else if (g_rebindingAction >= 0) {
                            CaptureRebind(BIND_GAMEPAD, pressed);
                        } else if (g_state == STATE_MENU || g_state == STATE_KEYBINDS || g_state == STATE_ABOUT
                                   || g_state == STATE_BENCHMARK_MENU || g_state == STATE_BENCHMARK_RESULT) {
                            if (pressed == GAMEPAD_POV_UP) {
                                NavigateMenu(-1);
                            } else if (pressed == GAMEPAD_POV_DOWN) {
                                NavigateMenu(1);
                            } else if (!IsGamepadStartButton(pressed)) {
                                ActivateSelectedButton();
                            }
                        } else {
                            if (newButtons) {
                                for (int i = 0; i < 32; i++) {
                                    if (!(newButtons & (1u << i))) continue;
                                    if (BindingMatches(g_bindReset, BIND_GAMEPAD, i))
                                        HandleAction(0);
                                    if (BindingMatches(g_bindClick, BIND_GAMEPAD, i))
                                        HandleAction(1);
                                }
                            }
                            if (povEdge) {
                                int povCode = GAMEPAD_POV_UP + povDir;
                                if (BindingMatches(g_bindReset, BIND_GAMEPAD, povCode))
                                    HandleAction(0);
                                if (BindingMatches(g_bindClick, BIND_GAMEPAD, povCode))
                                    HandleAction(1);
                            }
                        }
                    }
                    g_prevXInputButtons = buttons;
                    g_prevXInputPOVDir = povDir;

                    // Thumbstick menu navigation
                    // XInput: positive Y = up, negative Y = down
                    if (g_state == STATE_MENU || g_state == STATE_KEYBINDS || g_state == STATE_ABOUT
                        || g_state == STATE_BENCHMARK_MENU || g_state == STATE_BENCHMARK_RESULT) {
                        const SHORT deadzone = 16384;
                        int stickDir = 0;
                        SHORT ly = xstate.Gamepad.sThumbLY;
                        SHORT ry = xstate.Gamepad.sThumbRY;
                        if (ly > deadzone || ry > deadzone) stickDir = -1;  // up
                        else if (ly < -deadzone || ry < -deadzone) stickDir = 1;  // down
                        if (stickDir != 0 && stickDir != g_prevXInputStickDir) {
                            NavigateMenu(stickDir);
                        }
                        g_prevXInputStickDir = stickDir;
                    } else {
                        g_prevXInputStickDir = 0;
                    }
                } else {
                    // XInput controller disconnected
                    g_useXInput = false;
                    g_xinputPlayer = -1;
                    g_prevXInputButtons = 0;
                    g_prevXInputPOVDir = -1;
                    g_prevXInputStickDir = 0;
                    g_joyStartButton = -1;
                    g_joyType = JOY_GENERIC;
                }
            }
            // --- joyGetPosEx polling (only when XInput is not active) ---
            else if (g_joyId >= 0) {
                JOYINFOEX joyInfo = {};
                joyInfo.dwSize = sizeof(JOYINFOEX);
                joyInfo.dwFlags = JOY_RETURNBUTTONS | JOY_RETURNPOV | JOY_RETURNY | JOY_RETURNR;
                MMRESULT joyResult = joyGetPosEx((UINT)g_joyId, &joyInfo);
                if (joyResult == JOYERR_NOERROR) {
                    DWORD buttons = joyInfo.dwButtons;
                    DWORD newButtons = buttons & ~g_prevJoyButtons;
                    int povDir = POVToDirection(joyInfo.dwPOV);
                    bool povEdge = (povDir >= 0 && povDir != g_prevJoyPOVDir);

                    int pressed = -1;
                    if (newButtons) {
                        for (int i = 0; i < 32; i++) {
                            if (newButtons & (1u << i)) { pressed = i; break; }
                        }
                    }
                    if (pressed < 0 && povEdge) {
                        pressed = GAMEPAD_POV_UP + povDir;
                    }

                    bool startPressed = (g_joyStartButton >= 0 && (newButtons & (1u << g_joyStartButton)) != 0);
                    if (startPressed && g_rebindingAction < 0) {
                        ToggleMenu();
                        newButtons &= ~(1u << g_joyStartButton);
                    }

                    if (pressed >= 0) {
                        if (g_state == STATE_BENCHMARK_CPU || g_state == STATE_BENCHMARK_GPU || g_state == STATE_BENCHMARK_MULTICORE) {
                            // Block gamepad during benchmarks (Start handled above)
                        } else if (g_rebindingAction >= 0) {
                            CaptureRebind(BIND_GAMEPAD, pressed);
                        } else if (g_state == STATE_MENU || g_state == STATE_KEYBINDS || g_state == STATE_ABOUT
                                   || g_state == STATE_BENCHMARK_MENU || g_state == STATE_BENCHMARK_RESULT) {
                            if (pressed == GAMEPAD_POV_UP) {
                                NavigateMenu(-1);
                            } else if (pressed == GAMEPAD_POV_DOWN) {
                                NavigateMenu(1);
                            } else if (!IsGamepadStartButton(pressed)) {
                                ActivateSelectedButton();
                            }
                        } else {
                            if (newButtons) {
                                for (int i = 0; i < 32; i++) {
                                    if (!(newButtons & (1u << i))) continue;
                                    if (BindingMatches(g_bindReset, BIND_GAMEPAD, i))
                                        HandleAction(0);
                                    if (BindingMatches(g_bindClick, BIND_GAMEPAD, i))
                                        HandleAction(1);
                                }
                            }
                            if (povEdge) {
                                int povCode = GAMEPAD_POV_UP + povDir;
                                if (BindingMatches(g_bindReset, BIND_GAMEPAD, povCode))
                                    HandleAction(0);
                                if (BindingMatches(g_bindClick, BIND_GAMEPAD, povCode))
                                    HandleAction(1);
                            }
                        }
                    }
                    g_prevJoyButtons = buttons;
                    g_prevJoyPOVDir = povDir;

                    // Thumbstick menu navigation (joyGetPosEx: 0-65535, center ~32768)
                    if (g_state == STATE_MENU || g_state == STATE_KEYBINDS || g_state == STATE_ABOUT
                        || g_state == STATE_BENCHMARK_MENU || g_state == STATE_BENCHMARK_RESULT) {
                        const DWORD deadzone = 16384;
                        const DWORD center = 32768;
                        int stickDir = 0;
                        if (joyInfo.dwYpos < center - deadzone || joyInfo.dwRpos < center - deadzone)
                            stickDir = -1;
                        else if (joyInfo.dwYpos > center + deadzone || joyInfo.dwRpos > center + deadzone)
                            stickDir = 1;
                        if (stickDir != 0 && stickDir != g_prevStickDir) {
                            NavigateMenu(stickDir);
                        }
                        g_prevStickDir = stickDir;
                    } else {
                        g_prevStickDir = 0;
                    }
                } else {
                    g_joyId = -1;
                    g_prevJoyButtons = 0;
                    g_prevJoyPOVDir = -1;
                    g_prevStickDir = 0;
                    g_joyStartButton = -1;
                    g_joyType = JOY_GENERIC;
                }
            }
        }

        // Small sleep to prevent 100% CPU usage while maintaining responsiveness
        Sleep(1);
    }
}
