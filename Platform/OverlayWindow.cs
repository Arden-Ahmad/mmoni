using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace Mmoni.Platform;

internal readonly record struct OverlayPlacement(
    Rectangle CompactVisual,
    Rectangle TargetVisual,
    Rectangle StageWindow,
    double CompactOffsetX,
    double CompactOffsetY,
    double TargetOffsetX,
    double TargetOffsetY,
    double DpiScaleX,
    double DpiScaleY);

internal static class OverlayWindow
{
    public const double ShadowPadLeft = 18;
    public const double ShadowPadTop = 6;
    public const double ShadowPadRight = 18;
    public const double ShadowPadBottom = 26;

    private const int GwlExStyle = -20;
    private const int WsExTransparent = 0x00000020;
    private const int WsExToolWindow = 0x00000080;
    private const int WsExNoActivate = 0x08000000;

    private static readonly nint HwndTopMost = new(-1);
    private const uint SwpNoSize = 0x0001;
    private const uint SwpNoActivate = 0x0010;
    private const uint SwpMoveTopMost = SwpNoSize | SwpNoActivate;
    private const uint SwpBoundsTopMost = SwpNoActivate;
    private const uint MonitorDefaultToNearest = 2;
    private const int MdtEffectiveDpi = 0;

    public static void ConfigureOverlay(nint handle, bool clickThrough)
    {
        var current = GetWindowLongPtr(handle, GwlExStyle);
        var next = current | WsExToolWindow | WsExNoActivate;
        next = clickThrough ? next | WsExTransparent : next & ~WsExTransparent;
        if (current != next)
            SetWindowLongPtr(handle, GwlExStyle, next);
    }

    public static void SetClickThrough(nint handle, bool enabled)
    {
        var current = GetWindowLongPtr(handle, GwlExStyle);
        var next = enabled ? current | WsExTransparent : current & ~WsExTransparent;
        if (current != next)
            SetWindowLongPtr(handle, GwlExStyle, next);
    }

    public static Point GetPosition(nint handle)
    {
        GetWindowRect(handle, out var bounds);
        return new Point(bounds.Left, bounds.Top);
    }

    public static (double ScaleX, double ScaleY) GetMonitorDpiScale(Point? anchor, nint fallbackHandle)
    {
        try
        {
            var monitor = anchor is Point p
                ? MonitorFromPoint(new NativePoint { X = p.X, Y = p.Y }, MonitorDefaultToNearest)
                : MonitorFromWindow(fallbackHandle, MonitorDefaultToNearest);

            if (monitor != 0 && GetDpiForMonitor(monitor, MdtEffectiveDpi, out var dpiX, out var dpiY) == 0 && dpiX > 0 && dpiY > 0)
                return (dpiX / 96.0, dpiY / 96.0);
        }
        catch
        {
            // Fallback to caller-supplied DPI.
        }

        return (1.0, 1.0);
    }

