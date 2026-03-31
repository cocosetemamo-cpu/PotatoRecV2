# 🥔 PotatoRec

> Ultra-lightweight screen/game recorder for low-end PCs — **zero mouse interference guaranteed**

---

## Why PotatoRec?

Every screen recorder you've tried breaks your mouse in games. Here's why — and how PotatoRec fixes it.

### The Mouse Lag Problem (and the Fix)

| What other recorders do | Why it breaks your mouse |
|---|---|
| `SetWindowsHookEx(WH_MOUSE_LL)` | Global hook intercepts ALL mouse events BEFORE they reach the game |
| `SetCapture()` / `ClipCursor()` | Steals mouse ownership from the game |
| Reading raw input that competes with the game | Windows delivers the same packet to both; if recorder is slow → game's input gets delayed |

**PotatoRec's approach:**
- Uses `GetCursorPos()` to read the cursor position — a **read-only, zero-overhead** call on win32k.sys shared memory
- **No hooks registered.** The game never knows PotatoRec exists.
- Cursor shape read from `GetCursorInfo()` — same zero-interference approach
- Result: your mouse feels **identical** to not recording at all

### Performance Design

| Feature | Why it's fast |
|---|---|
| **DXGI Desktop Duplication** | GPU copies the frame — CPU does nothing on static screens |
| **Intel Quick Sync encoding** | Even old Celeron has QSV; encodes H.264 at ~0% CPU |
| **Frame drop on queue full** | Drops frames silently rather than causing lag or memory bloat |
| **CBR rate control** | No lookahead = less CPU than quality-based encoding |
| **SSE2 integer scaling** | Fast software downscale for 1080p→720p without AVX2 |

---

## Requirements

- **Windows 10** or later (Windows 7/8 won't work — DXGI Desktop Duplication requires Win10)
- **DirectX 11** GPU (Intel HD Graphics 2000+ works, including Celeron)
- **No install needed** — single `.exe` + optional `.ini` config

---

## Usage

1. **Download** `PotatoRec.exe` from Releases
2. **Run it** — a tray icon appears (looks like a generic app icon)
3. **Press F9** to start recording
4. **Press F9 again** to stop
5. File saved to `Documents\PotatoRec\PotatoRec_YYYYMMDD_HHMMSS.mp4`

### Tray Icon
Right-click the tray icon to access the menu:
- ▶ Start Recording
- ⏹ Stop Recording  
- Exit PotatoRec

---

## Configuration

Copy `PotatoRec.ini.example` to `PotatoRec.ini` (same folder as the .exe) and edit:

```ini
[Video]
FPS=30          ; Frames per second (Celeron: stick to 30)
Bitrate=3000000 ; 3 Mbps = good quality 720p
Width=1280      ; Scale to 720p to save CPU (recommended for Celeron!)
Height=720
HWEncode=1      ; 1 = use Intel QSV (STRONGLY recommended)
ShowCursor=1    ; Show mouse cursor in recording

[Audio]
Enable=1        ; Record audio
Loopback=1      ; 1 = system audio, 0 = microphone

[Hotkey]
Key=120         ; F9 (0x78 hex = 120 decimal)
Mod=0           ; No modifier
```

### Recommended settings for Celeron

```ini
[Video]
FPS=30
Bitrate=2000000   ; 2 Mbps is enough for 720p
Width=1280        ; SCALE DOWN from 1080p → saves massive CPU
Height=720
HWEncode=1        ; Quick Sync does the heavy lifting
```

---

## Building from Source

### Prerequisites
- Visual Studio 2022 (Community is free) with "Desktop development with C++" workload
- CMake 3.20+
- Git

### Build locally

```powershell
git clone https://github.com/yourusername/PotatoRec.git
cd PotatoRec

cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# Output: build\bin\Release\PotatoRec.exe
```

### GitHub Actions

Push to `main` or any tag `v*` to trigger a build. Download the artifact from the Actions tab.

To create a release:
```bash
git tag v1.0.0
git push origin v1.0.0
# → GitHub Actions builds and creates a Release automatically
```

---

## Limitations

- **Windows 10+ only** (DXGI Desktop Duplication requirement)
- **Exclusive fullscreen games**: DXGI Duplication may fail if a game takes exclusive fullscreen control. Use **Borderless Windowed** mode in the game for best results. PotatoRec will retry automatically when you alt-tab or the game switches modes.
- **HDR monitors**: HDR output is captured but tone-mapped to SDR in the output video (MF limitation)
- **Multi-monitor**: Records one monitor at a time; change `Monitor=` in config

---

## How it works (Technical)

```
Game runs → GPU composites frame → DWM (Desktop Window Manager)
                                          ↓
                              DXGI Desktop Duplication
                                          ↓
                              PotatoRec capture thread
                              (copies GPU tex → staging tex → CPU)
                                          ↓
                              GetCursorPos() ← READ ONLY, no hooks
                              CompositeCursor() ← software blit
                                          ↓
                              Frame queue (4 frames max)
                                          ↓
                              Encode thread → Intel QSV (MF)
                                          ↓
                              IMFSinkWriter → MP4 file
```

Audio runs in parallel:
```
WASAPI Loopback → PCM float → IMFSinkWriter (AAC encoder) → MP4 file
```

---

## License

MIT — do whatever you want with it.
