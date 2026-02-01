# Reaction Time Tester

A Windows application to test mouse click reaction time with high precision timing.

## Build

Requires Visual Studio 2022 Build Tools.

```batch
cmd.exe /c "\"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat\" x64 && cd /d I:\Projects\ReactionTime && cl /O2 /EHsc /DNDEBUG /Fe:ReactionTime.exe main.cpp user32.lib gdi32.lib winmm.lib /link /SUBSYSTEM:WINDOWS"
```

Or use `build.bat` / CMake.

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

## Controls

| Key | Action |
|-----|--------|
| ESC | Quit |
| F11 | Toggle fullscreen |
| R | Reset all scores |

## Game States

- `STATE_START` - Initial menu, click to begin
- `STATE_WAITING` - Green screen, random 1-5 second delay
- `STATE_READY` - Red screen, click now!
- `STATE_RESULT` - Shows reaction time and score history
- `STATE_TOO_EARLY` - Clicked before red, 2 second penalty message

## Architecture

Single-file Win32 application (`main.cpp`):
- Uses `RAWINPUT` with `RIDEV_INPUTSINK` for mouse
- Mouse clicks only register when cursor is 10+ pixels inside window
- GDI for rendering with back-buffer
