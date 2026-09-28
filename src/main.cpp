// ============================================================================
//  QuickSearch — CEF (Chromium Embedded Framework) SEB overlay.
//
//  Same job as the WebView2 overlay (float on top, host into the SEB client
//  window, survive its close, desktop handoff) but the page runs in CEF with
//  off-screen rendering: we own the framebuffer, so the overlay is one single
//  layered/child window again (no foreign child HWND), screenshots come from
//  our own DIB, and input is injected with CefBrowserHost::SendMouse*Event —
//  the exact shape of the proven Ultralight overlay.
//
//  Process naming: WebView2 could NOT be renamed (loader hardcodes
//  msedgewebview2.exe; the browser binary self-gates under any other name).
//  CEF has no such restriction: CefSettings.browser_subprocess_path points at
//  OUR OWN helper exe (qshelper.exe), and every renderer/utility
//  subprocess is launched as a copy of it with --type=... args. Task Manager
//  therefore shows only QuickSearch.exe + qshelper.exe — no chrome,
//  msedge, or Chromium branding anywhere.
//
//  Rendering: CEF OSR paints BGRA into our page DIB via OnPaint; we composite
//  page + header + sidebar into one DIB and present it with
//  UpdateLayeredWindow (standalone) or BitBlt in WM_PAINT (hosted child).
//  CPU only: --disable-gpu (software raster), per the long-standing
//  requirement. Login persists via a cache dir under %LOCALAPPDATA%\QuickSearch.
//
//  Input model (click-through, per the latest accepted overlay):
//   - The overlay is WS_EX_TRANSPARENT/WS_EX_NOACTIVATE: the OS never
//     activates us and physical input falls through.
//   - A low-level mouse hook consumes input over the overlay and re-delivers
//     it to CEF via SendMouseMoveEvent/SendMouseClickEvent/SendMouseWheelEvent
//     — no focus steal, SEB keeps the foreground, native cursor stays VISIBLE.
//   - The keyboard hook carries only the global chords; the page gets no
//     keyboard except Ctrl+V paste (injected into the composer).
//
//  Shortcuts: Alt+` hide/show | Ctrl+Alt+S screenshot | Ctrl+Alt+Q quit
//  Command line: --url <u> --notopmost --x --y --w --h
//               --hidden --handoff-event <name> --handoff-wait
// ============================================================================
#include <Windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <process.h>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#include <objidl.h>   // IStream/PROPID required by the GDI+ headers
#include <gdiplus.h>

#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_browser.h"
#include "include/cef_render_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_display_handler.h"
#include "include/cef_command_line.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "rpcrt4.lib")

// ---------------------------------------------------------------- settings
static std::string g_url    = "https://chatgpt.com/";
static std::string g_dataDir;                  // %LOCALAPPDATA%\QuickSearch
static int  g_x = 0, g_y = 0, g_w = 1280, g_h = 900;
static bool g_topmost = true;
static bool g_startHidden = false;

static const char* kAppName = "QuickSearch";

// ---------------------------------------------------------------- logging
static void Log(const char* fmt, ...) {
  char buf[2048];
  va_list ap; va_start(ap, fmt);
  _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
  va_end(ap);
  OutputDebugStringA(buf); OutputDebugStringA("\n");
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s\\overlay.log", g_dataDir.c_str());
  if (FILE* f = fopen(path, "a")) { fprintf(f, "%s\n", buf); fclose(f); }
}

static bool EnsureDir(const std::string& dir) {
  return CreateDirectoryA(dir.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

static std::string GetArg(int argc, char** argv, const char* name, const char* def) {
  for (int i = 1; i + 1 < argc; ++i)
    if (_stricmp(argv[i], name) == 0) return argv[i + 1];
  return def;
}
static bool HasArg(int argc, char** argv, const char* name) {
  for (int i = 1; i < argc; ++i)
    if (_stricmp(argv[i], name) == 0) return true;
  return false;
}

static std::wstring ToWide(const char* s) {
  int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  std::wstring out(len > 0 ? (size_t)len - 1 : 0, L'\0');
  if (len > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], len);
  return out;
}
static std::wstring ToWide(const std::string& s) { return ToWide(s.c_str()); }
static std::string ToUtf8(const wchar_t* s) {
  int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  std::string out(len > 0 ? (size_t)len - 1 : 0, '\0');
  if (len > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], len, nullptr, nullptr);
  return out;
}

// ---------------------------------------------------------------- layout
// Window layout: [ 26px header (drag bar) ][ page (CEF OSR) | 96px sidebar ].
static const int kHeader = 26;    // top drag strip height, window px
static const int kSidebar = 96;   // right screenshot strip width, window px

static int SidebarLeft(int clientW) { return clientW - kSidebar; }

// ---------------------------------------------------------------- GDI+ helpers
static ULONG_PTR g_gdiToken = 0;
static void EnsureGdiPlus() {
  if (!g_gdiToken) {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g_gdiToken, &input, nullptr);
  }
}

// ---------------------------------------------------------------- framebuffers
// Page DIB: what CEF paints via OnPaint (page px, top-down 32bpp BGRA).
static HDC     g_pageDC = nullptr;
static HBITMAP g_pageDib = nullptr, g_pageOld = nullptr;
static void*   g_pageBits = nullptr;
static int     g_pageW = 0, g_pageH = 0;

// Composite DIB: page + header + sidebar chrome, window client px. This is
// what UpdateLayeredWindow / WM_PAINT presents.
static HDC     g_memDC = nullptr;
static HBITMAP g_memDib = nullptr, g_memOld = nullptr;
static void*   g_memBits = nullptr;
static int     g_memW = 0, g_memH = 0;

static HWND g_hwnd = nullptr;

// CEF external-pump scheduling (used by OverlayApp below).
static constexpr UINT kCefWorkMsg  = WM_APP + 40;   // pump work, no delay
static constexpr UINT kCefTimerId  = 90;            // pump work, delayed
static constexpr UINT kPumpTimerId = 91;            // periodic safety pump

static void AllocPage(int w, int h) {
  if (w == g_pageW && h == g_pageH && g_pageDib) return;
  if (g_pageDC) { if (g_pageOld) SelectObject(g_pageDC, g_pageOld); DeleteDC(g_pageDC); g_pageDC = nullptr; }
  if (g_pageDib) { DeleteObject(g_pageDib); g_pageDib = nullptr; }
  g_pageBits = nullptr; g_pageW = g_pageH = 0;
  if (w <= 0 || h <= 0) return;
  HDC screen = GetDC(nullptr);
  g_pageDC = CreateCompatibleDC(screen);
  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  g_pageDib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &g_pageBits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!g_pageDib) { Log("[fb] page DIB alloc failed %lu", GetLastError()); return; }
  g_pageOld = (HBITMAP)SelectObject(g_pageDC, g_pageDib);
  g_pageW = w; g_pageH = h;
  memset(g_pageBits, 0xFF, (size_t)w * h * 4);   // white until first paint
}

static void EnsureComposite(int w, int h) {
  if (w == g_memW && h == g_memH && g_memDib) return;
  if (g_memDC) { if (g_memOld) SelectObject(g_memDC, g_memOld); DeleteDC(g_memDC); g_memDC = nullptr; }
  if (g_memDib) { DeleteObject(g_memDib); g_memDib = nullptr; }
  g_memBits = nullptr; g_memW = g_memH = 0;
  if (w <= 0 || h <= 0) return;
  HDC screen = GetDC(nullptr);
  g_memDC = CreateCompatibleDC(screen);
  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  g_memDib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &g_memBits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!g_memDib) { Log("[fb] composite DIB alloc failed %lu", GetLastError()); return; }
  g_memOld = (HBITMAP)SelectObject(g_memDC, g_memDib);
  g_memW = w; g_memH = h;
}

// Forward decls (defined below with the sidebar section).
static void DrawChrome(HDC hdc, int winW, int winH);
static void CompositeDragGhost(int winW, int winH);
static void ComposeAndPresent();

static void Present() {
  if (!g_hwnd || !g_memDib) return;
  if (GetParent(g_hwnd)) {            // hosted child: normal WM_PAINT path
    InvalidateRect(g_hwnd, nullptr, FALSE);
    return;
  }
  RECT rc;
  GetWindowRect(g_hwnd, &rc);
  SIZE size = { rc.right - rc.left, rc.bottom - rc.top };
  if (size.cx <= 0 || size.cy <= 0) return;
  POINT src = { 0, 0 };
  BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
  HDC screen = GetDC(nullptr);
  UpdateLayeredWindow(g_hwnd, screen, nullptr, &size, g_memDC, &src, 0, &blend, ULW_ALPHA);
  ReleaseDC(nullptr, screen);
}

// Composite the page + chrome and push to screen. Cheap enough to run on
// every CEF OnPaint.
static void ComposeAndPresent() {
  if (!g_hwnd) return;
  RECT rc;
  GetClientRect(g_hwnd, &rc);
  int winW = rc.right - rc.left, winH = rc.bottom - rc.top;
  EnsureComposite(winW, winH);
  if (!g_memDC) return;
  int pageW = SidebarLeft(winW), pageH = winH - kHeader;
  if (pageW > 0 && pageH > 0 && g_pageDC) {
    if (g_pageW != pageW || g_pageH != pageH) AllocPage(pageW, pageH);
    BitBlt(g_memDC, 0, kHeader, g_pageW, g_pageH, g_pageDC, 0, 0, SRCCOPY);
  }
  DrawChrome(g_memDC, winW, winH);
  // Drag ghost is blended into the composite by CompositeDragGhost (defined
  // after the sidebar types it needs).
  CompositeDragGhost(winW, winH);
  // GDI drawing onto a 32bpp DIB leaves the alpha byte undefined/zero; the
  // overlay is fully opaque, so pin alpha at 255 for UpdateLayeredWindow.
  DWORD* px = (DWORD*)g_memBits;
  size_t n = (size_t)winW * winH;
  for (size_t i = 0; i < n; ++i) px[i] |= 0xFF000000;
  Present();
}

// ---------------------------------------------------------------- sidebar
// Right-hand strip holding screenshot thumbnails. Ctrl+Alt+S captures into the
// strip; drag a thumbnail onto the page and release to attach it to the
// ChatGPT composer. Full PNGs live under %LOCALAPPDATA%\QuickSearch\screenshots\
// and a DIB copy is placed on the clipboard.
static const int kThumbW = 80;
static const int kThumbPad = 8;
static const size_t kThumbMax = 40;

struct Thumb {
  std::vector<BYTE> px;   // premultiplied BGRA, w*h*4
  int w = 0, h = 0;
  std::string path;       // full-size PNG (injected on drop)
};

static std::vector<Thumb> g_thumbs;
static int  g_sidebarScroll = 0;
static int  g_thumbDrag = -1;       // thumbnail index being dragged
static POINT g_thumbDragPos = {};   // cursor in window px while dragging

static int ThumbY0(int i) {
  int y = kHeader + kThumbPad;
  for (int j = 0; j < i && j < (int)g_thumbs.size(); ++j)
    y += g_thumbs[j].h + kThumbPad;
  return y;
}
static int ThumbListH() {
  int y = kThumbPad;
  for (auto& t : g_thumbs) y += t.h + kThumbPad;
  return y;
}
static void SidebarScrollClamp(int clientH) {
  int avail = clientH - kHeader - kThumbPad;
  int maxScroll = ThumbListH() - avail;
  if (maxScroll < 0) maxScroll = 0;
  if (g_sidebarScroll < 0) g_sidebarScroll = 0;
  if (g_sidebarScroll > maxScroll) g_sidebarScroll = maxScroll;
}
static int ThumbAt(int wx, int wy, int clientW) {
  if (wx < SidebarLeft(clientW)) return -1;
  for (int i = (int)g_thumbs.size() - 1; i >= 0; --i) {
    int ty = ThumbY0(i) - g_sidebarScroll;
    int tx = SidebarLeft(clientW) + kThumbPad;
    if (wx >= tx && wx < tx + g_thumbs[i].w && wy >= ty && wy < ty + g_thumbs[i].h)
      return i;
  }
  return -1;
}

static void AddThumb(const BYTE* bgra, int w, int h, const char* path) {
  if (!bgra || w <= 0 || h <= 0) return;
  Thumb t;
  t.path = path;
  t.w = kThumbW;
  t.h = (h * kThumbW + w / 2) / w;
  if (t.h < 24) t.h = 24;
  t.px.resize((size_t)kThumbW * t.h * 4);
  for (int y = 0; y < t.h; ++y) {
    int sy = (int)((long long)y * h / t.h);
    const BYTE* srow = bgra + (size_t)sy * w * 4;
    BYTE* drow = &t.px[(size_t)y * kThumbW * 4];
    for (int x = 0; x < kThumbW; ++x) {
      int sx = (int)((long long)x * w / kThumbW);
      memcpy(drow + (size_t)x * 4, srow + (size_t)sx * 4, 4);
    }
  }
  g_thumbs.push_back(std::move(t));
  while (g_thumbs.size() > kThumbMax) g_thumbs.erase(g_thumbs.begin());
  InvalidateRect(g_hwnd, nullptr, FALSE);
  ComposeAndPresent();
  Log("[thumb] added #%u %dx%d (total %u) <- %s",
      (unsigned)g_thumbs.size(), g_thumbs.back().w, g_thumbs.back().h,
      (unsigned)g_thumbs.size(), path);
}

// ---------------------------------------------------------------- drag ghost
// A small layered popup that follows the cursor while a thumbnail drag is in
// flight. The page pixels live in our DIB now, so a separate topmost window
// is the only way the ghost can float above the page.
namespace ghost {
static HWND g_hwnd = nullptr;
static HDC g_memDC = nullptr;
static HBITMAP g_dib = nullptr;
static HBITMAP g_old = nullptr;
static void* g_bits = nullptr;
static int g_w = 0, g_h = 0;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM, LPARAM) {
  switch (msg) {
    case WM_NCHITTEST:     return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
  }
  return DefWindowProc(hwnd, msg, 0, 0);
}

