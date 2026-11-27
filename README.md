# Mmoni

Mmoni is a minimal Windows desktop overlay for CPU and GPU temperature, power, load, and clocks. It uses WPF on .NET 10 and samples LibreHardwareMonitorLib once per second.

## Install (Recommended)

Build the Windows installers (requires `.NET 10 SDK` and [Inno Setup 6](https://jrsoftware.org/isinfo.php)):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-installer.ps1
```

This generates two installers in `dist\`:
- **`Mmoni-Setup-1.0.0-win-x64-standalone.exe`** — Self-contained; works on any 64-bit Windows 10/11 PC without installing .NET separately.
- **`Mmoni-Setup-1.0.0-win-x64-net10.exe`** — Lightweight installer for PCs that already have the .NET 10 Desktop Runtime.

The installer:
- Installs cleanly to `C:\Program Files\Mmoni`
- Registers **Mmoni** in **Windows Settings → Installed Apps / Control Panel** with a full uninstaller
- Creates Start Menu (and optional Desktop) shortcuts
- Optionally configures **Windows Task Scheduler** to start Mmoni automatically at sign-in with **Administrator privileges and no UAC prompt** (with battery & execution-time restrictions disabled)
- Gracefully stops running instances during upgrades/uninstalls and cleans up the scheduled task on uninstall

## Run locally (Development)

With the .NET 10 SDK installed, run this from the project folder:

```powershell
dotnet run --configuration Release
```

The Release executable is in `bin\Release\net10.0-windows\Mmoni.exe`.

## Overlay controls

- The always-on-top mini pill displays CPU and GPU telemetry and expands on hover (**Hover Peek**) or click (**Dashboard**). Drag the pill or expanded header to move it.
- Right-click the Mmoni tray icon to show/hide the overlay, switch visual style, choose collapsed pill metrics, adjust history range, switch GPU, toggle **Start with Windows (Admin)**, or exit. Double-click the tray icon (or launch Mmoni again from the Start Menu) to show the overlay.
- Use **Automatic** GPU selection to prefer NVIDIA or AMD adapters over Intel integrated graphics when available. Polling pauses automatically while the overlay is hidden, in fullscreen apps, or when the session is locked.

## Sensor access

An em dash indicates an unavailable reading. Available sensors depend on the hardware, graphics driver, and process privileges.

Mmoni requests Administrator privileges via `app.manifest` and uses Windows Task Scheduler (`RunLevel: HighestAvailable`) for UAC-free startup at sign-in. For Intel/AMD CPU package temperature and power sensors on modern Windows, install the official PawnIO driver: [PawnIO releases](https://github.com/namazso/PawnIO.Setup/releases) · [LibreHardwareMonitor sensor access](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor#administrator-rights)

Settings and saved overlay position are stored per user under `%LOCALAPPDATA%\Mmoni\settings.json`.
