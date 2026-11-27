using System;
using System.Runtime.InteropServices;

namespace Mmoni.Platform;

/// <summary>
/// Lightweight native Win32 DWM blur-behind backdrop window positioned directly beneath the WPF overlay.
/// Uses ACCENT_ENABLE_BLURBEHIND (lag-free hardware DWM blur on Windows 11) so real frosted glass tracks
/// Chrome's exact bounds and animated CornerRadius smoothly on every frame of expand, collapse, and drag.
/// </summary>
internal sealed class GlassBackdrop : IDisposable
{
    private const string ClassName = "MmoniGlassBackdropWindow";
    private const uint WsPopup = 0x80000000;
    private const uint WsExTopMost = 0x00000008;
    private const uint WsExTransparent = 0x00000020;
    private const uint WsExToolWindow = 0x00000080;
    private const uint WsExNoActivate = 0x08000000;

    private const uint SwpNoActivate = 0x0010;
    private const uint SwpShowWindow = 0x0040;
    private const uint SwpHideWindow = 0x0080;
    private const uint SwpNoMove = 0x0002;
    private const uint SwpNoSize = 0x0001;
    private const uint SwpNoZOrder = 0x0004;
    private const uint SwpNoRedraw = 0x0008;

    private const int WmNcHitTest = 0x0084;
    private const int WmEraseBkgnd = 0x0014;
    private const int WmPaint = 0x000F;
    private const nint HtTransparent = -1;

    // AccentState 3 = ACCENT_ENABLE_BLURBEHIND: continuous, zero-stutter DWM Gaussian blur during live resize/morph.
    private const int AccentEnableBlurBehind = 3;
    private const int WcaAccentPolicy = 19;

    private readonly WndProcDelegate _wndProc;
    private nint _hwnd;
    private ushort _classAtom;
    private bool _visible;
    private int _lastLeft, _lastTop, _lastWidth, _lastHeight, _lastRadius;
    private bool _disposed;

    public GlassBackdrop()
    {
        _wndProc = WindowProc;
        CreateNativeWindow();
    }

    private void CreateNativeWindow()
    {
        var hInstance = GetModuleHandleW(null);
        var wndClass = new WndClassEx
        {
            cbSize = (uint)Marshal.SizeOf<WndClassEx>(),
            lpfnWndProc = Marshal.GetFunctionPointerForDelegate(_wndProc),
            hInstance = hInstance,
            hbrBackground = GetStockObject(4), // BLACK_BRUSH
            lpszClassName = ClassName
        };

        _classAtom = RegisterClassExW(ref wndClass);
        if (_classAtom == 0)
            return;

        _hwnd = CreateWindowExW(
            WsExTopMost | WsExTransparent | WsExToolWindow | WsExNoActivate,
            ClassName,
            string.Empty,
            WsPopup,
            0, 0, 1, 1,
            0, 0, hInstance, 0);

        if (_hwnd == 0)
            return;

        var margins = new Margins { Left = -1, Right = -1, Top = -1, Bottom = -1 };
        DwmExtendFrameIntoClientArea(_hwnd, ref margins);
        ApplyBlurBehind(_hwnd);
    }

    public void SyncBehind(nint ownerHwnd, int leftPx, int topPx, int widthPx, int heightPx, int radiusPx, bool ensureZOrder = true)
    {
        if (_disposed || _hwnd == 0 || ownerHwnd == 0 || widthPx <= 4 || heightPx <= 4)
            return;

        // Inset by 1px so the GDI rounded clip sits strictly underneath WPF's anti-aliased 1px border.
        var insetLeft = leftPx + 1;
        var insetTop = topPx + 1;
        var insetW = Math.Max(2, widthPx - 2);
        var insetH = Math.Max(2, heightPx - 2);
        var insetR = Math.Max(1, Math.Min(radiusPx - 1, Math.Min(insetW, insetH) / 2));

        var sizeOrRadiusChanged = !_visible
            || insetW != _lastWidth
            || insetH != _lastHeight
            || insetR != _lastRadius;

        var posChanged = sizeOrRadiusChanged || insetLeft != _lastLeft || insetTop != _lastTop;

        if (posChanged)
        {
            if (sizeOrRadiusChanged)
            {
                var diameter = insetR * 2;
                var rgn = CreateRoundRectRgn(0, 0, insetW + 1, insetH + 1, diameter, diameter);
                if (rgn != 0)
                    SetWindowRgn(_hwnd, rgn, false);
            }

            SetWindowPos(_hwnd, ownerHwnd, insetLeft, insetTop, insetW, insetH, SwpNoActivate | SwpShowWindow | SwpNoRedraw);

            _lastLeft = insetLeft;
            _lastTop = insetTop;
            _lastWidth = insetW;
            _lastHeight = insetH;
            _lastRadius = insetR;
            _visible = true;
        }
        else if (ensureZOrder)
        {
            SetWindowPos(_hwnd, ownerHwnd, 0, 0, 0, 0, SwpNoMove | SwpNoSize | SwpNoActivate);
        }
    }