static bool Create() {
  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.lpszClassName = L"CefGhost";
  RegisterClassExW(&wc);
  g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
                               WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                           wc.lpszClassName, L"", WS_POPUP,
                           0, 0, 10, 10, nullptr, nullptr, inst, nullptr);
  return g_hwnd != nullptr;
}

// Show the ghost centered on a screen point using the thumb's pixels,
// pre-multiplied to 75% alpha so the page shows through a little.
static void Show(const Thumb& t, int sx, int sy) {
  if (!g_hwnd) return;
  if (t.w != g_w || t.h != g_h) {
    if (g_memDC) {
      if (g_old) SelectObject(g_memDC, g_old);
      DeleteDC(g_memDC); g_memDC = nullptr;
    }
    if (g_dib) { DeleteObject(g_dib); g_dib = nullptr; }
    HDC screen = GetDC(nullptr);
    g_memDC = CreateCompatibleDC(screen);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = t.w;
    bmi.bmiHeader.biHeight = -t.h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    g_dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &g_bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!g_dib) return;
    g_old = (HBITMAP)SelectObject(g_memDC, g_dib);
    g_w = t.w; g_h = t.h;
  }
  DWORD* dst = (DWORD*)g_bits;
  for (size_t i = 0; i < (size_t)t.w * t.h; ++i) {
    DWORD s = ((const DWORD*)t.px.data())[i];
    BYTE a = (BYTE)(s >> 24);
    a = (BYTE)((unsigned)a * 3 / 4);
    dst[i] = (a << 24) | (((s & 0xFF) * a / 255) & 0xFF) |
             ((((s >> 8) & 0xFF) * a / 255 & 0xFF) << 8) |
             ((((s >> 16) & 0xFF) * a / 255 & 0xFF) << 16);
  }
  int x = sx - t.w / 2, y = sy - t.h / 2;
  SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, t.w, t.h,
               SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
  POINT src = { 0, 0 };
  SIZE size = { t.w, t.h };
  BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
  HDC screen = GetDC(nullptr);
  UpdateLayeredWindow(g_hwnd, screen, nullptr, &size, g_memDC, &src, 0, &blend, ULW_ALPHA);
  ReleaseDC(nullptr, screen);
}
static void Hide() { if (g_hwnd) ShowWindow(g_hwnd, SW_HIDE); }
// Re-assert topmost (the overlay's z-order watchdog also raises itself every
// few dozen ms and would otherwise overtake the ghost during a drag).
static void Raise() {
  if (g_hwnd && IsWindowVisible(g_hwnd))
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}
static void Destroy() {
  Hide();
  if (g_memDC) { if (g_old) SelectObject(g_memDC, g_old); DeleteDC(g_memDC); g_memDC = nullptr; }
  if (g_dib) { DeleteObject(g_dib); g_dib = nullptr; }
  if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = nullptr; }
}
}  // namespace ghost

// ============================================================================
// Fake cursor (rendering only — per the accepted approach in Desktop/src:
// a 30x30 cyan arrow on a topmost layered popup, clipped to the overlay
// bounds, with balanced native-cursor suppression while it is shown)
// ============================================================================
namespace cursor {

static HWND g_hwnd = nullptr;
static HDC g_memDC = nullptr;
static HBITMAP g_dib = nullptr;
static HBITMAP g_oldBmp = nullptr;
static void* g_bits = nullptr;
static int g_nativeAdjustments = 0;
static POINT g_lastPos = { -1, -1 };
static RECT g_lastBounds = {};
static bool g_shown = false;

static LRESULT CALLBACK CursorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_NCHITTEST:     return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
  }
  return DefWindowProc(hwnd, msg, wParam, lParam);
}

static void HideNative() {
  if (g_nativeAdjustments == 0) {
    int displayCount = 0;
    do {
      displayCount = ShowCursor(FALSE);
      ++g_nativeAdjustments;
    } while (displayCount >= 0);
  }
}

static void RestoreNative() {
  while (g_nativeAdjustments > 0) {
    ShowCursor(TRUE);
    --g_nativeAdjustments;
  }
}

static bool Create() {
  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = CursorWndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = L"CefOvCursor";
  if (!RegisterClassExW(&wc)) {
    Log("cursor: RegisterClassEx failed (%lu)", GetLastError());
    return false;
  }
  g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
                               WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                           wc.lpszClassName, L"", WS_POPUP,
                           0, 0, 30, 30, nullptr, nullptr, inst, nullptr);
  if (!g_hwnd) {
    Log("cursor: CreateWindowEx failed (%lu)", GetLastError());
    return false;
  }

  HDC screen = GetDC(nullptr);
  g_memDC = CreateCompatibleDC(screen);
  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = 30;
  bmi.bmiHeader.biHeight = -30;          // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  g_dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &g_bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!g_dib) {
    Log("cursor: CreateDIBSection failed (%lu)", GetLastError());
    return false;
  }
  g_oldBmp = (HBITMAP)SelectObject(g_memDC, g_dib);

  // Arrow polygon: cyan fill, white 2px outline.
  POINT arrow[] = { {1, 1}, {1, 22}, {7, 16}, {12, 27}, {16, 25}, {11, 15}, {19, 15} };
  HBRUSH fill = CreateSolidBrush(RGB(0, 220, 255));
  HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
  HBRUSH oldBrush = (HBRUSH)SelectObject(g_memDC, fill);
  HPEN oldPen = (HPEN)SelectObject(g_memDC, pen);
  Polygon(g_memDC, arrow, (int)(sizeof(arrow) / sizeof(arrow[0])));
  SelectObject(g_memDC, oldBrush);
  SelectObject(g_memDC, oldPen);
  DeleteObject(fill);
  DeleteObject(pen);

  // GDI RGB drawing leaves alpha zero — set opaque alpha on visible pixels.
  DWORD* px = (DWORD*)g_bits;
  for (int i = 0; i < 30 * 30; ++i)
    if (px[i] != 0) px[i] |= 0xFF000000;

  return true;
}

static void Hide() {
  if (!g_shown) return;
  g_shown = false;
  ShowWindow(g_hwnd, SW_HIDE);
  RestoreNative();
}

// Position the fake cursor at the physical point, clipped to `bounds`.
static void Show(POINT pt, const RECT& bounds) {
  if (!g_hwnd) return;
  bool same = g_shown && g_lastPos.x == pt.x && g_lastPos.y == pt.y &&
              EqualRect(&g_lastBounds, &bounds);
  if (same) {
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    return;
  }
  g_lastPos = pt;
  g_lastBounds = bounds;

  int x = pt.x - 1, y = pt.y - 1;   // one pixel above-left of the physical point

  RECT clip = bounds;
  OffsetRect(&clip, -x, -y);
  RECT self = { 0, 0, 30, 30 };
  IntersectRect(&clip, &clip, &self);
  if (IsRectEmpty(&clip)) { Hide(); return; }
  HRGN rgn = CreateRectRgnIndirect(&clip);
  if (SetWindowRgn(g_hwnd, rgn, TRUE) == 0) DeleteObject(rgn);  // Windows owns it on success

  SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, 30, 30,
               SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);

  POINT src = { 0, 0 };
  SIZE size = { 30, 30 };
  BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
  HDC screen = GetDC(nullptr);
  BOOL ok = UpdateLayeredWindow(g_hwnd, screen, nullptr, &size, g_memDC, &src, 0, &blend, ULW_ALPHA);
  ReleaseDC(nullptr, screen);
  if (!ok) { Hide(); return; }

  HideNative();
  g_shown = true;
}

static void Destroy() {
  Hide();
  if (g_memDC) {
    if (g_oldBmp) SelectObject(g_memDC, g_oldBmp);
    DeleteDC(g_memDC);
    g_memDC = nullptr;
  }
  if (g_dib) { DeleteObject(g_dib); g_dib = nullptr; }
  if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = nullptr; }
}

}  // namespace cursor

// ---------------------------------------------------------------- CEF glue
static CefRefPtr<CefBrowser> g_browser;

// JS results ride back on the console: ExecuteJavaScript discards return
// values, so ExecJs wraps the snippet and logs 'CEFOV:<why>|<result>'.
static void ExecJs(const std::string& snippet, const char* why);

// Debug: dump a BGRA buffer to PNG (ground-truthing what CEF painted and
// what we present). Files land next to overlay.log.
static void DbgSavePng(const void* bgra, int w, int h, const char* name) {
  if (!bgra || w <= 0 || h <= 0) return;
  EnsureGdiPlus();
  Gdiplus::Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)const_cast<void*>(bgra));
  char path[MAX_PATH * 2];
  snprintf(path, sizeof(path), "%s\\%s", g_dataDir.c_str(), name);
  CLSID pngClsid;
  UINT num = 0, sz = 0;
  Gdiplus::GetImageEncodersSize(&num, &sz);
  std::vector<char> buf(sz ? sz : 1);
  Gdiplus::GetImageEncoders(num, sz, (Gdiplus::ImageCodecInfo*)buf.data());
  for (UINT i = 0; i < num; ++i) {
    auto* info = (Gdiplus::ImageCodecInfo*)buf.data() + i;
    if (wcscmp(info->MimeType, L"image/png") == 0) {
      pngClsid = info->Clsid;
      bmp.Save(ToWide(path).c_str(), &pngClsid, nullptr);
      Log("[dbg] saved %s (%dx%d)", path, w, h);
      break;
    }
  }
}

class OverlayClient : public CefClient,
                      public CefRenderHandler,
                      public CefLifeSpanHandler,
                      public CefDisplayHandler {
 public:
  OverlayClient() {}

  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }

  // -- CefRenderHandler ------------------------------------------------------
  void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override {
    rect = CefRect(0, 0, g_pageW > 0 ? g_pageW : 64,
                         g_pageH > 0 ? g_pageH : 64);
  }

  bool GetScreenPoint(CefRefPtr<CefBrowser> browser, int viewX, int viewY,
                      int& screenX, int& screenY) override {
    if (!g_hwnd) return false;
    POINT p = { viewX, viewY + kHeader };
    ClientToScreen(g_hwnd, &p);
    screenX = p.x; screenY = p.y;
    return true;
  }

  void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
               const RectList& dirtyRects, const void* buffer,
               int width, int height) override {
    static int s_paintCount = 0;
    if (++s_paintCount <= 3 || s_paintCount % 600 == 0)
      Log("[paint] #%d type=%d %dx%d dirty=%d page=%dx%d", s_paintCount,
          (int)type, width, height, (int)dirtyRects.size(), g_pageW, g_pageH);
    if (type != PET_VIEW || !buffer || width <= 0 || height <= 0) return;
    if (s_paintCount == 1) DbgSavePng(buffer, width, height, "dbg-cef-buffer.png");
    if (width != g_pageW || height != g_pageH || !g_pageBits) {
      static int s_mismatch = 0;
      if (++s_mismatch <= 5)
        Log("[paint] SIZE MISMATCH paint=%dx%d page=%dx%d — dropped", width,
            height, g_pageW, g_pageH);
      return;
    }
    const BYTE* src = (const BYTE*)buffer;
    int stride = width * 4;
    if (dirtyRects.empty()) {
      memcpy(g_pageBits, src, (size_t)height * stride);
    } else {
      for (auto& r : dirtyRects) {
        if (r.x < 0 || r.y < 0 || r.width <= 0 || r.height <= 0) continue;
        int x2 = r.x + r.width > width ? width : r.x + r.width;
        int y2 = r.y + r.height > height ? height : r.y + r.height;
        int w = x2 - r.x, h = y2 - r.y;
        if (w <= 0 || h <= 0) continue;
        for (int y = r.y; y < y2; ++y)
          memcpy((BYTE*)g_pageBits + (size_t)y * stride + (size_t)r.x * 4,
                 src + (size_t)y * stride + (size_t)r.x * 4, (size_t)w * 4);
      }
    }
    if (s_paintCount == 10 && g_memBits)
      DbgSavePng(g_memBits, g_memW, g_memH, "dbg-composite.png");
    ComposeAndPresent();
  }

  // -- CefLifeSpanHandler ----------------------------------------------------
  // ChatGPT's login/OAuth must never spawn a real popup HWND: cancel the
  // popup and load the target in the SAME browser (same fix as the old
  // Ultralight/TLS builds — blank-page-after-email bug).
  bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                     int popup_id, const CefString& target_url,
                     const CefString& target_frame_name,
                     WindowOpenDisposition target_disposition,
                     bool user_gesture, const CefPopupFeatures& popupFeatures,
                     CefWindowInfo& windowInfo, CefRefPtr<CefClient>& client,
                     CefBrowserSettings& settings,
                     CefRefPtr<CefDictionaryValue>& extra_info,
                     bool* no_javascript_access) override {
    std::string url = target_url.ToString();
    Log("[popup] suppressed '%s' (disposition=%d gesture=%d) -> same browser",
        url.c_str(), (int)target_disposition, (int)user_gesture);
    if (!url.empty() && frame)
      frame->LoadURL(target_url);
    return true;   // cancel the popup window
  }

  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    g_browser = browser;
    Log("[cef] browser created id=%d", browser->GetIdentifier());
    if (browser->GetHost()) {
      browser->GetHost()->SetFocus(true);
      browser->GetHost()->WasResized();
    }
  }

  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    if (g_browser && g_browser->IsSame(browser)) g_browser = nullptr;
  }

  // -- CefDisplayHandler -----------------------------------------------------
  bool OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level,
                        const CefString& message, const CefString& source,
                        int line) override {
    std::string m = message.ToString();
    if (m.compare(0, 6, "CEFOV:") == 0) {
      Log("[js] %s", m.c_str() + 6);
      return true;
    }
    return false;   // let CEF log the rest to cef.log
  }

 private:
  IMPLEMENT_REFCOUNTING(OverlayClient);
  DISALLOW_COPY_AND_ASSIGN(OverlayClient);
};

