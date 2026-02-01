#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#pragma comment(lib, "winmm.lib")

// Game states
enum GameState {
    STATE_START,        // Start menu, waiting for user to begin
    STATE_WAITING,      // Green screen, waiting for random delay
    STATE_READY,        // Red screen, measuring reaction time
    STATE_RESULT,       // Showing result, waiting for click to restart
    STATE_TOO_EARLY     // Clicked too early, showing message
};

// Global variables
static HWND g_hwnd = NULL;
static GameState g_state = STATE_START;
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

// Colors
static const COLORREF COLOR_GREEN = RGB(0, 180, 0);
static const COLORREF COLOR_RED = RGB(220, 0, 0);
static const COLORREF COLOR_YELLOW = RGB(255, 200, 0);
static const COLORREF COLOR_WHITE = RGB(255, 255, 255);
static const COLORREF COLOR_BLACK = RGB(0, 0, 0);

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

// Check if mouse cursor is inside the window client area (with margin)
static bool IsMouseInsideWindow(int margin = 10) {
    POINT cursorPos;
    if (!GetCursorPos(&cursorPos)) return false;

    RECT clientRect;
    GetClientRect(g_hwnd, &clientRect);

    // Convert client rect to screen coordinates
    POINT topLeft = { clientRect.left, clientRect.top };
    POINT bottomRight = { clientRect.right, clientRect.bottom };
    ClientToScreen(g_hwnd, &topLeft);
    ClientToScreen(g_hwnd, &bottomRight);

    // Check if cursor is inside with margin
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

// Paint the window
static void OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT clientRect;
    GetClientRect(hwnd, &clientRect);

    // Create back buffer for flicker-free drawing
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBitmap = CreateCompatibleBitmap(hdc, clientRect.right, clientRect.bottom);
    HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

    // Determine background color
    COLORREF bgColor;
    switch (g_state) {
        case STATE_START:
            bgColor = RGB(30, 30, 30);  // Dark gray for start menu
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
    HFONT largeFont = CreateFontA(48, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT mediumFont = CreateFontA(32, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT smallFont = CreateFontA(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");

    // Draw header instructions at top
    COLORREF instructionColor = (g_state == STATE_TOO_EARLY) ? COLOR_BLACK : COLOR_WHITE;
    DrawCenteredText(memDC, "ESC = Quit | F11 = Fullscreen | R = Reset Scores",
                     20, smallFont, instructionColor);

    int contentY = clientRect.bottom / 3;
    char buffer[256];

    switch (g_state) {
        case STATE_START:
            DrawCenteredText(memDC, "Reaction Time Tester", contentY, largeFont, COLOR_WHITE);
            DrawCenteredText(memDC, "Click to Start", contentY + 80, mediumFont, COLOR_GREEN);
            DrawCenteredText(memDC, "When the screen turns GREEN, wait...", contentY + 140, smallFont, COLOR_WHITE);
            DrawCenteredText(memDC, "When it turns RED, click as fast as you can!", contentY + 170, smallFont, COLOR_WHITE);
            break;

        case STATE_WAITING:
            DrawCenteredText(memDC, "Wait for RED...", contentY, largeFont, COLOR_WHITE);
            DrawCenteredText(memDC, "Click when the screen turns RED", contentY + 70, mediumFont, COLOR_WHITE);
            break;

        case STATE_READY:
            DrawCenteredText(memDC, "CLICK NOW!", contentY + 20, largeFont, COLOR_WHITE);
            break;

        case STATE_RESULT:
            {
                int resultY = 100;
                snprintf(buffer, sizeof(buffer), "Reaction Time: %.1f ms", g_reactionTime);
                DrawCenteredText(memDC, buffer, resultY, largeFont, COLOR_WHITE);
                DrawCenteredText(memDC, "Click to try again", resultY + 60, mediumFont, COLOR_WHITE);

                // Show last scores
                if (g_scoreCount > 0) {
                    DrawCenteredText(memDC, "Last scores:", resultY + 130, mediumFont, COLOR_WHITE);
                    int y = resultY + 170;
                    for (int i = 0; i < g_scoreCount; i++) {
                        // Show scores in reverse order of entry (newest first)
                        int idx = (g_scoreIndex - 1 - i + 5) % 5;
                        if (i < g_scoreCount) {
                            snprintf(buffer, sizeof(buffer), "%d. %.1f ms", i + 1, g_scores[idx]);
                            DrawCenteredText(memDC, buffer, y, smallFont, COLOR_WHITE);
                            y += 30;
                        }
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
    }

    // Copy back buffer to screen
    BitBlt(hdc, 0, 0, clientRect.right, clientRect.bottom, memDC, 0, 0, SRCCOPY);

    // Cleanup
    SelectObject(memDC, oldBitmap);
    DeleteObject(memBitmap);
    DeleteDC(memDC);
    DeleteObject(largeFont);
    DeleteObject(mediumFont);
    DeleteObject(smallFont);

    EndPaint(hwnd, &ps);
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

    if (raw->header.dwType == RIM_TYPEMOUSE) {
        // Check for left mouse button down
        if (raw->data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) {
            // Only process clicks if mouse is inside window (with margin)
            if (!IsMouseInsideWindow(10)) {
                return;
            }

            switch (g_state) {
                case STATE_START:
                    // Start the game
                    StartWaiting();
                    break;

                case STATE_WAITING:
                    // Clicked too early
                    g_state = STATE_TOO_EARLY;
                    QueryPerformanceCounter(&g_tooEarlyTime);
                    InvalidateRect(hwnd, NULL, FALSE);
                    break;

                case STATE_READY:
                    // Calculate reaction time
                    {
                        LARGE_INTEGER now;
                        QueryPerformanceCounter(&now);
                        g_reactionTime = (double)(now.QuadPart - g_flashTime.QuadPart) * 1000.0 / (double)g_perfFreq.QuadPart;
                        AddScore(g_reactionTime);
                        g_state = STATE_RESULT;
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                    break;

                case STATE_RESULT:
                    // Start new round
                    StartWaiting();
                    break;

                case STATE_TOO_EARLY:
                    // Ignore clicks during too early message
                    break;
            }
        }
    }
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
                rid.usUsagePage = 0x01; // HID_USAGE_PAGE_GENERIC
                rid.usUsage = 0x02;     // HID_USAGE_GENERIC_MOUSE
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

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                PostQuitMessage(0);
            } else if (wParam == VK_F11) {
                ToggleFullscreen(hwnd);
            } else if (wParam == 'R') {
                ResetScores();
            }
            return 0;

        case WM_ERASEBKGND:
            return 1; // Prevent flicker

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

    // Seed random number generator
    srand((unsigned int)time(NULL));

    // Register window class
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
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
        // Process all pending messages
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                timeEndPeriod(1);
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

        // Small sleep to prevent 100% CPU usage while maintaining responsiveness
        Sleep(1);
    }
}
