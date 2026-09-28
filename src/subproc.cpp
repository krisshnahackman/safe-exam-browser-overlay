// CefOverlayBrowser.exe — CEF subprocess helper for CefOverlay.
//
// Every renderer / GPU / utility subprocess CEF spawns is launched as a copy
// of THIS executable (via CefSettings.browser_subprocess_path) with --type=...
// arguments. Keeping it a separate tiny exe means Task Manager shows only our
// own names (CefOverlay.exe + CefOverlayBrowser.exe) — no chrome/msedge
// branding, which WebView2 could not do.
#include <Windows.h>
#include "include/cef_app.h"
#include "include/cef_command_line.h"

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
  CefMainArgs main_args(hInst);
  CefRefPtr<CefApp> app;   // no handler overrides needed in the helper
  int exit_code = CefExecuteProcess(main_args, app.get(), nullptr);
  if (exit_code >= 0) return exit_code;
  return 0;
}