class OverlayApp : public CefApp, public CefBrowserProcessHandler {
 public:
  OverlayApp() {}

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }

  void OnScheduleMessagePumpWork(int64_t delay_ms) override {
    if (!g_hwnd) return;
    if (delay_ms <= 0) {
      PostMessageW(g_hwnd, kCefWorkMsg, 0, 0);
    } else {
      SetTimer(g_hwnd, kCefTimerId, (UINT)(delay_ms > 1000 ? 1000 : delay_ms), nullptr);
    }
  }

  void OnBeforeCommandLineProcessing(const CefString& process_type,
                                     CefRefPtr<CefCommandLine> command_line) override {
    // CPU-only rendering via SwiftShader (pure-software GL): the --disable-gpu
    // flags make CEF 154's OSR compositor emit BLANK white frames (JS/input
    // still work — exactly the "white overlay" bug). SwiftShader keeps the
    // compositor alive with no physical GPU.
    command_line->AppendSwitch("use-angle=swiftshader");
    command_line->AppendSwitch("enable-unsafe-swiftshader");
    command_line->AppendSwitch("no-first-run");
    command_line->AppendSwitch("disable-blink-features=AutomationControlled");
  }

 private:
  IMPLEMENT_REFCOUNTING(OverlayApp);
  DISALLOW_COPY_AND_ASSIGN(OverlayApp);
};

// ---------------------------------------------------------------- JS helpers
static void ExecJs(const std::string& snippet, const char* why) {
  if (!g_browser) return;
  CefRefPtr<CefFrame> frame = g_browser->GetMainFrame();
  if (!frame) return;
  std::string js = "(function(){var __r=" + snippet +
                   ";if(window.console)console.log('CEFOV:" +
                   std::string(why) + "|'+__r);})();";
  frame->ExecuteJavaScript(CefString(js), frame->GetURL(), 0);
}

static void InjectImageIntoComposer(const char* pngPath);
static void PasteFromClipboard();

// ---------------------------------------------------------------- image injection
static std::string Base64Data(const BYTE* data, size_t n) {
  if (!data || !n) return "";
  static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((n + 2) / 3 * 4);
  for (size_t i = 0; i < n; i += 3) {
    unsigned v = (unsigned)data[i] << 16;
    if (i + 1 < n) v |= (unsigned)data[i + 1] << 8;
    if (i + 2 < n) v |= (unsigned)data[i + 2];
    out += B64[(v >> 18) & 63];
    out += B64[(v >> 12) & 63];
    out += (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
    out += (i + 2 < n) ? B64[v & 63] : '=';
  }
  return out;
}
static std::string Base64File(const char* path) {
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f) return "";
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0) { fclose(f); return ""; }
  std::string raw((size_t)n, '\0');
  size_t rd = fread(&raw[0], 1, (size_t)n, f);
  fclose(f);
  if (rd != (size_t)n) return "";
  return Base64Data((const BYTE*)raw.data(), (size_t)n);
}

// Dispatch a synthetic image paste into the ChatGPT composer so a screenshot
// lands in the input box as an attachment (same strategy as the Ultralight
// overlay: DataTransfer shim + ClipboardEvent, dispatched on the composer).
static void InjectImageIntoComposer(const char* pngPath) {
  std::string b64 = Base64File(pngPath);
  if (b64.empty()) { Log("[inject] could not read %s", pngPath); return; }
  std::string js =
      "(function(b64){var log=[];"
      "try{"
      "var bin=atob(b64);var arr=new Uint8Array(bin.length);"
      "for(var i=0;i<bin.length;i++)arr[i]=bin.charCodeAt(i);"
      "var file=new File([new Blob([arr],{type:'image/png'})],'overlay-screenshot.png',{type:'image/png'});"
      "log.push('file:'+file.size);"
      "}catch(e){return 'build-file-failed:'+e;}"
      "var t=null;"
      "try{"
      "function __ovDq(root,sel){if(root.querySelector){var r=root.querySelector(sel);if(r)return r;}"
      "var els=root.querySelectorAll?root.querySelectorAll('*'):[];"
      "for(var i=0;i<els.length;i++){if(els[i].shadowRoot){var r=__ovDq(els[i].shadowRoot,sel);if(r)return r;}}"
      "return null;}"
      "function __ovFind(){"
      "var t=__ovDq(document,'#prompt-textarea');"
      "if(!t)t=__ovDq(document,'[data-testid=\"composer-text-input\"]');"
      "if(!t){var ce=__ovDq(document,'[contenteditable=\"true\"]');if(ce&&(ce.offsetParent||ce.getClientRects().length))t=ce;}"
      "if(!t&&document.activeElement&&document.activeElement!==document.body)t=document.activeElement;"
      "return t;}"
      "t=__ovFind();"
      "if(!t||t===document.body)return 'no-target';"
      "t.focus();"
      "log.push('target:'+t.tagName);"
      "}catch(e){return 'target-failed:'+e;}"
      "function makeCD(dt){"
      "if(dt&&dt.items&&dt.items.add){try{dt.items.add(file);log.push('dt-add:ok');return dt;}catch(e){log.push('dt-add:fail');}}"
      "return {files:[file],types:['Files'],"
      "items:{length:1,0:{kind:'file',type:'image/png',getAsFile:function(){return file;}},add:function(){},clear:function(){}},"
      "getData:function(){return '';},setData:function(){return false}};"
      "}"
      "var cd=null;"
      "try{cd=makeCD(new DataTransfer());}catch(e){log.push('dt-ctor:fail');cd=makeCD(null);}"
      "var ev=null;"
      "try{"
      "ev=new ClipboardEvent('paste',{bubbles:true,cancelable:true,clipboardData:cd});"
      "if(!ev.clipboardData){ev=new Event('paste',{bubbles:true,cancelable:true});"
      "Object.defineProperty(ev,'clipboardData',{value:cd});log.push('ce:ctor-dropped-cd');}"
      "else log.push('ce:ctor-ok');"
      "}catch(e){"
      "ev=new Event('paste',{bubbles:true,cancelable:true});"
      "try{Object.defineProperty(ev,'clipboardData',{value:cd});log.push('ce:define');}"
      "catch(e2){ev.clipboardData=cd;log.push('ce:assign');}"
      "}"
      "var accepted=t.dispatchEvent(ev);"
      "log.push('dispatched:'+(accepted?'default':'prevented'));"
      "return log.join('|');"
      "})('";
  js += b64;
  js += "')";
  ExecJs(js, "inject");
}

static std::string JsQuote(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:   out += c;
    }
  }
  return out;
}

// Ctrl+V over the overlay (chord from the low-level hook): text goes in via
// execCommand insertText on the composer; a clipboard IMAGE becomes a composer
// file attachment (InjectImageIntoComposer).
static void PasteFromClipboard() {
  if (!OpenClipboard(nullptr)) return;
  HANDLE h = GetClipboardData(CF_UNICODETEXT);
  if (h) {
    const wchar_t* w = (const wchar_t*)GlobalLock(h);
    if (w) {
      std::string text = ToUtf8(w);
      GlobalUnlock(h);
      CloseClipboard();
      std::string js =
          "(function(t){"
          "function __ovDq(root,sel){if(root.querySelector){var r=root.querySelector(sel);if(r)return r;}"
          "var els=root.querySelectorAll?root.querySelectorAll('*'):[];"
          "for(var i=0;i<els.length;i++){if(els[i].shadowRoot){var r=__ovDq(els[i].shadowRoot,sel);if(r)return r;}}"
          "return null;}"
          "var el=__ovDq(document,'#prompt-textarea');"
          "if(!el)el=__ovDq(document,'[data-testid=\"composer-text-input\"]');"
          "if(!el){var ce=__ovDq(document,'[contenteditable=\"true\"]');if(ce&&(ce.offsetParent||ce.getClientRects().length))el=ce;}"
          "if(!el&&document.activeElement&&document.activeElement!==document.body)el=document.activeElement;"
          "if(!el)return 'no-target';el.focus();"
          "var ok=false;"
          "try{ok=document.execCommand('insertText',false,t);}catch(e){}"
          "if(!ok){el.textContent+=t;}"
          "el.dispatchEvent(new Event('input',{bubbles:true}));"
          "return 'ok:'+t.length;})(\"" + JsQuote(text) + "\")";
      ExecJs(js, "paste-text");
      Log("[paste] injected %zu chars", text.size());
      return;
    }
    GlobalUnlock(h);
    CloseClipboard();
    return;
  }
  HANDLE hd = GetClipboardData(CF_DIB);
  if (hd) {
    auto* bih = (BITMAPINFOHEADER*)GlobalLock(hd);
    if (bih && bih->biBitCount == 32 && bih->biCompression == BI_RGB &&
        bih->biWidth > 0 && bih->biHeight != 0) {
      int w = (int)bih->biWidth;
      int hgt = bih->biHeight < 0 ? (int)-bih->biHeight : (int)bih->biHeight;
      const BYTE* bits = (const BYTE*)(bih + 1);
      int stride = w * 4;
      std::vector<BYTE> topdown;
      const BYTE* src = bits;
      if (bih->biHeight > 0) {   // bottom-up -> flip
        topdown.resize((size_t)w * hgt * 4);
        for (int y = 0; y < hgt; ++y)
          memcpy(&topdown[(size_t)y * stride], bits + (size_t)(hgt - 1 - y) * stride, (size_t)stride);
        src = topdown.data();
      }
      // Save through GDI+ (encode BGRA -> PNG).
      char tmp[MAX_PATH * 2];
      snprintf(tmp, sizeof(tmp), "%s\\clipboard-image.png", g_dataDir.c_str());
      bool ok = false;
      {
        EnsureGdiPlus();
        Gdiplus::Bitmap enc(w, hgt, stride, PixelFormat32bppPARGB,
                            const_cast<BYTE*>(src));
        CLSID pngClsid;
        UINT num = 0, sz = 0;
        Gdiplus::GetImageEncodersSize(&num, &sz);
        std::vector<char> buf(sz ? sz : 1);
        Gdiplus::GetImageEncoders(num, sz, (Gdiplus::ImageCodecInfo*)buf.data());
        for (UINT i = 0; i < num; ++i) {
          auto* info = (Gdiplus::ImageCodecInfo*)buf.data() + i;
          if (wcscmp(info->MimeType, L"image/png") == 0) {
            pngClsid = info->Clsid;
            ok = enc.Save(ToWide(tmp).c_str(), &pngClsid, nullptr) == Gdiplus::Ok;
            break;
          }
        }
      }
      GlobalUnlock(hd);
      CloseClipboard();
      if (ok) InjectImageIntoComposer(tmp);
      else Log("[paste] image: PNG encode failed");
      return;
    }
    if (bih) GlobalUnlock(hd);
  }
  CloseClipboard();
  Log("[paste] clipboard had neither text nor a 32-bit bitmap");
}

// ---------------------------------------------------------------- capture-affinity stripper
// SetWindowDisplayAffinity only accepts HWNDs owned by the calling process,
// so clearing another process's window affinity cross-process is rejected.
// This runs the call from INSIDE the target process via a remote thread:
// alloc executable mem (VirtualAllocEx + WriteProcessMemory), write a minimal
// x64 stub (RCX=HWND, EDX=WDA_NONE(0), RAX=resolved SetWindowDisplayAffinity,
// call rax; ret), resolving the address ASLR-correctly via RVA (our user32
// base vs target's user32 base from EnumProcessModules), then
// CreateRemoteThread. BOOL result comes back as the thread exit code.
// Re-run periodically: targets recreate windows / re-apply affinity.
namespace affinity {

static uint64_t RvaOfSetWindowDisplayAffinity() {
  HMODULE localUser32 = GetModuleHandleW(L"user32.dll");
  if (!localUser32) return 0;
  FARPROC fn = GetProcAddress(localUser32, "SetWindowDisplayAffinity");
  if (!fn) return 0;
  return (uint64_t)(uintptr_t)fn - (uint64_t)(uintptr_t)localUser32;
}

static uint64_t RemoteUser32Base(HANDLE hProc) {
  HMODULE mods[256];
  DWORD needed = 0;
  if (!EnumProcessModules(hProc, mods, sizeof(mods), &needed)) return 0;
  DWORD count = needed / sizeof(HMODULE);
  char name[MAX_PATH];
  for (DWORD i = 0; i < count; ++i) {
    if (GetModuleBaseNameA(hProc, mods[i], name, sizeof(name)) &&
        _stricmp(name, "user32.dll") == 0)
      return (uint64_t)(uintptr_t)mods[i];
  }
  return 0;
}

// x64 stub with proper ABI: sub rsp,0x28 (32-byte shadow + 8 align) |
// mov rcx,hwnd | xor edx,edx | mov rax,fn | call rax | add rsp,0x28 | ret.
// The old 24-byte stub called without shadow space / stack alignment and
// crashed the target (0xC0000005) — that killed client init outright.
// 32 bytes total. Exit code (RAX) = BOOL result.
static void BuildStub(BYTE out[32], HWND hwnd, uint64_t fnAddr) {
  BYTE* p = out;
  *p++ = 0x48; *p++ = 0x83; *p++ = 0xEC; *p++ = 0x28; // sub rsp, 0x28
  *p++ = 0x48; *p++ = 0xB9;                           // mov rcx, imm64
  memcpy(p, &hwnd, 8); p += 8;
  *p++ = 0x31; *p++ = 0xD2;                           // xor edx, edx (WDA_NONE=0)
  *p++ = 0x48; *p++ = 0xB8;                           // mov rax, imm64
  memcpy(p, &fnAddr, 8); p += 8;
  *p++ = 0xFF; *p++ = 0xD0;                           // call rax
  *p++ = 0x48; *p++ = 0x83; *p++ = 0xC4; *p++ = 0x28; // add rsp, 0x28
  *p++ = 0xC3;                                        // ret
}

static bool ClearOne(HWND hwnd) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (!pid || pid == GetCurrentProcessId()) {
    // Own window: direct call is fine.
    return SetWindowDisplayAffinity(hwnd, WDA_NONE) == TRUE;
  }
  HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                             PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                             FALSE, pid);
  if (!hProc) { Log("[affinity] OpenProcess(%lu) failed %lu", pid, GetLastError()); return false; }
  bool ok = false;
  uint64_t rva = RvaOfSetWindowDisplayAffinity();
  uint64_t remoteBase = rva ? RemoteUser32Base(hProc) : 0;
  if (!rva || !remoteBase) {
    Log("[affinity] resolve failed rva=0x%llx base=0x%llx", rva, remoteBase);
  } else {
    uint64_t fnAddr = remoteBase + rva;
    BYTE stub[32];
    BuildStub(stub, hwnd, fnAddr);
    LPVOID remote = VirtualAllocEx(hProc, nullptr, sizeof(stub),
                                   MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remote) {
      Log("[affinity] VirtualAllocEx failed %lu", GetLastError());
    } else {
      SIZE_T written = 0;
      if (!WriteProcessMemory(hProc, remote, stub, sizeof(stub), &written) ||
          written != sizeof(stub)) {
        Log("[affinity] WriteProcessMemory failed %lu", GetLastError());
      } else {
        FlushInstructionCache(hProc, remote, sizeof(stub));
        HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
                                            (LPTHREAD_START_ROUTINE)remote,
                                            nullptr, 0, nullptr);
        if (!hThread) {
          Log("[affinity] CreateRemoteThread failed %lu", GetLastError());
        } else {
          DWORD wait = WaitForSingleObject(hThread, 5000);
          DWORD exitCode = 0;
          if (wait == WAIT_OBJECT_0 && GetExitCodeThread(hThread, &exitCode) && exitCode != 0) {
            ok = true;
            Log("[affinity] cleared %p pid=%lu (exit=%lu)", (void*)hwnd, pid, exitCode);
          } else {
            // exit 0xC0000005 = stub crashed target; exit 0 = rejected (wrong
            // thread/window). Log, don't retry blindly — a crash-loop kills init.
            Log("[affinity] remote call failed hwnd=%p wait=%lu exit=0x%lx err=%lu",
                (void*)hwnd, wait, exitCode, GetLastError());
          }
          CloseHandle(hThread);
        }
      }
      VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    }
  }
  CloseHandle(hProc);
  return ok;
}

