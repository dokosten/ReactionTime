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

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "msimg32.lib")

// Game states
enum GameState {
    STATE_START,        // Start menu, waiting for user to begin
    STATE_WAITING,      // Green screen, waiting for random delay
    STATE_READY,        // Red screen, measuring reaction time
    STATE_RESULT,       // Showing result, waiting for click to restart
    STATE_TOO_EARLY,    // Clicked too early, showing message
    STATE_MENU,         // ESC menu overlay
    STATE_KEYBINDS,     // Keybinds configuration screen
    STATE_ABOUT         // About screen
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
    BTN_CLOSE
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

// Forward declaration
static void OnButtonClick(int id);

// Get ordered list of button IDs for the current menu state
static int GetMenuButtonIds(int* ids, int maxIds) {
    int count = 0;
    switch (g_state) {
        case STATE_MENU:
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
            DrawButton(memDC, centerX, startY, btnW, btnH, "KEYBINDS", BTN_KEYBINDS, btnFont);
            DrawButton(memDC, centerX, startY + btnH + gap, btnW, btnH, "ABOUT", BTN_ABOUT, btnFont);
            DrawButton(memDC, centerX, startY + 2 * (btnH + gap), btnW, btnH, "QUIT", BTN_QUIT, btnFont);
            DrawButton(memDC, centerX, startY + 3 * (btnH + gap), btnW, btnH, "CLOSE", BTN_CLOSE, btnFont);

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
        case BTN_BACK:
            if (g_state == STATE_KEYBINDS || g_state == STATE_ABOUT) {
                g_state = STATE_MENU;
                g_rebindingAction = -1;
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

    // In menu/keybinds/about — only left-click for UI navigation
    if (g_state == STATE_MENU || g_state == STATE_KEYBINDS || g_state == STATE_ABOUT) {
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
            } else if ((g_state == STATE_MENU || g_state == STATE_KEYBINDS || g_state == STATE_ABOUT) &&
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

    // Create window
    g_hwnd = CreateWindowExW(
        0,
        L"ReactionTimeClass",
        L"Reaction Time Tester",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        800, 600,
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

        // Poll gamepad via joyGetPosEx (works with PS5, Xbox, Switch Pro, etc.)
        {
            // Scan for a connected joystick if we don't have one (every 1 second)
            DWORD tickNow = GetTickCount();
            if (g_joyId < 0 && (tickNow - g_joyScanTime) > 1000) {
                g_joyScanTime = tickNow;
                UINT numDevs = joyGetNumDevs();
                for (UINT i = 0; i < numDevs && i < 16; i++) {
                    JOYINFOEX probe = {};
                    probe.dwSize = sizeof(JOYINFOEX);
                    probe.dwFlags = JOY_RETURNBUTTONS;
                    if (joyGetPosEx(i, &probe) == JOYERR_NOERROR) {
                        g_joyId = (int)i;
                        g_prevJoyButtons = probe.dwButtons;
                        g_prevJoyPOVDir = -1;
                        // Detect controller type for correct Start button mapping
                        // Xbox (DirectInput): button 7 = Start/Menu
                        // PlayStation/Switch (DirectInput): button 9 = Options/+
                        JOYCAPSA caps = {};
                        g_joyType = JOY_GENERIC;
                        g_joyStartButton = 9;  // default to PS/Switch mapping
                        if (joyGetDevCapsA(i, &caps, sizeof(caps)) == JOYERR_NOERROR) {
                            if (strstr(caps.szPname, "Xbox") || strstr(caps.szPname, "xbox") ||
                                strstr(caps.szPname, "XBOX") || strstr(caps.szPname, "X-Box")) {
                                g_joyType = JOY_XBOX;
                                g_joyStartButton = 7;
                            } else if (strstr(caps.szPname, "Pro Controller") ||
                                       strstr(caps.szPname, "Nintendo") || strstr(caps.szPname, "Joy-Con")) {
                                g_joyType = JOY_SWITCH;
                            } else {
                                // Default to PlayStation for DualShock/DualSense ("Wireless Controller" etc.)
                                g_joyType = JOY_PLAYSTATION;
                            }
                        }
                        break;
                    }
                }
                // Repaint so button names update with detected controller type
                if (g_joyId >= 0) InvalidateRect(g_hwnd, NULL, FALSE);
            }

            if (g_joyId >= 0) {
                JOYINFOEX joyInfo = {};
                joyInfo.dwSize = sizeof(JOYINFOEX);
                joyInfo.dwFlags = JOY_RETURNBUTTONS | JOY_RETURNPOV | JOY_RETURNY | JOY_RETURNR;
                MMRESULT joyResult = joyGetPosEx((UINT)g_joyId, &joyInfo);
                if (joyResult == JOYERR_NOERROR) {
                    DWORD buttons = joyInfo.dwButtons;
                    DWORD newButtons = buttons & ~g_prevJoyButtons;
                    int povDir = POVToDirection(joyInfo.dwPOV);
                    bool povEdge = (povDir >= 0 && povDir != g_prevJoyPOVDir);

                    // Find first new press (button or POV) for rebinding
                    int pressed = -1;
                    if (newButtons) {
                        for (int i = 0; i < 32; i++) {
                            if (newButtons & (1u << i)) { pressed = i; break; }
                        }
                    }
                    if (pressed < 0 && povEdge) {
                        pressed = GAMEPAD_POV_UP + povDir;  // 0x100 + 0..3
                    }

                    // Check for Start/Menu/Options button to toggle menu (like ESC)
                    bool startPressed = (g_joyStartButton >= 0 && (newButtons & (1u << g_joyStartButton)) != 0);
                    if (startPressed && g_rebindingAction < 0) {
                        ToggleMenu();
                        // Remove start from newButtons so it doesn't also trigger bindings
                        newButtons &= ~(1u << g_joyStartButton);
                    }

                    if (pressed >= 0) {
                        if (g_rebindingAction >= 0) {
                            CaptureRebind(BIND_GAMEPAD, pressed);
                        } else if (g_state == STATE_MENU || g_state == STATE_KEYBINDS || g_state == STATE_ABOUT) {
                            // Menu navigation: D-pad Up/Down to navigate, any other button to activate
                            if (pressed == GAMEPAD_POV_UP) {
                                NavigateMenu(-1);
                            } else if (pressed == GAMEPAD_POV_DOWN) {
                                NavigateMenu(1);
                            } else if (!IsGamepadStartButton(pressed)) {
                                ActivateSelectedButton();
                            }
                        } else {
                            // Check all new digital buttons
                            if (newButtons) {
                                for (int i = 0; i < 32; i++) {
                                    if (!(newButtons & (1u << i))) continue;
                                    if (BindingMatches(g_bindReset, BIND_GAMEPAD, i))
                                        HandleAction(0);
                                    if (BindingMatches(g_bindClick, BIND_GAMEPAD, i))
                                        HandleAction(1);
                                }
                            }
                            // Check POV hat (D-pad)
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

                    // Thumbstick menu navigation (left stick Y = dwYpos, right stick Y = dwRpos)
                    // joyGetPosEx axes range 0-65535, center ~32768
                    if (g_state == STATE_MENU || g_state == STATE_KEYBINDS || g_state == STATE_ABOUT) {
                        const DWORD deadzone = 16384;  // ~25% of half-range
                        const DWORD center = 32768;
                        int stickDir = 0;
                        // Left stick Y (dwYpos) or right stick Y (dwRpos)
                        if (joyInfo.dwYpos < center - deadzone || joyInfo.dwRpos < center - deadzone)
                            stickDir = -1;  // up
                        else if (joyInfo.dwYpos > center + deadzone || joyInfo.dwRpos > center + deadzone)
                            stickDir = 1;   // down
                        if (stickDir != 0 && stickDir != g_prevStickDir) {
                            NavigateMenu(stickDir);
                        }
                        g_prevStickDir = stickDir;
                    } else {
                        g_prevStickDir = 0;
                    }
                } else {
                    // Controller disconnected — rescan next cycle
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
