# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# Reaction Time Tester

A Windows application to test mouse click reaction time with high precision timing.

## Build

Requires Visual Studio 2022 Build Tools.

```batch
cmd.exe /c "\"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat\" x64 && cd /d I:\Projects\ReactionTime && rc /fo resource.res resource.rc && cl /O2 /EHsc /DNDEBUG /Fe:ReactionTime.exe main.cpp resource.res user32.lib gdi32.lib winmm.lib msimg32.lib shell32.lib /link /SUBSYSTEM:WINDOWS"
```

Or use `build.bat` / CMake.

### Icon Generation

Run `generate_icon.ps1` to create `app.ico` before building (required for Explorer icon):
```powershell
powershell -ExecutionPolicy Bypass -File generate_icon.ps1
```

## Run

```batch
cmd.exe /c "start \"\" I:\Projects\ReactionTime\ReactionTime.exe"
```

## Features

- Raw mouse input (HID-level) for fastest click detection
- High-precision timing with QueryPerformanceCounter
- 1ms timer resolution via timeBeginPeriod
- Tracks last 5 scores in circular buffer
- Shows average reaction time
- Fullscreen support
- Double-buffered rendering (no flicker)
- ESC menu with keybinds, about screen, and quit
- Unified input bindings: keyboard, mouse, or gamepad (XInput) for any action
- Rebindable reset and game click (press any input to rebind)
- Runtime-generated app icon (red circle with "RT")

## Controls

| Key | Action |
|-----|--------|
| ESC | Open/close menu |
| F11 | Toggle fullscreen |
| R (rebindable) | Reset all scores (default: keyboard R) |
| Configured input | Game click (default: left mouse) |
| Gamepad buttons | Any gamepad button can be bound to either action |

## Game States

- `STATE_START` - Initial menu, click to begin
- `STATE_WAITING` - Green screen, random 1-5 second delay
- `STATE_READY` - Red screen, click now!
- `STATE_RESULT` - Shows reaction time and score history
- `STATE_TOO_EARLY` - Clicked before red, 2 second penalty message
- `STATE_MENU` - ESC menu overlay (keybinds, about, quit)
- `STATE_KEYBINDS` - Rebindable key/button configuration
- `STATE_ABOUT` - Credits screen

## Architecture

Single-file Win32 application (`main.cpp`):
- Uses `RAWINPUT` with `RIDEV_INPUTSINK` for mouse
- `joyGetPosEx` polling for gamepad (works with PS5, Xbox, Switch Pro, any DirectInput controller)
- Mouse clicks only register when cursor is 10+ pixels inside window
- GDI for rendering with back-buffer
- UI button system with hover highlighting for menu screens
- Runtime icon creation via GDI (`CreateAppIcon`)
- Resource icon (`resource.rc` + `app.ico`) for Explorer/taskbar
