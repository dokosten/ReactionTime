# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

Requires Visual Studio 2022 Build Tools. Build outputs go to `build/` (gitignored).

```batch
cmd.exe /c "\"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat\" x64 >nul 2>&1 && cd /d I:\Projects\ReactionTime && if not exist build mkdir build && rc /fo build\resource.res resource.rc && cl /O2 /EHsc /DNDEBUG /Fo:build\ /Fe:build\ReactionTime.exe main.cpp build\resource.res user32.lib gdi32.lib winmm.lib msimg32.lib shell32.lib xinput.lib d3d11.lib d3dcompiler.lib /link /SUBSYSTEM:WINDOWS"
```

**Important:** Kill any running `ReactionTime.exe` before building — the linker cannot overwrite a locked executable.

## Run

```batch
cmd.exe /c "start \"\" I:\Projects\ReactionTime\build\ReactionTime.exe"
```

## Icon Generation

Run once before first build (creates `app.ico` for Explorer/taskbar icon):
```powershell
powershell -ExecutionPolicy Bypass -File generate_icon.ps1
```

## Architecture

Single-file Win32 application (`main.cpp`, ~1500 lines). No frameworks, no dependencies beyond Windows SDK and winmm.lib. All state is global statics.

### Input System

Three input types unified under `InputBinding { InputType type; int code; }`:
- **Mouse:** RAWINPUT with `RIDEV_INPUTSINK` for HID-level click detection. Code: 0=left, 1=right, 2=middle.
- **Keyboard:** WM_KEYDOWN messages. Code: virtual key code.
- **Gamepad:** `joyGetPosEx` polled each frame in the main loop (works with Xbox, PS5 DualSense, Switch Pro, any DirectInput controller). Button codes: 0-31 for digital buttons, 0x100-0x103 for POV D-pad directions.

Controller type is detected at connection via `joyGetDevCapsA` product name ("Xbox" → Xbox layout, "Pro Controller"/"Nintendo" → Switch, else PlayStation). This affects:
- Start/Menu button index (7 for Xbox, 9 for PS/Switch)
- Button display names (A/B/X/Y vs Cross/Circle/Square/Triangle vs B/A/X/Y)

`HandleAction(int action)` centralizes game logic (0=reset, 1=click).
`CaptureRebind(InputType, code)` captures any input during rebind mode.

### Menu Navigation

Two independent highlight systems: `g_hoveredButton` (mouse) and `g_selectedButton` (keyboard/gamepad). Mouse movement clears keyboard selection. Menu navigation via arrow keys, D-pad, or thumbsticks; activation via Enter or any gamepad face button. Start/Options button toggles the menu (same as ESC).

### Rendering

GDI with double-buffered back-buffer (CreateCompatibleBitmap). Buttons are registered during paint via `DrawButton()` and hit-tested for mouse clicks. Runtime icon created via GDI (`CreateAppIcon`); resource icon (`resource.rc` + `app.ico`) provides Explorer/taskbar icon.

### Config

Keybinds saved to `ReactionTime.cfg` next to the executable. Format: `resetType/resetCode/clickType/clickCode` key=value pairs. Supports legacy `keyReset/clickButton` format for backward compatibility.

## Game States

`STATE_START` → `STATE_WAITING` (random 1-5s delay) → `STATE_READY` (measure reaction) → `STATE_RESULT` (shows score history). `STATE_TOO_EARLY` on premature click (2s penalty). `STATE_MENU`/`STATE_KEYBINDS`/`STATE_ABOUT` for ESC menu overlay.