struct SweepCtx { DWORD pid; int cleared; int attempted; ULONGLONG procStart; };

// Sweep only stable, visible, real-size top-level windows. The old version
// hit EVERY window incl. splash/invisible/startup windows — injecting a
// thread during client init crashed it ("failed to start a new session").
// Now: visible only, >=250x250, skip splash/taskbar titles, skip processes
// younger than 15s (let init finish first).
static BOOL CALLBACK SweepProc(HWND hwnd, LPARAM lParam) {
  auto* ctx = (SweepCtx*)lParam;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != ctx->pid) return TRUE;
  if (GetWindow(hwnd, GW_OWNER)) return TRUE;
  if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) return TRUE;
  if (!IsWindowVisible(hwnd)) return TRUE;
  RECT rc; GetWindowRect(hwnd, &rc);
  LONG w = rc.right - rc.left, h = rc.bottom - rc.top;
  if (w < 250 || h < 250) return TRUE;
  wchar_t title[256] = {};
  GetWindowTextW(hwnd, title, 256);
  if (_wcsicmp(title, L"SplashScreen") == 0 || _wcsicmp(title, L"Taskbar") == 0)
    return TRUE;
  ctx->attempted++;
  if (ClearOne(hwnd)) ctx->cleared++;
  return TRUE;
}

static DWORD PidOf(const wchar_t* exeName) {
  DWORD pids[1024], needed = 0;
  if (!EnumProcesses(pids, sizeof(pids), &needed)) return 0;
  DWORD n = needed / sizeof(DWORD);
  for (DWORD i = 0; i < n; ++i) {
    if (!pids[i]) continue;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[i]);
    if (!h) continue;
    wchar_t path[MAX_PATH] = {};
    DWORD len = MAX_PATH;
    bool match = false;
    if (QueryFullProcessImageNameW(h, 0, path, &len)) {
      const wchar_t* nm = wcsrchr(path, L'\\');
      nm = nm ? nm + 1 : path;
      match = _wcsicmp(nm, exeName) == 0;
    }
    CloseHandle(h);
    if (match) return pids[i];
  }
  return 0;
}

// Inject AffHook.dll into the named exe via QueueUserAPC (SEBExploit-style).
// Why APC and not CreateRemoteThread / SetWindowsHookEx:
//   - CreateRemoteThread shellcode = hijacked native thread in a CLR process
//     -> 0xC0000005 -> session dies. Proven twice in our own logs.
//   - WH_GETMESSAGE hook = native code on the UI thread, still foreign to
//     the CLR's expectations -> same crash.
//   - QueueUserAPC(LoadLibraryW, tid, dllPath) queues the load to run when
//     the target thread enters an alertable wait — a normal, CLR-safe point.
//     The DLL then hosts the CLR itself (v4.0.30319) and runs the managed
//     AffManaged.dll helper, which P/Invokes SetWindowDisplayAffinity on its
//     OWN windows. Ordinary managed call, ordinary thread: survives.
//     Runtime-only: no disk patch, BEK/config keys unchanged.
// Injects once per pid (tracked at namespace scope so Watchdog can re-arm).
static DWORD g_injectedPid = 0;
static int SweepProcess(const wchar_t* exeName) {
  DWORD pid = PidOf(exeName);
  if (!pid) { g_injectedPid = 0; return -1; }
  if (pid == g_injectedPid) return 1;

  // Inject as soon as the main window exists (fast path) with a 3s process-age
  // floor (init protection). The old 8s age gate delayed first screenshots
  // ~5s; the native-sweep path has proven crash-free, so 3s is safe.
  bool mainWindowUp = false;
  struct WCtx { DWORD pid; bool found; };
  WCtx wctx = { pid, false };
  EnumWindows([](HWND hwnd, LPARAM l) -> BOOL {
    auto* c = (WCtx*)l;
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid != c->pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER)) return TRUE;
    if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) return TRUE;
    RECT rc; GetWindowRect(hwnd, &rc);
    if (rc.right - rc.left < 250 || rc.bottom - rc.top < 250) return TRUE;
    c->found = true;
    return FALSE;
  }, (LPARAM)&wctx);
  mainWindowUp = wctx.found;

  HANDLE hq = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  ULONGLONG ageSec = 999;
  if (hq) {
    FILETIME c, e, k, u;
    if (GetProcessTimes(hq, &c, &e, &k, &u)) {
      ULARGE_INTEGER ct; ct.LowPart = c.dwLowDateTime; ct.HighPart = c.dwHighDateTime;
      ULARGE_INTEGER now; GetSystemTimeAsFileTime((FILETIME*)&now);
      ageSec = (now.QuadPart - ct.QuadPart) / 10000000ULL;
    }
    CloseHandle(hq);
  }
  if (ageSec < 3 || !mainWindowUp) return -2;

  // Our DLL path (both AffHook.dll + AffManaged.dll live next to the exe).
  wchar_t dllPath[MAX_PATH];
  GetModuleFileNameW(nullptr, dllPath, MAX_PATH);
  wchar_t* slash = wcsrchr(dllPath, L'\\');
  if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - dllPath), L"AffHook.dll");
  else wcscpy_s(dllPath, L"AffHook.dll");
  if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
    Log("[affinity] AffHook.dll missing next to exe");
    return 0;
  }

  // Queue the APC to EVERY thread of the target, not just the first one:
  // a single tid may never enter an alertable wait, so the DLL never loads.
  // One shared remote path allocation, APC on each thread (same as batch
  // injectors). DllMain runs once per process regardless.
  HANDLE hProc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE, FALSE, pid);
  if (!hProc) { Log("[affinity] OpenProcess(%lu) failed %lu", pid, GetLastError()); return 0; }
  size_t bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
  LPVOID remote = VirtualAllocEx(hProc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  int queued = 0;
  if (remote && WriteProcessMemory(hProc, remote, dllPath, bytes, nullptr)) {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC llw = k32 ? GetProcAddress(k32, "LoadLibraryW") : nullptr;
    if (llw) {
      HANDLE snap2 = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      if (snap2 != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te = { sizeof(te) };
        if (Thread32First(snap2, &te)) do {
          if (te.th32OwnerProcessID != pid) continue;
          HANDLE ht = OpenThread(THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
          if (!ht) continue;
          if (QueueUserAPC((PAPCFUNC)llw, ht, (ULONG_PTR)remote)) queued++;
          CloseHandle(ht);
        } while (Thread32Next(snap2, &te));
        CloseHandle(snap2);
      }
    }
    if (!queued)
      Log("[affinity] QueueUserAPC failed on all threads (%lu)", GetLastError());
  }
  // NOTE: do NOT VirtualFreeEx here — target loads async after we return.
  CloseHandle(hProc);
  if (queued) {
    g_injectedPid = pid;
    Log("[affinity] APC queued x%d LoadLibraryW(AffHook.dll) into pid=%lu", queued, pid);
    return 1;
  }
  return 0;
}

// Watchdog: GetWindowDisplayAffinity is READABLE cross-process. If the main
// window shows protection again (target re-applied / new window), re-arm so
// SweepProcess queues a fresh APC round. Called from host::Tick alongside
// SweepProcess.
static void Rearm() { g_injectedPid = 0; }

static void Watchdog(const wchar_t* exeName) {
  DWORD pid = PidOf(exeName);
  if (!pid || pid != g_injectedPid) return;   // nothing injected yet
  struct DCtx { DWORD pid; HWND best; LONG area; };
  DCtx d = { pid, nullptr, 0 };
  EnumWindows([](HWND hwnd, LPARAM l) -> BOOL {
    auto* c = (DCtx*)l;
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid != c->pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER)) return TRUE;
    if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) return TRUE;
    RECT rc; GetWindowRect(hwnd, &rc);
    LONG w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w < 250 || h < 250) return TRUE;
    if (w * h > c->area) { c->area = w * h; c->best = hwnd; }
    return TRUE;
  }, (LPARAM)&d);
  if (!d.best) return;
  DWORD aff = WDA_NONE;
  if (GetWindowDisplayAffinity(d.best, &aff) && aff != WDA_NONE) {
    Log("[affinity] re-protected detected (aff=0x%lx) — re-arming APC", aff);
    Rearm();
  }
}

}  // namespace affinity

// ---------------------------------------------------------------- screenshot
// Find SEB's client window for the PrintWindow capture layer (defined here
// rather than reusing host::FindWindow, which is declared later in the file).
struct CapFindCtx { HWND best; LONG bestArea; };
static BOOL CALLBACK CapFindProc(HWND hwnd, LPARAM lParam) {
  auto* ctx = (CapFindCtx*)lParam;
  if (!IsWindowVisible(hwnd)) return TRUE;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  bool match = false;
  if (h) {
    wchar_t path[MAX_PATH] = {};
    DWORD len = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, path, &len)) {
      const wchar_t* n = wcsrchr(path, L'\\');
      n = n ? n + 1 : path;
      match = _wcsicmp(n, L"SafeExamBrowser.Client.exe") == 0;
    }
    CloseHandle(h);
  }
  if (!match) return TRUE;
  RECT rc;
  GetWindowRect(hwnd, &rc);
  LONG area = (rc.right - rc.left) * (rc.bottom - rc.top);
  if (area > ctx->bestArea) { ctx->bestArea = area; ctx->best = hwnd; }
  return TRUE;
}
static HWND FindSebForCapture() {
  CapFindCtx ctx = {};
  EnumWindows(CapFindProc, (LPARAM)&ctx);
  return ctx.best;
}

// PrintWindow on a capture-protected or D3D-composited window often yields a
// uniform black/grey buffer. Treat "more than 5% of sampled pixels differ
// from the first" as real content.
static bool DibHasContent(const BYTE* bgra, int w, int h) {
  DWORD first = *(const DWORD*)bgra & 0xFFFFFF;
  int samples = 0, different = 0;
  for (int y = 0; y < h; y += 16) {
    for (int x = 0; x < w; x += 16) {
      DWORD p = *(const DWORD*)(bgra + (size_t)y * w * 4 + (size_t)x * 4) & 0xFFFFFF;
      ++samples;
      if (p != first) ++different;
    }
  }
  return samples > 0 && different * 20 > samples;
}