    public void Hide()
    {
        if (_disposed || _hwnd == 0 || !_visible)
            return;

        SetWindowPos(_hwnd, 0, 0, 0, 0, 0, SwpNoMove | SwpNoSize | SwpNoZOrder | SwpNoActivate | SwpHideWindow);
        _visible = false;
    }

    private static void ApplyBlurBehind(nint hwnd)
    {
        var policy = new AccentPolicy
        {
            AccentState = AccentEnableBlurBehind,
            AccentFlags = 0,
            GradientColor = 0,
            AnimationId = 0
        };

        var size = Marshal.SizeOf<AccentPolicy>();
        var ptr = Marshal.AllocHGlobal(size);
        try
        {
            Marshal.StructureToPtr(policy, ptr, false);
            var data = new WindowCompositionAttributeData
            {
                Attribute = WcaAccentPolicy,
                Data = ptr,
                SizeOfData = size
            };
            SetWindowCompositionAttribute(hwnd, ref data);
        }
        catch
        {
            // Silently ignore on unsupported OS builds; WPF translucent glass still renders cleanly.
        }
        finally
        {
            Marshal.FreeHGlobal(ptr);
        }
    }

    private static nint WindowProc(nint hwnd, uint msg, nint wParam, nint lParam)
    {
        if (msg == WmNcHitTest)
            return HtTransparent;
        if (msg == WmEraseBkgnd)
            return 1;
        if (msg == WmPaint)
        {
            var hdc = BeginPaint(hwnd, out var ps);
            if (hdc != 0)
            {
                FillRect(hdc, ref ps.rcPaint, GetStockObject(4)); // BLACK_BRUSH => alpha 0 under DWM glass
                EndPaint(hwnd, ref ps);
            }
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    public void Dispose()
    {
        if (_disposed)
            return;

        _disposed = true;
        if (_hwnd != 0)
        {
            DestroyWindow(_hwnd);
            _hwnd = 0;
        }

        if (_classAtom != 0)
        {
            UnregisterClassW(ClassName, GetModuleHandleW(null));
            _classAtom = 0;
        }
    }

    private delegate nint WndProcDelegate(nint hwnd, uint msg, nint wParam, nint lParam);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WndClassEx
    {
        public uint cbSize;
        public uint style;
        public nint lpfnWndProc;
        public int cbClsExtra;
        public int cbWndExtra;
        public nint hInstance;
        public nint hIcon;
        public nint hCursor;
        public nint hbrBackground;
        public string? lpszMenuName;
        public string lpszClassName;
        public nint hIconSm;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Margins
    {
        public int Left;
        public int Right;
        public int Top;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct AccentPolicy
    {
        public int AccentState;
        public int AccentFlags;
        public uint GradientColor;
        public int AnimationId;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct WindowCompositionAttributeData
    {
        public int Attribute;
        public nint Data;
        public int SizeOfData;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Rect
    {
        public int Left, Top, Right, Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PaintStruct
    {
        public nint hdc;
        public int fErase;
        public Rect rcPaint;
        public int fRestore;
        public int fIncUpdate;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)]
        public byte[] rgbReserved;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern nint GetModuleHandleW(string? lpModuleName);

    [DllImport("gdi32.dll")]
    private static extern nint GetStockObject(int fnObject);

    [DllImport("gdi32.dll")]
    private static extern nint CreateRoundRectRgn(int x1, int y1, int x2, int y2, int cx, int cy);

    [DllImport("user32.dll")]
    private static extern int SetWindowRgn(nint hWnd, nint hRgn, [MarshalAs(UnmanagedType.Bool)] bool bRedraw);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern ushort RegisterClassExW(ref WndClassEx lpwcx);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UnregisterClassW(string lpClassName, nint hInstance);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern nint CreateWindowExW(
        uint dwExStyle,
        string lpClassName,
        string lpWindowName,
        uint dwStyle,
        int x, int y, int nWidth, int nHeight,
        nint hWndParent, nint hMenu, nint hInstance, nint lpParam);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DestroyWindow(nint hWnd);

    [DllImport("user32.dll")]
    private static extern nint DefWindowProcW(nint hWnd, uint msg, nint wParam, nint lParam);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWindowPos(nint handle, nint insertAfter, int x, int y, int width, int height, uint flags);

    [DllImport("user32.dll")]
    private static extern nint BeginPaint(nint hWnd, out PaintStruct lpPaint);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EndPaint(nint hWnd, ref PaintStruct lpPaint);

    [DllImport("user32.dll")]
    private static extern int FillRect(nint hDC, ref Rect lprc, nint hbr);

    [DllImport("dwmapi.dll")]
    private static extern int DwmExtendFrameIntoClientArea(nint hWnd, ref Margins pMarInset);

    [DllImport("user32.dll")]
    private static extern int SetWindowCompositionAttribute(nint hwnd, ref WindowCompositionAttributeData data);
}
