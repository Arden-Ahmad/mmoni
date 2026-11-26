# Mmoni

Mmoni is a minimal Windows desktop overlay for CPU and GPU temperature and power. It uses WPF on .NET 10 and samples LibreHardwareMonitorLib once per second.

## Run locally

With the .NET 10 SDK installed, run this from the project folder.

```powershell
dotnet run --configuration Release
```

The Release executable is in `bin\Release\net10.0-windows\Mmoni.exe`.

## Overlay controls

- The always-on-top mini pill (`204 × 26` DIP) displays only CPU and GPU temperature and power numbers.
- Hover over the mini pill to auto-expand hardware details (toggleable via **Auto-expand on hover** in the tray menu), or click to expand/collapse and pin it open. Drag the pill or expanded header to move it.
- Select **Temp**, **Power**, **Load**, or **Clock** to view up to 60 seconds of history.
- Right-click the Mmoni tray icon to show or hide the overlay, toggle **Auto-expand on hover**, toggle **Click-through**, **Reset position**, switch GPU, enable startup at sign-in, or exit. Double-click the tray icon to toggle visibility.
- Use **Automatic** GPU selection to prefer NVIDIA or AMD adapters over Intel integrated graphics when available. Polling follows the selected GPU and pauses automatically while the overlay is hidden.

## Sensor access

An em dash indicates an unavailable reading. Available sensors depend on the hardware, graphics driver, and process privileges.

For Intel CPU package temperature and power, install the official PawnIO driver and use **Restart as administrator** in Mmoni's tray menu. Windows startup opens Mmoni with normal user privileges. [PawnIO releases](https://github.com/namazso/PawnIO.Setup/releases) · [LibreHardwareMonitor sensor access](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor#administrator-rights)

Position, auto-expand on hover, click-through, and GPU selection are saved per user under `%LOCALAPPDATA%\Mmoni`.