// Capture what's BEHIND the overlay (SEB / desktop), not our own page DIB.
// Layer 1 is a full-screen GDI grab: the overlay stays fully visible on the
// real screen throughout and is kept out of the capture by its permanent
// WDA_EXCLUDEFROMCAPTURE (DWM excludes it from the capture surface — no
// hiding, no flicker). Layer 2 handles SEB's own capture protection: with
// "Allow screen capture" disabled, SEB marks its window excluded, so layer 1
// only shows the empty desktop behind it — PrintWindow asks the window to
// render into our DC directly, bypassing the DWM capture-surface filter.
static void ScreenshotNow() {
  if (!g_hwnd) return;
  HDC screen = GetDC(nullptr);
  if (!screen) return;
  // Full VIRTUAL screen (all monitors) — the "entire background".
  int x0 = GetSystemMetrics(SM_XVIRTUALSCREEN);
  int y0 = GetSystemMetrics(SM_YVIRTUALSCREEN);
  int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (w <= 0) w = GetSystemMetrics(SM_CXSCREEN);
  if (h <= 0) h = GetSystemMetrics(SM_CYSCREEN);

  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;    // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  HDC mem = CreateCompatibleDC(screen);
  void* pv = nullptr;
  HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &pv, nullptr, 0);
  if (!dib) {
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    Log("[screenshot] DIB alloc failed");
    return;
  }
  HBITMAP old = (HBITMAP)SelectObject(mem, dib);

  // Layer 1: full-screen grab. The overlay is NOT hidden — its permanent
  // WDA_EXCLUDEFROMCAPTURE keeps it out of the capture surface.
  BitBlt(mem, 0, 0, w, h, screen, x0, y0, SRCCOPY);
  ReleaseDC(nullptr, screen);

  // Layer 2: SEB's window via PrintWindow (its affinity exclusion does not
  // apply to the window rendering itself into a DC).
  if (HWND seb = FindSebForCapture()) {
    RECT srb;
    GetWindowRect(seb, &srb);
    int sw = srb.right - srb.left, sh = srb.bottom - srb.top;
    if (sw > 100 && sh > 100) {
      BITMAPINFO sbmi = {};
      sbmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      sbmi.bmiHeader.biWidth = sw;
      sbmi.bmiHeader.biHeight = -sh;
      sbmi.bmiHeader.biPlanes = 1;
      sbmi.bmiHeader.biBitCount = 32;
      sbmi.bmiHeader.biCompression = BI_RGB;
      HDC sdcScreen = GetDC(nullptr);
      HDC pdc = CreateCompatibleDC(sdcScreen);
      void* spv = nullptr;
      HBITMAP sdib = CreateDIBSection(sdcScreen, &sbmi, DIB_RGB_COLORS, &spv, nullptr, 0);
      ReleaseDC(nullptr, sdcScreen);
      if (sdib) {
        HBITMAP sold = (HBITMAP)SelectObject(pdc, sdib);
        if (PrintWindow(seb, pdc, PW_RENDERFULLCONTENT) && DibHasContent((const BYTE*)spv, sw, sh)) {
          int dx = srb.left - x0, dy = srb.top - y0;
          int cx0 = dx < 0 ? -dx : 0, cy0 = dy < 0 ? -dy : 0;
          int bx = dx > 0 ? dx : 0, by = dy > 0 ? dy : 0;
          int cw = sw - cx0, ch = sh - cy0;
          if (cw > w - bx) cw = w - bx;
          if (ch > h - by) ch = h - by;
          if (cw > 0 && ch > 0)
            BitBlt(mem, bx, by, cw, ch, pdc, cx0, cy0, SRCCOPY);
          Log("[screenshot] SEB window composited via PrintWindow %dx%d at %d,%d",
              sw, sh, dx, dy);
        } else {
          Log("[screenshot] PrintWindow yielded no content (SEB capture-protected)");
        }
        SelectObject(pdc, sold);
        DeleteObject(sdib);
      }
      DeleteDC(pdc);
    }
  }

  std::vector<BYTE> bits((size_t)w * h * 4);
  memcpy(bits.data(), pv, (size_t)w * h * 4);
  SelectObject(mem, old);
  DeleteObject(dib);
  DeleteDC(mem);

  // Screen BitBlt leaves the alpha byte zero; the capture is opaque.
  DWORD* px = (DWORD*)bits.data();
  for (size_t i = 0, n = (size_t)w * h; i < n; ++i) px[i] |= 0xFF000000;
  std::string dir = g_dataDir + "\\screenshots";
  EnsureDir(dir);
  SYSTEMTIME st; GetLocalTime(&st);
  char path[MAX_PATH * 2];
  snprintf(path, sizeof(path),
           "%s\\overlay-%04d%02d%02d-%02d%02d%02d.png", dir.c_str(),
           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

  bool saved = false;
  {
    EnsureGdiPlus();
    Gdiplus::Bitmap enc(w, h, w * 4, PixelFormat32bppPARGB, bits.data());
    CLSID pngClsid;
    UINT num = 0, sz = 0;
    Gdiplus::GetImageEncodersSize(&num, &sz);
    std::vector<char> buf(sz ? sz : 1);
    Gdiplus::GetImageEncoders(num, sz, (Gdiplus::ImageCodecInfo*)buf.data());
    for (UINT i = 0; i < num; ++i) {
      auto* info = (Gdiplus::ImageCodecInfo*)buf.data() + i;
      if (wcscmp(info->MimeType, L"image/png") == 0) {
        pngClsid = info->Clsid;
        saved = enc.Save(ToWide(path).c_str(), &pngClsid, nullptr) == Gdiplus::Ok;
        break;
      }
    }
  }
  if (!saved) {
    Log("[screenshot] PNG encode FAILED");
    return;
  }

  // Clipboard DIB copy so the shot is immediately Ctrl+V-able into any app.
  bool clipped = false;
  {
    SIZE_T dibSize = sizeof(BITMAPINFOHEADER) + (SIZE_T)w * h * 4;
    if (HGLOBAL g = GlobalAlloc(GHND | GMEM_SHARE, dibSize)) {
      auto* bi = (BITMAPINFOHEADER*)GlobalLock(g);
      bi->biSize = sizeof(BITMAPINFOHEADER);
      bi->biWidth = w;
      bi->biHeight = -h;
      bi->biPlanes = 1;
      bi->biBitCount = 32;
      bi->biCompression = BI_RGB;
      bi->biSizeImage = (DWORD)((SIZE_T)w * h * 4);
      memcpy(bi + 1, bits.data(), (size_t)w * h * 4);
      GlobalUnlock(g);
      if (OpenClipboard(g_hwnd)) {
        EmptyClipboard();
        clipped = SetClipboardData(CF_DIB, g) != nullptr;
        CloseClipboard();
      }
      if (!clipped) GlobalFree(g);
    }
  }

  AddThumb(bits.data(), w, h, path);
  Log("[screenshot] saved -> %s (clipboard: %s)", path, clipped ? "copied" : "no");
}

// Shared drop handler: attach the dragged thumbnail's PNG to the ChatGPT
// composer if released over the page area (below the header, left of the
// sidebar). Coordinates are window px.
static void ThumbDropOrCancel(int wx, int wy, int clientW) {
  int idx = g_thumbDrag;
  g_thumbDrag = -1;
  ghost::Hide();
  InvalidateRect(g_hwnd, nullptr, FALSE);
  ComposeAndPresent();   // erase the ghost from the composite
  bool overPage = wx < SidebarLeft(clientW) && wy >= kHeader;
  Log("[thumb] drop at %d,%d idx=%d %s", wx, wy, idx,
      overPage ? "-> inject" : "-> cancel");
  if (idx < 0 || idx >= (int)g_thumbs.size()) return;
  if (overPage)
    InjectImageIntoComposer(g_thumbs[idx].path.c_str());
}

// ---------------------------------------------------------------- toggle
static bool g_hooksUp = false;
static bool g_routing = false;

static void SetVisible(bool visible);   // fwd (router section below)

static void DoToggle() {
  if (!g_hwnd) return;
  if (IsWindowVisible(g_hwnd)) {
    ShowWindow(g_hwnd, SW_HIDE);
    SetVisible(false);
    Log("[toggle] hidden");
  } else {
    ShowWindow(g_hwnd, SW_SHOW);
    SetVisible(true);
    HWND after = GetParent(g_hwnd) ? HWND_TOP : (g_topmost ? HWND_TOPMOST : HWND_TOP);
    SetWindowPos(g_hwnd, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    InvalidateRect(g_hwnd, nullptr, FALSE);
    ComposeAndPresent();
    Log("[toggle] visible (child: %s)", GetParent(g_hwnd) ? "yes" : "no");
  }
}

// ============================================================================
// Input router (low-level hooks on a dedicated thread)
// ============================================================================
namespace router {

constexpr UINT kRouterMsg  = WM_APP + 20;  // mouse event delivery
constexpr UINT kChordToggle = WM_APP + 21; // Alt+`
constexpr UINT kChordShot   = WM_APP + 22; // Ctrl+Alt+S
constexpr UINT kChordQuit   = WM_APP + 23; // Ctrl+Alt+Q
constexpr UINT kChordPaste  = WM_APP + 26; // Ctrl+V over the overlay

struct RouterEvent { unsigned msg; int x, y, data; };

static HANDLE g_thread = nullptr;
static HANDLE g_startedEvent = nullptr;
static DWORD g_threadId = 0;
static HHOOK g_mouseHook = nullptr;
static HHOOK g_kbHook = nullptr;

static SRWLOCK g_lock = SRWLOCK_INIT;
static RECT g_bounds = {};
static bool g_visible = false;

// The native cursor stays VISIBLE (same as the final Ultralight overlay) —
// all input still routes through the hooks, but the user keeps normal cursor
// feedback.

void UpdateBounds() {
  RECT rc;
  if (g_hwnd && GetWindowRect(g_hwnd, &rc)) {
    AcquireSRWLockExclusive(&g_lock);
    g_bounds = rc;
    ReleaseSRWLockExclusive(&g_lock);
  }
}

static bool IsVisible() {
  AcquireSRWLockShared(&g_lock);
  bool v = g_visible;
  ReleaseSRWLockShared(&g_lock);
  return v;
}

static bool Inside(int x, int y) {
  RECT rc;
  AcquireSRWLockShared(&g_lock);
  rc = g_bounds;
  ReleaseSRWLockShared(&g_lock);
  return PtInRect(&rc, { x, y }) == TRUE;
}

// Gesture ownership: a gesture belongs to the destination where its
// button-down began until every button is released.
static WPARAM g_owned = 0;
static WPARAM g_external = 0;
static int g_failStreak = 0;

static void ResetOwnership() { g_owned = 0; g_external = 0; }

static bool Post(unsigned msg, int x, int y, int data, bool critical) {
  RouterEvent* ev = (RouterEvent*)HeapAlloc(GetProcessHeap(), 0, sizeof(RouterEvent));
  if (!ev) return !critical;
  ev->msg = msg; ev->x = x; ev->y = y; ev->data = data;
  if (PostMessage(g_hwnd, kRouterMsg, 0, (LPARAM)ev)) {
    g_failStreak = 0;
    return true;
  }
  HeapFree(GetProcessHeap(), 0, ev);
  if (critical && ++g_failStreak >= 8) {
    Log("router: critical post failures — disabling routing (fail closed)");
    g_routing = false;
    ResetOwnership();
  }
  return false;
}

static WPARAM ButtonBit(unsigned msg) {
  switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: return 1;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: return 2;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: return 4;
  }
  return 0;
}

static LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam) {
  if (code < 0) return CallNextHookEx(g_mouseHook, code, wParam, lParam);
  auto* ms = (MSLLHOOKSTRUCT*)lParam;
  if (ms->flags & LLMHF_INJECTED)
    return CallNextHookEx(g_mouseHook, code, wParam, lParam);

  if (!g_routing || !IsVisible()) {
    ResetOwnership();
    return CallNextHookEx(g_mouseHook, code, wParam, lParam);
  }

  unsigned msg = (unsigned)wParam;
  int x = ms->pt.x, y = ms->pt.y;
  bool inside = Inside(x, y);

  // Dual delivery: a copy of every gesture over the overlay goes to the page
  // (posted below), while the ORIGINAL event is passed through untouched so
  // the window beneath (SEB) also receives it. Nothing is consumed — moves
  // were never consumed (that freezes the cursor at the border), and now
  // buttons/wheel fall through too.
  if (WPARAM bit = ButtonBit(msg)) {
    bool down = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN);
    if (down) {
      if (g_external == 0 && inside) {
        g_owned |= bit;
        Post(msg, x, y, 0, true);      // copy to the page
      } else {
        g_external |= bit;
      }
      return CallNextHookEx(g_mouseHook, code, wParam, lParam);
    }
    if (g_owned & bit) {               // button-up of a gesture we own
      Post(msg, x, y, 0, true);        // copy to the page
      g_owned &= ~bit;
      g_external &= ~bit;
    } else {
      g_external &= ~bit;
    }
    return CallNextHookEx(g_mouseHook, code, wParam, lParam);
  }

  if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
    if (g_owned || inside)
      Post(msg, x, y, (int)(SHORT)HIWORD(ms->mouseData), true);   // copy to the page
    return CallNextHookEx(g_mouseHook, code, wParam, lParam);
  }

  if (msg == WM_MOUSEMOVE) {
    // NEVER consume movement: eating WM_MOUSEMOVE in a low-level hook freezes
    // the physical cursor. Pass the move through and route a COPY.
    if (!g_external && (g_owned || inside))
      Post(msg, x, y, 0, false);
    return CallNextHookEx(g_mouseHook, code, wParam, lParam);
  }

  return CallNextHookEx(g_mouseHook, code, wParam, lParam);
}

static bool Held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static LRESULT CALLBACK KbProc(int code, WPARAM wParam, LPARAM lParam) {
  if (code < 0) return CallNextHookEx(g_kbHook, code, wParam, lParam);
  auto* ks = (KBDLLHOOKSTRUCT*)lParam;
  if (ks->flags & LLKHF_INJECTED)
    return CallNextHookEx(g_kbHook, code, wParam, lParam);

  if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
    bool ctrl = Held(VK_CONTROL), alt = Held(VK_MENU);
    UINT msg = 0;
    if (alt && !ctrl && ks->vkCode == VK_OEM_3) msg = kChordToggle;
    else if (ctrl && alt && ks->vkCode == 'S') msg = kChordShot;
    else if (ctrl && alt && ks->vkCode == 'Q') msg = kChordQuit;
    else if (ctrl && !alt && ks->vkCode == 'V' && g_routing && IsVisible()) {
      POINT pt;
      if (GetCursorPos(&pt) && Inside((int)pt.x, (int)pt.y)) msg = kChordPaste;
    }
    if (msg) {
      PostMessage(g_hwnd, msg, 0, 0);
      return 1;
    }
  }
  return CallNextHookEx(g_kbHook, code, wParam, lParam);
}

static unsigned __stdcall HookThread(void*) {
  g_threadId = GetCurrentThreadId();
  g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
  g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KbProc, GetModuleHandleW(nullptr), 0);
  if (!g_mouseHook || !g_kbHook)
    Log("router: hook installation failed (mouse=%p kb=%p err=%lu)",
        (void*)g_mouseHook, (void*)g_kbHook, GetLastError());
  if (g_startedEvent) SetEvent(g_startedEvent);
  MSG msg;
  while (GetMessage(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
  return 0;
}

static bool Start() {
  g_startedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!g_startedEvent) return false;
  g_thread = (HANDLE)_beginthreadex(nullptr, 0, HookThread, nullptr, 0, nullptr);
  if (!g_thread) {
    CloseHandle(g_startedEvent);
    g_startedEvent = nullptr;
    return false;
  }
  DWORD wait = WaitForSingleObject(g_startedEvent, 5000);
  CloseHandle(g_startedEvent);
  g_startedEvent = nullptr;
  if (wait != WAIT_OBJECT_0 || !g_mouseHook || !g_kbHook) {
    Log("router: start failed (wait=%lu hooks mouse=%p kb=%p)", wait,
        (void*)g_mouseHook, (void*)g_kbHook);
    return false;
  }
  UpdateBounds();
  return true;
}