    public static OverlayPlacement ComputePlacement(
        nint handle,
        Point? compactAnchor,
        double compactWidthDip,
        double compactHeightDip,
        double targetWidthDip,
        double targetHeightDip,
        double fallbackDpiX,
        double fallbackDpiY)
    {
        var (dpiScaleX, dpiScaleY) = GetMonitorDpiScale(compactAnchor, handle);
        if (dpiScaleX <= 0 || dpiScaleY <= 0)
        {
            dpiScaleX = fallbackDpiX;
            dpiScaleY = fallbackDpiY;
        }

        var area = compactAnchor is Point point
            ? Screen.FromPoint(point).WorkingArea
            : Screen.PrimaryScreen?.WorkingArea ?? Screen.FromHandle(handle).WorkingArea;

        var compactW = Math.Max(1, (int)Math.Round(compactWidthDip * dpiScaleX));
        var compactH = Math.Max(1, (int)Math.Round(compactHeightDip * dpiScaleY));
        var targetW = Math.Max(compactW, (int)Math.Round(targetWidthDip * dpiScaleX));
        var targetH = Math.Max(compactH, (int)Math.Round(targetHeightDip * dpiScaleY));

        var topMargin = (int)Math.Round(5 * dpiScaleY);
        var desiredCompact = compactAnchor ?? new Point(area.Left + (area.Width - compactW) / 2, area.Top + topMargin);

        var compactLeft = Math.Clamp(desiredCompact.X, area.Left, Math.Max(area.Left, area.Right - compactW));
        var compactTop = Math.Clamp(desiredCompact.Y, area.Top, Math.Max(area.Top, area.Bottom - compactH));

        var desiredTargetLeft = compactLeft - (targetW - compactW) / 2;
        var desiredTargetTop = compactTop;

        var targetLeft = Math.Clamp(desiredTargetLeft, area.Left, Math.Max(area.Left, area.Right - targetW));
        var targetTop = Math.Clamp(desiredTargetTop, area.Top, Math.Max(area.Top, area.Bottom - targetH));

        var padLPx = (int)Math.Round(ShadowPadLeft * dpiScaleX);
        var padTPx = (int)Math.Round(ShadowPadTop * dpiScaleY);
        var padRPx = (int)Math.Round(ShadowPadRight * dpiScaleX);
        var padBPx = (int)Math.Round(ShadowPadBottom * dpiScaleY);

        var stageLeft = targetLeft - padLPx;
        var stageTop = targetTop - padTPx;
        var stageW = targetW + padLPx + padRPx;
        var stageH = targetH + padTPx + padBPx;

        var compactOffsetX = ShadowPadLeft + Math.Clamp((compactLeft - targetLeft) / dpiScaleX, 0, Math.Max(0, targetWidthDip - compactWidthDip));
        var compactOffsetY = ShadowPadTop + Math.Clamp((compactTop - targetTop) / dpiScaleY, 0, Math.Max(0, targetHeightDip - compactHeightDip));

        return new OverlayPlacement(
            new Rectangle(compactLeft, compactTop, compactW, compactH),
            new Rectangle(targetLeft, targetTop, targetW, targetH),
            new Rectangle(stageLeft, stageTop, stageW, stageH),
            compactOffsetX,
            compactOffsetY,
            ShadowPadLeft,
            ShadowPadTop,
            dpiScaleX,
            dpiScaleY);
    }

    public static void MoveTopMost(nint handle, int left, int top) =>
        SetWindowPos(handle, HwndTopMost, left, top, 0, 0, SwpMoveTopMost);

    public static void SetBoundsTopMost(nint handle, int left, int top, int width, int height) =>
        SetWindowPos(handle, HwndTopMost, left, top, width, height, SwpBoundsTopMost);

    public static bool IsFullscreenAppActive()
    {
        try
        {
            if (SHQueryUserNotificationState(out var state) == 0)
                return state is QueryUserNotificationState.Busy
                    or QueryUserNotificationState.RunningD3DFullScreen
                    or QueryUserNotificationState.PresentationMode;
        }
        catch
        {
            // Ignore shell query errors.
        }

        return false;
    }

    private enum QueryUserNotificationState
    {
        NotPresent = 1,
        Busy = 2,
        RunningD3DFullScreen = 3,
        PresentationMode = 4,
        AcceptsNotifications = 5,
        QuietTime = 6,
        App = 7
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativePoint
    {
        public int X;
        public int Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct WindowRect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    private static nint GetWindowLongPtr(nint hWnd, int nIndex) =>
        IntPtr.Size == 8 ? GetWindowLongPtr64(hWnd, nIndex) : GetWindowLong32(hWnd, nIndex);

    private static nint SetWindowLongPtr(nint hWnd, int nIndex, nint dwNewLong) =>
        IntPtr.Size == 8 ? SetWindowLongPtr64(hWnd, nIndex, dwNewLong) : SetWindowLong32(hWnd, nIndex, (int)dwNewLong);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongW")]
    private static extern int GetWindowLong32(nint hWnd, int nIndex);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
    private static extern nint GetWindowLongPtr64(nint hWnd, int nIndex);

    [DllImport("user32.dll", EntryPoint = "SetWindowLongW")]
    private static extern int SetWindowLong32(nint hWnd, int nIndex, int dwNewLong);

    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")]
    private static extern nint SetWindowLongPtr64(nint hWnd, int nIndex, nint dwNewLong);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetWindowRect(nint handle, out WindowRect bounds);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWindowPos(nint handle, nint insertAfter, int x, int y, int width, int height, uint flags);

    [DllImport("user32.dll")]
    private static extern nint MonitorFromPoint(NativePoint pt, uint dwFlags);

    [DllImport("user32.dll")]
    private static extern nint MonitorFromWindow(nint hwnd, uint dwFlags);

    [DllImport("shcore.dll")]
    private static extern int GetDpiForMonitor(nint hmonitor, int dpiType, out uint dpiX, out uint dpiY);

    [DllImport("shell32.dll")]
    private static extern int SHQueryUserNotificationState(out QueryUserNotificationState pquns);
}
