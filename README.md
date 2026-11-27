# Mmoni

Mmoni is a minimal, ultra-lightweight Windows 11 desktop telemetry overlay for CPU and GPU temperature, power, load, and clocks.

**Mmoni v2** is a native **C++20 (Win32 + Windows.UI.Composition + Direct2D/DirectWrite)** rewrite with zero runtime dependencies (no `.NET`, no MSVC redistributable required) and direct modern hardware telemetry (**PawnIO + Windows PDH + NVML/NVAPI + ADLX + Intel IGCL**).

## Highlights (v2 Native)

- **~507 KB single executable** (`~2.16 MB` Inno Setup installer), statically linked (`/MT`)
- **~13–15 MB Working Set** and **0.00% idle CPU** at 1 Hz polling
- **< 1 ms sensor polling** (vs ~327 ms with LibreHardwareMonitorLib)
- **Single-HWND Windows 11 Liquid Glass** (`HostBackdropBrush` + Direct2D/DirectWrite pinned strictly to iGPU on hybrid laptops, plus **Precision Dark** no-blur theme)
- **Native monitor refresh-rate transitions** (DWM `DwmFlush()` VSync pacer + QPC, sleeping at 0% CPU when idle)
- **True cross-process click-through** outside the visible pill/peek/dashboard bounds (`SetWindowRgn`), Aero Peek exclusion (`DWMWA_EXCLUDED_FROM_PEEK`), and Optimus/hybrid dGPU D3 sleep protection via `CfgMgr32`

## Build & Install (Windows 11 22H2+)

Requires **Visual Studio 2022/2026 C++ Build Tools** (with Windows 11 SDK) and optionally **[Inno Setup 6](https://jrsoftware.org/isinfo.php)** for packaging.

```powershell
# Build native Release binary (native\build\release\Mmoni.exe)
powershell -ExecutionPolicy Bypass -File native\scripts\build-native.ps1 -Configuration Release

# Build Release binary + single native installer (dist\Mmoni-Setup-2.0.0-win-x64-native.exe)
powershell -ExecutionPolicy Bypass -File native\scripts\build-installer-native.ps1 -Configuration Release

# Run head-to-head live GUI benchmark
powershell -ExecutionPolicy Bypass -File native\scripts\benchmark-compare.ps1 -SteadySeconds 6
```

The native installer (`Mmoni-Setup-2.0.0-win-x64-native.exe`) is a drop-in upgrade over Mmoni v1.0.0:
- Installs cleanly to `C:\Program Files\Mmoni` (same `AppId`, preserves `%LOCALAPPDATA%\Mmoni\settings.json`)
- Registers **Windows Task Scheduler** via native COM (`ITaskService`) for UAC-free Administrator startup at sign-in
- Gracefully exits any running instance during upgrades/uninstalls via `--exit`

## Overlay Controls

- **3-State Island**: Minimal pill expands on hover (**Hover Peek**) or click (**Full Dashboard** with dual Catmull-Rom charts, session Peak/Avg, and interactive hover scrubbing). Drag the pill or dashboard header to move across multi-DPI monitors.
- **Tray Menu**: Right-click the tray icon to toggle visibility, switch visual style (**Liquid Glass** / **Precision Dark**), choose pill metrics (**Temp + Power** / **Temp + Load** / **Temp only**), adjust history range (`60s` / `5m` / `15m`), pick GPU, toggle **Hover Peek**, **Auto-hide in fullscreen**, **Click-through**, **Start with Windows (Admin)**, or exit. Double-click the tray icon (or re-launch Mmoni) to show the overlay.

## Hardware & Sensor Access

- **CPU**: Modern Intel (Core / Core Ultra) via embedded signed `IntelMSR.bin` and AMD Ryzen (Zen 1–5) via `AMDFamily17.bin` (from official [PawnIO.Modules v0.2.2](https://github.com/namazso/PawnIO.Modules/releases/tag/0.2.2)), paired with Windows PDH English counters (`% Processor Utility` & `% Processor Performance`) for load and effective clock.
- **GPU**: NVIDIA (`nvml.dll` primary + `nvapi64.dll` fallback), AMD (`amdadlx64.dll` ADLX), and Intel Arc / Iris Xe (`ControlLib.dll` IGCL + iGPU-only RAPL PP1 fallback), all dynamically loaded from `System32` with per-adapter `CfgMgr32` D3 sleep guard.
- For CPU package temperature and power sensors on Windows 11, install the official signed PawnIO driver: [PawnIO releases](https://github.com/namazso/PawnIO.Setup/releases).

## Legacy v1 (.NET 10 / WPF)

The original v1 WPF project (`Mmoni.csproj`) remains in the repository root for historical reference and head-to-head benchmarking.