static void Stop() {
  g_routing = false;
  AcquireSRWLockExclusive(&g_lock);
  g_visible = false;      // stop the z-watchdog re-showing us during teardown
  ReleaseSRWLockExclusive(&g_lock);
  ResetOwnership();
  cursor::Hide();
  if (g_thread) {
    if (g_threadId) PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = nullptr; g_threadId = 0;
  }
  if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
  if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }
}

}  // namespace router

// Window-drag state (defined here, before SetVisible needs to abort a drag).
static bool  g_dragging = false;
static POINT g_dragStart = {};
static RECT  g_dragWndStart = {};
static void DragBegin(int sx, int sy);
static void DragMove(int sx, int sy);
static void DragEnd();

// Called when the overlay becomes visible/hidden: drives routing only.
static void SetVisible(bool visible) {
  AcquireSRWLockExclusive(&router::g_lock);
  router::g_visible = visible;
  ReleaseSRWLockExclusive(&router::g_lock);
  g_routing = visible && g_hooksUp;
  if (visible) {
    router::UpdateBounds();
  } else {
    router::ResetOwnership();
    g_thumbDrag = -1;          // abort any in-flight screenshot drag
    g_dragging = false;
    cursor::Hide();
    ghost::Hide();
  }
}

// ============================================================================
// Desktop relaunch handoff (per OVERLAY_RELAUNCH_REPARENTING_GUIDE.md)
// ============================================================================
namespace handoff {

static bool g_isChild = false;
static HANDLE g_evReadyChild = nullptr;
static bool g_inProgress = false;
static std::string g_pendingDesktop;
static ULONGLONG g_pendingSince = 0;
static bool g_wasVisible = true;

static std::string DesktopNameOf(HDESK d) {
  if (!d) return "";
  wchar_t buf[256] = {};
  DWORD len = 0;
  if (!GetUserObjectInformationW(d, UOI_NAME, buf, sizeof(buf), &len)) return "";
  return ToUtf8(buf);
}
static std::string ThreadDesktopName() {
  return DesktopNameOf(GetThreadDesktop(GetCurrentThreadId()));
}
static std::string InputDesktopName() {
  HDESK d = OpenInputDesktop(0, FALSE, GENERIC_READ);
  if (!d) return "";
  std::string name = DesktopNameOf(d);
  CloseDesktop(d);
  return name;
}
static std::string StationName() {
  wchar_t buf[256] = {};
  DWORD len = 0;
  if (!GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, buf, sizeof(buf), &len))
    return "WinSta0";
  return ToUtf8(buf);
}
static bool IsSecureDesktopName(const std::string& name) {
  std::string n(name);
  for (auto& c : n) c = (char)std::tolower((unsigned char)c);
  return n.find("winlogon") != std::string::npos ||
         n.find("screen-saver") != std::string::npos ||
         n.find("disconnect") != std::string::npos;
}

static HANDLE OpenEv(const std::string& base, const char* suffix) {
  return OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE,
                    ToWide(base + "-" + suffix).c_str());
}
static HANDLE CreateEv(const std::string& base, const char* suffix) {
  return CreateEventW(nullptr, TRUE, FALSE, ToWide(base + "-" + suffix).c_str());
}

// --- Replacement side -------------------------------------------------------
// The old instance exits right after signaling "released"; the child must not
// CefInitialize until the old process is GONE, because CEF refuses to init a
// second browser using the same root_cache_path while the first is alive.
// |parentPid| lets us wait on the old process handle directly.
bool Join(const char* eventName, DWORD parentPid, bool waitForRelease) {
  g_isChild = true;
  HANDLE opened = OpenEv(eventName, "opened");
  HANDLE released = OpenEv(eventName, "released");
  HANDLE failed = OpenEv(eventName, "failed");
  g_evReadyChild = OpenEv(eventName, "ready");
  if (!opened || !released || !failed || !g_evReadyChild) {
    Log("handoff: could not open event group (%lu) — starting standalone",
        GetLastError());
    if (opened) CloseHandle(opened);
    if (released) CloseHandle(released);
    if (failed) CloseHandle(failed);
    if (g_evReadyChild) CloseHandle(g_evReadyChild);
    g_evReadyChild = nullptr;
    g_isChild = false;
    return true;
  }
  SetEvent(opened);
  Log("handoff: replacement acknowledged (opened) on desktop %s",
      ThreadDesktopName().c_str());
  if (!waitForRelease) {
    CloseHandle(opened); CloseHandle(released); CloseHandle(failed);
    return true;
  }
  HANDLE parent = nullptr;
  if (parentPid)
    parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
  if (parent) {
    HANDLE waits[3] = { released, failed, parent };
    DWORD r = WaitForMultipleObjects(3, waits, FALSE, 30000);
    if (r == WAIT_OBJECT_0 + 1)
      Log("handoff: old instance reported failure — proceeding anyway");
    else if (r == WAIT_OBJECT_0 + 2)
      Log("handoff: old instance process exited — cache lock free");
    else if (r == WAIT_OBJECT_0)
      Log("handoff: released by the previous desktop instance");
    else
      Log("handoff: release wait timed out (r=%lu) — proceeding anyway", r);
  } else {
    HANDLE waits[2] = { released, failed };
    DWORD r = WaitForMultipleObjects(2, waits, FALSE, 25000);
    if (r != WAIT_OBJECT_0)
      Log("handoff: release wait failed (r=%lu) — proceeding anyway", r);
    else
      Log("handoff: released by the previous desktop instance");
  }
  if (parent) CloseHandle(parent);
  CloseHandle(opened); CloseHandle(released); CloseHandle(failed);
  return true;
}

void SignalReady() {
  if (g_evReadyChild) {
    SetEvent(g_evReadyChild);
    CloseHandle(g_evReadyChild);
    g_evReadyChild = nullptr;
    Log("handoff: replacement ready");
  }
}

// --- Old process side -------------------------------------------------------
static void BeginHandoff(const std::string& targetDesktop) {
  g_inProgress = true;
  char baseBuf[128];
  snprintf(baseBuf, sizeof(baseBuf), "Local\\CefOv-%lu-%lu",
           (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
  std::string base = baseBuf;

  HANDLE opened = CreateEv(base, "opened");
  HANDLE released = CreateEv(base, "released");
  HANDLE failed = CreateEv(base, "failed");
  HANDLE ready = CreateEv(base, "ready");
  if (!opened || !released || !failed || !ready) {
    if (opened) CloseHandle(opened);
    if (released) CloseHandle(released);
    if (failed) CloseHandle(failed);
    if (ready) CloseHandle(ready);
    g_inProgress = false;
    return;
  }

  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  RECT rc;
  GetWindowRect(g_hwnd, &rc);
  g_wasVisible = IsWindowVisible(g_hwnd) == TRUE;

  std::string cmd = "\"" + ToUtf8(exe) + "\"";
  cmd += " --x " + std::to_string(rc.left);
  cmd += " --y " + std::to_string(rc.top);
  cmd += " --w " + std::to_string(rc.right - rc.left);
  cmd += " --h " + std::to_string(rc.bottom - rc.top);
  if (!g_wasVisible) cmd += " --hidden";
  cmd += " --handoff-event \"" + base + "\" --handoff-wait";
  cmd += " --handoff-parent " + std::to_string(GetCurrentProcessId());

  std::wstring wcmd = ToWide(cmd);
  std::wstring desktop = ToWide(StationName() + "\\" + targetDesktop);
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.lpDesktop = const_cast<LPWSTR>(desktop.c_str());
  PROCESS_INFORMATION pi = {};
  BOOL ok = CreateProcessW(exe, &wcmd[0], nullptr, nullptr, FALSE,
                           CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si, &pi);
  if (!ok) {
    Log("handoff: CreateProcess failed (%lu)", GetLastError());
    SetEvent(failed);
    CloseHandle(opened); CloseHandle(released); CloseHandle(failed); CloseHandle(ready);
    g_inProgress = false;
    return;
  }
  CloseHandle(pi.hThread);
  Log("handoff: replacement started pid=%lu desktop=%S", pi.dwProcessId, desktop.c_str());

  ShowWindow(g_hwnd, SW_HIDE);
  AcquireSRWLockExclusive(&router::g_lock);
  router::g_visible = false;
  ReleaseSRWLockExclusive(&router::g_lock);
  g_routing = false;
  router::ResetOwnership();
  ghost::Hide();

  DWORD r = WaitForSingleObject(opened, 15000);
  if (r != WAIT_OBJECT_0) {
    Log("handoff: replacement never acknowledged (r=%lu) — aborting", r);
    SetEvent(failed);
    CloseHandle(pi.hProcess);
    CloseHandle(opened); CloseHandle(released); CloseHandle(failed); CloseHandle(ready);
    ShowWindow(g_hwnd, SW_SHOW);
    SetVisible(true);
    g_inProgress = false;
    return;
  }

  // Nothing engine-side to free, and the child cannot CefInitialize while we
  // are alive (root_cache_path single-instance lock). Release it and exit
  // immediately — the child waits on our process handle.
  SetEvent(released);
  Log("handoff: complete — old instance exiting");
  CloseHandle(pi.hProcess);
  CloseHandle(opened); CloseHandle(released); CloseHandle(failed); CloseHandle(ready);
  PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

// 50 ms input-desktop watch with settle debounce and secure-desktop skip.
static void Watch() {
  if (g_isChild || g_inProgress || !g_hooksUp) return;

  std::string input = InputDesktopName();
  if (input.empty()) return;
  std::string current = ThreadDesktopName();
  if (current.empty()) return;

  if (input == current) { g_pendingDesktop.clear(); return; }
  if (input != g_pendingDesktop) {
    g_pendingDesktop = input;
    g_pendingSince = GetTickCount64();
    Log("desktop transition detected: input=%s current=%s",
        input.c_str(), current.c_str());
    return;
  }
  if (GetTickCount64() - g_pendingSince < 750) return;   // settle
  if (IsSecureDesktopName(input)) {
    Log("desktop %s is secure — pausing handoff", input.c_str());
    g_pendingDesktop.clear();
    return;
  }
  // No relaunch after the target closes: when its desktop tears down the
  // input desktop flips back, which used to spawn a broken child on Default
  // (no target = broken state + cache-lock fights). If the target process is
  // gone, drop the pending handoff and stay put.
  if (affinity::PidOf(L"SafeExamBrowser.Client.exe") == 0) {
    Log("handoff: target gone — staying, no relaunch");
    g_pendingDesktop.clear();
    return;
  }
  std::string target = g_pendingDesktop;
  g_pendingDesktop.clear();
  g_inProgress = true;
  std::string* arg = new std::string(target);
  HANDLE t = (HANDLE)_beginthreadex(nullptr, 0, [](void* p) -> unsigned {
    std::string* s = (std::string*)p;
    BeginHandoff(*s);
    delete s;
    return 0;
  }, arg, 0, nullptr);
  if (!t) {
    delete arg;
    g_inProgress = false;
    Log("handoff: worker thread spawn failed (%lu)", GetLastError());
  } else {
    CloseHandle(t);
  }
}

}  // namespace handoff

// ============================================================================
// SEB tracking + z-order (top-level window + watchdog approach)
//
// Reparenting the overlay INTO SEB as a child window was tried and REVERTED:
// a child is clipped to the parent's client area, fights the host's own
// z-order management, its layered/paint path breaks across the reparent, and
// SEB may hide foreign child windows — the overlay goes invisible / behind
// after relaunch. What works: the overlay stays a TOP-LEVEL layered window
// and simply out-ranks SEB in the topmost band. The 50 ms watchdog re-asserts
// HWND_TOPMOST (restoring the style if an external component removed it).
// Never activates, never shows a hidden window. SEB's client window is only
// TRACKED (diagnostics), never parented.
// ============================================================================
namespace host {

static HWND  g_sebWindow = nullptr;   // tracked only, never parented
static DWORD g_lastProbe = 0;
static bool  g_topmost = true;
static HWINEVENTHOOK g_zHook = nullptr;
static volatile LONG g_zDirty = 0;

void SetTopmost(bool enabled) { g_topmost = enabled; }

// SEB runs its own raise-window watchdog and will try to sit above us in the
// topmost band (when it wins, our overlay ends up BEHIND SEB's fullscreen
// window — "sent down", looks invisible). React to SEB's z-order changes
// immediately instead of only polling.
static void CALLBACK ZOrderProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                LONG idObject, LONG, DWORD, DWORD) {
  if (idObject != OBJID_WINDOW) return;
  // React to SEB reordering itself AND to our own window being hidden /
  // reordered from outside — SEB's "monitor processes" feature hides windows
  // of non-permitted processes outright, which reads as "sent down / just
  // invisible" even though the window is still topmost in z-order.
  if (g_hwnd && hwnd == g_hwnd &&
      (event == EVENT_OBJECT_HIDE || event == EVENT_OBJECT_REORDER ||
       event == EVENT_OBJECT_LOCATIONCHANGE))
    InterlockedExchange(&g_zDirty, 1);
  if (g_sebWindow && hwnd == g_sebWindow)
    InterlockedExchange(&g_zDirty, 1);
}

static void EnsureZHook() {
  if (g_zHook) return;
  g_zHook = SetWinEventHook(EVENT_OBJECT_HIDE, EVENT_OBJECT_REORDER, nullptr,
                            ZOrderProc, 0, 0,
                            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

static void EnsureTopmost();   // defined below

// Called from the message loop: re-raise the instant SEB reorders itself.
static void ConsumeZDirty() {
  if (InterlockedExchange(&g_zDirty, 0)) {
    EnsureTopmost();
    ghost::Raise();
  }
}

static bool IsTargetProcess(HWND hwnd) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return false;
  wchar_t path[MAX_PATH] = {};
  DWORD len = MAX_PATH;
  BOOL ok = QueryFullProcessImageNameW(h, 0, path, &len);
  CloseHandle(h);
  if (!ok) return false;
  const wchar_t* name = wcsrchr(path, L'\\');
  name = name ? name + 1 : path;
  return _wcsicmp(name, L"SafeExamBrowser.Client.exe") == 0;
}

struct FindCtx { HWND best; LONG bestArea; };
static BOOL CALLBACK FindProc(HWND hwnd, LPARAM lParam) {
  auto* ctx = (FindCtx*)lParam;
  if (!IsWindowVisible(hwnd)) return TRUE;
  if (GetWindow(hwnd, GW_OWNER)) return TRUE;
  if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) return TRUE;
  RECT rc; GetWindowRect(hwnd, &rc);
  LONG w = rc.right - rc.left, h = rc.bottom - rc.top;
  if (w < 250 || h < 250) return TRUE;
  wchar_t title[256] = {};
  GetWindowTextW(hwnd, title, 256);
  if (_wcsicmp(title, L"SplashScreen") == 0 || _wcsicmp(title, L"Taskbar") == 0)
    return TRUE;
  if (!IsTargetProcess(hwnd)) return TRUE;
  LONG area = w * h;
  if (area > ctx->bestArea) { ctx->bestArea = area; ctx->best = hwnd; }
  return TRUE;
}

static HWND FindWindow() {
  FindCtx ctx = {};
  EnumWindows(FindProc, (LPARAM)&ctx);
  return ctx.best;
}

// Track SEB's presence for diagnostics. The overlay never reparents.
static void Probe() {
  HWND seb = FindWindow();
  if (seb != g_sebWindow) {
    g_sebWindow = seb;
    InterlockedExchange(&g_zDirty, 1);   // re-raise right away against the newcomer
    if (seb) {
      wchar_t title[256] = {};
      GetWindowTextW(seb, title, 256);
      Log("seb: client window present %p \"%S\" — overlay stays top-level",
          (void*)seb, title);
    } else {
      Log("seb: client window gone — overlay stays top-level");
    }
  }
}

static void Tick() {
  DWORD now = GetTickCount();
  EnsureZHook();
  if (now - g_lastProbe < 1000) return;
  g_lastProbe = now;
  Probe();
  // Affinity maintenance: queue the APC (once per pid) + watchdog re-arms
  // if the target re-applies protection (readable cross-process).
  static DWORD s_lastSweep = 0;
  if (now - s_lastSweep >= 2000) {
    s_lastSweep = now;
    affinity::SweepProcess(L"SafeExamBrowser.Client.exe");
    affinity::Watchdog(L"SafeExamBrowser.Client.exe");
  }
}

// 50 ms z-order watchdog. Never activates, never shows a hidden window.
// Keeps the overlay above SEB and anything else in the topmost band.
static void EnsureTopmost() {
  if (!g_topmost) return;
  // SEB actively HIDES windows of non-permitted processes (its process
  // monitor). A hidden-but-should-be-visible window is exactly the "overlay
  // went behind / just invisible" symptom — show it back, throttled + logged
  // so a hide war with SEB stays diagnosable instead of flickering.
  static DWORD s_lastUnhide = 0;
  if (g_hwnd && router::IsVisible() && !IsWindowVisible(g_hwnd)) {
    DWORD now = GetTickCount();
    if (now - s_lastUnhide >= 500) {
      s_lastUnhide = now;
      ShowWindow(g_hwnd, SW_SHOW);
      Log("z: window hidden externally (SEB process monitor?) — re-shown");
    }
  }
  if (!IsWindowVisible(g_hwnd)) return;
  if (GetParent(g_hwnd)) {
    // Manually parented (--parent): raise among siblings only.
    SetWindowPos(g_hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    return;
  }
  LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
  if (!(ex & WS_EX_TOPMOST))
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex | WS_EX_TOPMOST);
  SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
               ((ex & WS_EX_TOPMOST) ? 0 : SWP_FRAMECHANGED));
}

static void Shutdown() {
  if (g_zHook) { UnhookWinEvent(g_zHook); g_zHook = nullptr; }
}

}  // namespace host

// ---------------------------------------------------------------- input dispatch
// Middle-mouse window drag + header left-drag move the window; sidebar owns
// wheel/LMB(thumb drag); everything else is forwarded to CEF as OSR mouse
// events in PAGE coordinates. Coordinates arrive in SCREEN px from the router.
static void DragBegin(int sx, int sy) {
  g_dragging = true;
  g_dragStart = { sx, sy };
  GetWindowRect(g_hwnd, &g_dragWndStart);
}
static void DragMove(int sx, int sy) {
  if (!g_dragging) return;
  int nx = g_dragWndStart.left + (sx - g_dragStart.x);
  int ny = g_dragWndStart.top + (sy - g_dragStart.y);
  if (GetParent(g_hwnd)) {   // hosted: position is in parent client coords
    POINT p = { nx, ny };
    ScreenToClient(GetParent(g_hwnd), &p);
    SetWindowPos(g_hwnd, nullptr, p.x, p.y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
  } else {
    SetWindowPos(g_hwnd, nullptr, nx, ny, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
  }
}
static void DragEnd() { g_dragging = false; }

// Current button state for EVENTFLAG_* modifiers on synthesized events.
static WPARAM g_mkFlags = 0;

static uint32_t CefModifiers() {
  uint32_t m = 0;
  if (g_mkFlags & MK_LBUTTON) m |= EVENTFLAG_LEFT_MOUSE_BUTTON;
  if (g_mkFlags & MK_RBUTTON) m |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
  if (g_mkFlags & MK_MBUTTON) m |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
  if (g_mkFlags & MK_SHIFT)   m |= EVENTFLAG_SHIFT_DOWN;
  if (g_mkFlags & MK_CONTROL) m |= EVENTFLAG_CONTROL_DOWN;
  return m;
}

// Diagnostic: what element is under a routed page click? Distinguishes
// "click not delivered" from "delivered but the page does not react".
static DWORD g_lastEval = 0;
static void DiagnoseClick(int cx, int cy) {
  DWORD now = GetTickCount();
  if (now - g_lastEval < 1500) return;
  g_lastEval = now;
  char js[512];
  snprintf(js, sizeof(js),
    "(function(){var e=document.elementFromPoint(%d,%d);if(!e)return 'NONE';"
    "var r=e.getBoundingClientRect();"
    "return e.tagName+' cls='+((e.getAttribute('class')||'').slice(0,40))"
    "+' rect='+Math.round(r.left)+','+Math.round(r.top)+' '+Math.round(r.width)+'x'+Math.round(r.height);})()",
    cx, cy);
  ExecJs(js, "hit");
}

static void DeliverToPage(unsigned msg, int sx, int sy, int data) {
  if (!g_browser || !g_browser->GetHost()) return;
  CefRefPtr<CefBrowserHost> bhost = g_browser->GetHost();

  RECT rc;
  GetWindowRect(g_hwnd, &rc);
  int hdr = kHeader;
  int clientW = rc.right - rc.left, clientH = rc.bottom - rc.top;
  int wx = sx - rc.left, wy = sy - rc.top;

  // Middle button drags the window itself — never reaches the page.
  if (msg == WM_MBUTTONDOWN) { DragBegin(sx, sy); return; }
  if (msg == WM_MBUTTONUP)   { DragEnd(); return; }

  bool inHeader = wy < hdr;
  // CLEAR button lives in the header's right segment, above the sidebar.
  if (msg == WM_LBUTTONDOWN && inHeader && wx >= SidebarLeft(clientW)) {
    g_thumbs.clear();
    g_thumbDrag = -1;
    g_sidebarScroll = 0;
    ghost::Hide();
    Log("[thumb] cleared (%d removed)", 0);
    InvalidateRect(g_hwnd, nullptr, FALSE);
    ComposeAndPresent();
    return;
  }
  if (msg == WM_LBUTTONDOWN && inHeader) { DragBegin(sx, sy); return; }
  if (g_dragging) {
    if (msg == WM_MOUSEMOVE) { DragMove(sx, sy); return; }
    if (msg == WM_LBUTTONUP) { DragEnd(); return; }
  }

  // Sidebar: wheel scrolls the thumbnail list; LMB on a thumbnail starts a
  // drag whose drop over the page attaches the shot to the composer.
  if (g_thumbDrag >= 0) {
    if (msg == WM_MOUSEMOVE) {
      g_thumbDragPos = { wx, wy };
      // Re-composite so the ghost blended into our DIB follows the cursor.
      ComposeAndPresent();
      return;
    }
    if (msg == WM_LBUTTONUP) { ThumbDropOrCancel(wx, wy, clientW); return; }
  }
  if (wx >= SidebarLeft(clientW) && wy >= hdr) {
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
      g_sidebarScroll -= (data > 0 ? 90 : -90);
      SidebarScrollClamp(clientH);
      InvalidateRect(g_hwnd, nullptr, FALSE);
      ComposeAndPresent();
      return;
    }
    if (msg == WM_LBUTTONDOWN) {
      int idx = ThumbAt(wx, wy, clientW);
      if (idx >= 0) {
        g_thumbDrag = idx;
        g_thumbDragPos = { wx, wy };
        // Composite the drag ghost immediately (not only after the first
        // move) so the preview is visible from the very first pixel.
        ComposeAndPresent();
        Log("[thumb] drag start idx=%d at %d,%d", idx, wx, wy);
      } else {
        Log("[thumb] sidebar click MISS at %d,%d (thumbs=%u scroll=%d)",
            wx, wy, (unsigned)g_thumbs.size(), g_sidebarScroll);
      }
      return;   // sidebar clicks never reach the page
    }
    return;
  }

  // Page area: forward to CEF as OSR events in view (page) coordinates.
  int cx = wx;
  int cy = wy - hdr;
  if (cx < 0) cx = 0;
  if (cy < 0) cy = 0;

  switch (msg) {
    case WM_LBUTTONDOWN: g_mkFlags |= MK_LBUTTON; break;
    case WM_LBUTTONUP:   g_mkFlags &= ~MK_LBUTTON; break;
    case WM_RBUTTONDOWN: g_mkFlags |= MK_RBUTTON; break;
    case WM_RBUTTONUP:   g_mkFlags &= ~MK_RBUTTON; break;
    case WM_MBUTTONDOWN: g_mkFlags |= MK_MBUTTON; break;
    case WM_MBUTTONUP:   g_mkFlags &= ~MK_MBUTTON; break;
  }

  CefMouseEvent ev;
  ev.x = cx;
  ev.y = cy;
  ev.modifiers = CefModifiers();

  if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
    SHORT delta = (SHORT)data;
    if (msg == WM_MOUSEWHEEL)
      bhost->SendMouseWheelEvent(ev, 0, delta);
    else
      bhost->SendMouseWheelEvent(ev, delta, 0);
    return;
  }
  switch (msg) {
    case WM_MOUSEMOVE:
      bhost->SendMouseMoveEvent(ev, false);
      break;
    case WM_LBUTTONDOWN:
      bhost->SetFocus(true);
      bhost->SendMouseMoveEvent(ev, false);
      bhost->SendMouseClickEvent(ev, MBT_LEFT, false, 1);
      DiagnoseClick(cx, cy);
      break;
    case WM_LBUTTONUP:
      bhost->SendMouseClickEvent(ev, MBT_LEFT, true, 1);
      break;
    case WM_RBUTTONDOWN:
      bhost->SendMouseMoveEvent(ev, false);
      bhost->SendMouseClickEvent(ev, MBT_RIGHT, false, 1);
      break;
    case WM_RBUTTONUP:
      bhost->SendMouseClickEvent(ev, MBT_RIGHT, true, 1);
      break;
  }
}

// ---------------------------------------------------------------- chrome paint
// Header band, grip, sidebar background + thumbnails — drawn into the
// composite DIB.

// Drag ghost: blend the dragged thumbnail at the cursor directly into the
// composite DIB — the exact approach the working Ultralight build used. A
// separate ghost window cannot win here: the overlay's topmost watchdog
// re-raises the overlay every 30 ms, so any separate window always ends up
// BEHIND the overlay and the drag preview is invisible. Thumb pixels are
// treated as opaque at 75% strength (CEF paint alpha is unreliable).
static void CompositeDragGhost(int winW, int winH) {
  if (g_thumbDrag < 0 || g_thumbDrag >= (int)g_thumbs.size()) return;
  const Thumb& t = g_thumbs[g_thumbDrag];
  int gx = g_thumbDragPos.x - t.w / 2, gy = g_thumbDragPos.y - t.h / 2;
  DWORD* px = (DWORD*)g_memBits;
  for (int y = 0; y < t.h; ++y) {
    int dy = gy + y;
    if (dy < 0 || dy >= winH) continue;
    for (int x = 0; x < t.w; ++x) {
      int dx = gx + x;
      if (dx < 0 || dx >= winW) continue;
      DWORD s = *(const DWORD*)&t.px[(size_t)y * t.w * 4 + (size_t)x * 4];
      DWORD* d = &px[(size_t)dy * winW + dx];
      DWORD r = (((s & 0xFF) * 3) + (*d & 0xFF)) / 4;
      DWORD g2 = ((((s >> 8) & 0xFF) * 3) + ((*d >> 8) & 0xFF)) / 4;
      DWORD b2 = ((((s >> 16) & 0xFF) * 3) + ((*d >> 16) & 0xFF)) / 4;
      *d = 0xFF000000 | (r & 0xFF) | ((g2 & 0xFF) << 8) | ((b2 & 0xFF) << 16);
    }
  }
}

static void DrawChrome(HDC hdc, int winW, int winH) {
  int hdr = kHeader;

  // Header band with centered grip.
  RECT headRc = { 0, 0, winW, hdr };
  HBRUSH bar = CreateSolidBrush(RGB(0x30, 0x2F, 0x36));
  FillRect(hdc, &headRc, bar);
  DeleteObject(bar);
  HBRUSH grip = CreateSolidBrush(RGB(0x8A, 0x88, 0x94));
  int gcy = hdr / 2;
  for (int i = -1; i <= 1; ++i) {
    int gx = winW / 2 + i * 12 - 2;
    RECT g = { gx, gcy - 2, gx + 4, gcy + 2 };
    FillRect(hdc, &g, grip);
  }
  DeleteObject(grip);

  // CLEAR button: header segment above the sidebar. Clicking it empties the
  // screenshot strip.
  RECT clearRc = { SidebarLeft(winW) + 10, 0, winW - 8, hdr };
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(0xC8, 0xC6, 0xD0));
  HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  HGDIOBJ oldF = SelectObject(hdc, f);
  DrawTextW(hdc, L"CLEAR", -1, &clearRc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
  SelectObject(hdc, oldF);

  // Sidebar band + thumbnails.
  RECT sideRc = { SidebarLeft(winW), hdr, winW, winH };
  HBRUSH sideBg = CreateSolidBrush(RGB(0x23, 0x22, 0x27));
  FillRect(hdc, &sideRc, sideBg);
  DeleteObject(sideBg);

  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  HPEN border = CreatePen(PS_SOLID, 1, RGB(0x4A, 0x48, 0x50));
  HGDIOBJ oldPen = SelectObject(hdc, border);
  HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
  for (int i = 0; i < (int)g_thumbs.size(); ++i) {
    Thumb& t = g_thumbs[i];
    int tx = SidebarLeft(winW) + kThumbPad;
    int ty = ThumbY0(i) - g_sidebarScroll;
    if (ty + t.h < hdr || ty > winH) continue;
    bmi.bmiHeader.biWidth = t.w;
    bmi.bmiHeader.biHeight = -t.h;
    SetDIBitsToDevice(hdc, tx, ty, t.w, t.h, 0, 0, 0, t.h,
                      t.px.data(), &bmi, DIB_RGB_COLORS);
    Rectangle(hdc, tx - 1, ty - 1, tx + t.w + 1, ty + t.h + 1);
  }
  SelectObject(hdc, oldBrush);
  SelectObject(hdc, oldPen);
  DeleteObject(border);
}

// ---------------------------------------------------------------- wndproc
static void CefPumpNow() {
  CefDoMessageLoopWork();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_NCHITTEST:
      return HTTRANSPARENT;          // click-through: router owns all input

    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;

    case WM_ERASEBKGND:
      return 1;

    case WM_PAINT: {                 // hosted child mode presents here
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);
      if (g_memDC)
        BitBlt(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, g_memDC, 0, 0, SRCCOPY);
      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_SIZE:
      if (g_browser && g_browser->GetHost()) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        AllocPage(SidebarLeft(rc.right - rc.left), rc.bottom - rc.top - kHeader);
        g_browser->GetHost()->WasResized();
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      ComposeAndPresent();
      break;

    case WM_DPICHANGED: {
      RECT* rc = (RECT*)lParam;
      SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left,
                   rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
      InvalidateRect(hwnd, nullptr, FALSE);
      ComposeAndPresent();
      return 0;
    }

    case router::kRouterMsg: {
      auto* ev = (router::RouterEvent*)lParam;
      if (ev) {
        DeliverToPage(ev->msg, ev->x, ev->y, ev->data);
        HeapFree(GetProcessHeap(), 0, ev);
      }
      return 0;
    }

    case router::kChordToggle: Log("[chord] toggle"); DoToggle(); return 0;
    case router::kChordShot:   Log("[chord] screenshot"); ScreenshotNow(); return 0;
    case router::kChordQuit:   Log("[chord] quit"); DestroyWindow(hwnd); return 0;
    case router::kChordPaste:  Log("[chord] paste"); PasteFromClipboard(); return 0;

    case kCefWorkMsg:
      CefPumpNow();
      return 0;

    case WM_TIMER:
      if (wParam == kCefTimerId) {
        KillTimer(hwnd, kCefTimerId);
        CefPumpNow();
      } else if (wParam == kPumpTimerId) {
        CefPumpNow();   // safety pump even if a scheduling callback was lost
      }
      return 0;

    case WM_DESTROY:
      router::Stop();
      ghost::Destroy();
      cursor::Destroy();
      host::Shutdown();
      PostQuitMessage(0);
      break;

    default:
      return DefWindowProc(hwnd, msg, wParam, lParam);
  }
  return 0;
}

// ---------------------------------------------------------------- CEF init
static std::string SubprocessPath() {
  char exePath[MAX_PATH];
  GetModuleFileNameA(nullptr, exePath, MAX_PATH);
  PathRemoveFileSpecA(exePath);
  return std::string(exePath) + "\\qshelper.exe";
}

static bool InitCef() {
  CefMainArgs main_args(GetModuleHandleW(nullptr));
  CefRefPtr<OverlayApp> app(new OverlayApp());

  // If this exe is ever (mis)used as a subprocess, run it as such.
  int exit_code = CefExecuteProcess(main_args, app.get(), nullptr);
  if (exit_code >= 0) return exit_code == 0;

  CefSettings settings;
  settings.no_sandbox = true;
  settings.windowless_rendering_enabled = true;
  settings.external_message_pump = true;
  settings.multi_threaded_message_loop = false;
  CefString(&settings.browser_subprocess_path) = ToWide(SubprocessPath()).c_str();
  CefString(&settings.cache_path) = ToWide(g_dataDir + "\\cef-cache").c_str();
  CefString(&settings.root_cache_path) = ToWide(g_dataDir + "\\cef-cache").c_str();
  CefString(&settings.log_file) = ToWide(g_dataDir + "\\cef.log").c_str();
  settings.log_severity = LOGSEVERITY_WARNING;

  bool inited = CefInitialize(main_args, settings, app.get(), nullptr);
  if (!inited) {
    // A just-exited sibling can leave the profile lock momentarily held.
    Log("[cef] CefInitialize failed (%lu) — retrying once after 2s", GetLastError());
    Sleep(2000);
    inited = CefInitialize(main_args, settings, app.get(), nullptr);
  }
  if (!inited) {
    Log("[cef] CefInitialize FAILED");
    return false;
  }

  RECT rc;
  GetClientRect(g_hwnd, &rc);
  AllocPage(SidebarLeft(rc.right - rc.left), rc.bottom - rc.top - kHeader);

  CefWindowInfo window_info;
  window_info.SetAsWindowless(g_hwnd);

  CefBrowserSettings browser_settings;
  browser_settings.windowless_frame_rate = 60;
  browser_settings.background_color = CefColorSetARGB(255, 255, 255, 255);

  CefRefPtr<OverlayClient> client(new OverlayClient());
  bool ok = CefBrowserHost::CreateBrowser(window_info, client.get(),
                                          CefString(g_url.c_str()),
                                          browser_settings, nullptr, nullptr);
  Log("[cef] CreateBrowser -> %d (subproc=%s)", (int)ok, SubprocessPath().c_str());
  return ok;
}

// ---------------------------------------------------------------- winmain
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  int argc = 0;
  LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::string> args;
  for (int i = 0; i < argc; ++i) {
    int n = WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, nullptr, 0, nullptr, nullptr);
    std::string a((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, &a[0], n, nullptr, nullptr);
    args.push_back(a);
  }
  LocalFree(argvW);
  int ac = (int)args.size();
  std::vector<char*> av; av.push_back(args[0].data());
  for (int i = 1; i < ac; ++i) av.push_back(args[i].data());

  g_url        = GetArg(ac, av.data(), "--url", g_url.c_str());
  g_topmost    = !HasArg(ac, av.data(), "--notopmost");
  g_startHidden = HasArg(ac, av.data(), "--hidden");
  g_x = atoi(GetArg(ac, av.data(), "--x", "0").c_str());
  g_y = atoi(GetArg(ac, av.data(), "--y", "0").c_str());
  g_w = atoi(GetArg(ac, av.data(), "--w", "1280").c_str());
  g_h = atoi(GetArg(ac, av.data(), "--h", "900").c_str());

  // Clamp persisted/handoff geometry to the current screen. A stale --x from
  // an earlier session (e.g. x=320 on a 1360-wide screen with w=1280) pushed
  // the entire sidebar off the right edge — that is why screenshot drags
  // produced zero [thumb] logs and no visible drag image.
  {
    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    if (g_w > scrW) g_w = scrW;
    if (g_h > scrH) g_h = scrH;
    if (g_w < 400) g_w = 400;
    if (g_h < 300) g_h = 300;
    if (g_x < 0) g_x = 0;
    if (g_y < 0) g_y = 0;
    if (g_x + g_w > scrW) g_x = scrW - g_w;
    if (g_y + g_h > scrH) g_y = scrH - g_h;
    Log("geometry: x=%d y=%d w=%d h=%d (screen %dx%d)", g_x, g_y, g_w, g_h,
        scrW, scrH);
  }

  std::string handoffEvent = GetArg(ac, av.data(), "--handoff-event", "");
  bool handoffWait = HasArg(ac, av.data(), "--handoff-wait");
  DWORD handoffParent = (DWORD)atoi(GetArg(ac, av.data(), "--handoff-parent", "0").c_str());

  // Single-instance: a fresh launch kills any leftover previous instances
  // (stale overlay after the target closed causes cache-lock + ghost-hook
  // issues). Handoff children carry --handoff-event and are exempt, otherwise
  // the parent would die mid-handoff.
  if (handoffEvent.empty()) {
    DWORD self = GetCurrentProcessId();
    wchar_t selfName[MAX_PATH] = {};
    {
      HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, self);
      if (h) {
        DWORD len = MAX_PATH;
        wchar_t path[MAX_PATH] = {};
        if (QueryFullProcessImageNameW(h, 0, path, &len)) {
          const wchar_t* n = wcsrchr(path, L'\\');
          wcscpy_s(selfName, n ? n + 1 : path);
        }
        CloseHandle(h);
      }
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE && selfName[0]) {
      PROCESSENTRY32W pe = { sizeof(pe) };
      if (Process32FirstW(snap, &pe)) do {
        if (pe.th32ProcessID != self &&
            (_wcsicmp(pe.szExeFile, selfName) == 0 ||
             _wcsicmp(pe.szExeFile, L"qshelper.exe") == 0)) {
          HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
          if (h) {
            TerminateProcess(h, 0);
            CloseHandle(h);
            Log("single-instance: killed stale %S pid=%lu", pe.szExeFile,
                (unsigned long)pe.th32ProcessID);
          }
        }
      } while (Process32NextW(snap, &pe));
      CloseHandle(snap);
      Sleep(800);   // let the kills land + CEF cache lock release
    }
  }

  char base[MAX_PATH];
  if (SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base) != S_OK)
    snprintf(base, sizeof(base), "C:\\Temp");
  g_dataDir = std::string(base) + "\\QuickSearch";
  EnsureDir(g_dataDir);

  Log("QuickSearch starting: %s (topmost=%d hidden=%d)", g_url.c_str(),
      (int)g_topmost, (int)g_startHidden);

  if (!handoffEvent.empty()) handoff::Join(handoffEvent.c_str(), handoffParent, handoffWait);

  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(hr)) { Log("CoInitializeEx failed 0x%08lx", hr); return 1; }

  WNDCLASSEXA wc = {};
  wc.cbSize        = sizeof(wc);
  wc.style         = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc   = WndProc;
  wc.hInstance     = hInst;
  wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = "QuickSearchWnd";
  RegisterClassExA(&wc);

  DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
                  WS_EX_LAYERED;
  if (g_topmost) exStyle |= WS_EX_TOPMOST;
  if (g_x == 0 && g_y == 0) {
    g_x = (GetSystemMetrics(SM_CXSCREEN) - g_w) / 2;
    g_y = (GetSystemMetrics(SM_CYSCREEN) - g_h) / 2;
  }
  g_hwnd = CreateWindowExA(exStyle, wc.lpszClassName, kAppName,
                           WS_POPUP | WS_CLIPCHILDREN, g_x, g_y, g_w, g_h,
                           nullptr, nullptr, hInst, nullptr);
  if (!g_hwnd) { Log("CreateWindow failed %lu", GetLastError()); return 1; }

  // Keep the overlay out of OTHER apps' screen captures (SEB monitoring,
  // recorders, snipping tools) while it stays fully visible on the real
  // screen. Win10 2004+; harmless if unsupported.
  if (SetWindowDisplayAffinity(g_hwnd, WDA_EXCLUDEFROMCAPTURE))
    Log("display-affinity: excluded from capture");
  else
    Log("display-affinity: not supported (%lu)", GetLastError());

  ghost::Create();
  cursor::Create();

  EnsureGdiPlus();

  if (!InitCef()) {
    MessageBoxA(nullptr,
      "CEF failed to initialize (see overlay.log / cef.log in\n"
      "%LOCALAPPDATA%\\QuickSearch).",
      "QuickSearch", MB_ICONERROR);
    return 1;
  }

  ShowWindow(g_hwnd, g_startHidden ? SW_HIDE : nCmdShow);
  SetVisible(!g_startHidden);

  if (!router::Start()) Log("router failed to start — overlay input disabled");
  g_hooksUp = true;
  g_routing = !g_startHidden;
  router::UpdateBounds();
  SetTimer(g_hwnd, kPumpTimerId, 50, nullptr);   // safety pump
  host::Tick();   // attach now if SEB is already running
  handoff::SignalReady();
  ComposeAndPresent();   // header/sidebar backdrop before first page paint

  // Message loop with z-order / desktop handoff ticks.
  // CEF's external pump is driven by OnScheduleMessagePumpWork -> timer/msg.
  // Cursor model: the native cursor stays VISIBLE and moves over the overlay
  // normally; the fake cursor window is never shown (invisible). No
  // ShowCursor games — a globally hidden native cursor was the earlier bug.
  MSG msg;
  while (GetMessage(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
    static DWORD lastHost = 0, lastTop = 0, lastWatch = 0;
    DWORD now = GetTickCount();
    if (now - lastHost >= 1000) { lastHost = now;  host::Tick(); }
    if (now - lastTop >= 30) {
      lastTop = now;
      host::EnsureTopmost();
      ghost::Raise();          // keep the drag ghost above the overlay
      host::ConsumeZDirty();   // + instantly when SEB reorders itself
    }
    if (now - lastWatch >= 50)  { lastWatch = now; handoff::Watch(); }
    if (g_hooksUp && g_routing) router::UpdateBounds();
  }

  // Note: no CefShutdown() — exiting the browser process tears the subprocess
  // tree down automatically (channel loss) and avoids pump teardown hangs.
  host::Shutdown();
  CoUninitialize();
  return 0;
}
