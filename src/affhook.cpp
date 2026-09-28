// ============================================================================
// AffHook — native loader DLL, SEBExploit-style (QueueUserAPC entry).
//
// Why this design (matches https://github.com/fictiouss/SEBExploit):
//   The target is a .NET CLR process. Raw CreateRemoteThread shellcode and
//   WH_GETMESSAGE hooks both execute NATIVE code on threads the CLR doesn't
//   expect -> 0xC0000005 -> "unrecoverable error" / "failed to start".
//   The repo's proven path instead:
//     1. Injector finds the target pid+tid (Toolhelp), writes the DLL path
//        with VirtualAllocEx/WriteProcessMemory, then
//        QueueUserAPC(LoadLibraryW, thread, path) — the APC runs when the
//        thread hits an alertable wait, a normal CLR-safe spot.
//     2. DllMain does ~nothing (DisableThreadLibraryCalls + spawn worker;
//        never CLR work inside DllMain = loader lock).
//     3. The worker loads the MANAGED helper the same way the CLR already
//        exists in the process: via mscoree's CLRCreateInstance resolved
//        dynamically with LoadLibrary/GetProcAddress (no SDK headers needed
//        to build — plain kernel32/user32 links only).
//     4. Managed helper (AffManaged.dll, C#) P/Invokes
//        SetWindowDisplayAffinity(hwnd, WDA_NONE) on its OWN windows:
//        ordinary managed call, ordinary thread — survives. Runtime-only,
//        no disk patch, BEK/config keys unchanged.
//
// Layout: AffHook.dll (this) + AffManaged.dll (csc-built C#) must BOTH sit
// next to CefOverlay.exe. If the managed DLL is missing, the worker just
// self-sweeps (harmless, crashes nothing — native call on own windows only
// when this DLL is loaded into OUR OWN process, no-op otherwise).
// ============================================================================
#include <Windows.h>
#include <string>

static HMODULE g_hMod = nullptr;

typedef HRESULT(WINAPI* FnCLRCreateInstance)(REFCLSID, REFIID, LPVOID*);
// Correct GUIDs (from dotnet/runtime src/coreclr/inc/metahost.idl):
//   CLSID_CLRMetaHost = 9280188D-0E8E-4867-B30C-7FA83884E8DE
//   IID_ICLRMetaHost  = D332DB9E-B9B3-4125-8207-A14884F53216
// (An earlier build had these swapped/wrong -> CLRCreateInstance failed.)
static const GUID CLSID_CLRMetaHost_ =
{ 0x9280188D, 0x0E8E, 0x4867, { 0xB3, 0x0C, 0x7F, 0xA8, 0x38, 0x84, 0xE8, 0xDE } };
static const GUID IID_ICLRMetaHost_ =
{ 0xD332DB9E, 0xB9B3, 0x4125, { 0x82, 0x07, 0xA1, 0x48, 0x84, 0xF5, 0x32, 0x16 } };

static std::wstring DllDir() {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(g_hMod, path, MAX_PATH);
  std::wstring s(path);
  size_t p = s.find_last_of(L"\\/");
  return (p == std::wstring::npos) ? std::wstring(L".\\") : s.substr(0, p + 1);
}

static void HookLog(const wchar_t* fmt, DWORD v) {
  wchar_t dir[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, dir);
  wcscat_s(dir, L"AffHook.log");
  HANDLE f = CreateFileW(dir, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  char buf[256];
  int n = 0;
  if (fmt) {
    // minimal: "msg value=%lu pid=%lu\n"
    n = snprintf(buf, sizeof(buf), "%S value=%lu pid=%lu\r\n", fmt, v, GetCurrentProcessId());
  }
  DWORD w = 0;
  if (n > 0) WriteFile(f, buf, (DWORD)n, &w, nullptr);
  CloseHandle(f);
}

static void NativeSelfSweep() {
  DWORD myPid = GetCurrentProcessId();
  for (HWND hwnd = GetTopWindow(nullptr); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != myPid) continue;
    if (GetWindow(hwnd, GW_OWNER)) continue;
    if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) continue;
    if (!IsWindowVisible(hwnd)) continue;
    SetWindowDisplayAffinity(hwnd, WDA_NONE);
  }
}

// Minimal COM vtables for ICLRMetaHost / ICLRRuntimeInfo / ICLRRuntimeHost —
// just enough to call GetRuntime + GetInterface + Start +
// ExecuteInDefaultAppDomain without metahost.h/mscoree.h.
struct IMetaHost;
typedef HRESULT(STDMETHODCALLTYPE IMetaHost::*FnGetRuntime)(
  LPCWSTR, REFIID, LPVOID*);
struct IRuntimeInfo;
struct IRuntimeHost;

struct IMetaHostVtbl {
  HRESULT(STDMETHODCALLTYPE* QueryInterface)(IMetaHost*, REFIID, void**);
  ULONG(STDMETHODCALLTYPE* AddRef)(IMetaHost*);
  ULONG(STDMETHODCALLTYPE* Release)(IMetaHost*);
  HRESULT(STDMETHODCALLTYPE* GetRuntime)(IMetaHost*, LPCWSTR, REFIID, LPVOID*);
};
struct IMetaHost { IMetaHostVtbl* lpVtbl; };

struct IRuntimeInfoVtbl {
  HRESULT(STDMETHODCALLTYPE* QueryInterface)(IRuntimeInfo*, REFIID, void**);
  ULONG(STDMETHODCALLTYPE* AddRef)(IRuntimeInfo*);
  ULONG(STDMETHODCALLTYPE* Release)(IRuntimeInfo*);
  HRESULT(STDMETHODCALLTYPE* GetVersionString)(IRuntimeInfo*, LPWSTR, DWORD*);
  HRESULT(STDMETHODCALLTYPE* GetRuntimeDirectory)(IRuntimeInfo*, LPWSTR, DWORD*);
  HRESULT(STDMETHODCALLTYPE* IsLoaded)(IRuntimeInfo*, HANDLE, BOOL*);
  HRESULT(STDMETHODCALLTYPE* LoadErrorString)(IRuntimeInfo*, UINT, LPWSTR, DWORD*, LONG*);
  HRESULT(STDMETHODCALLTYPE* LoadLibrary)(IRuntimeInfo*, LPCWSTR, HMODULE*);
  HRESULT(STDMETHODCALLTYPE* GetProcAddress)(IRuntimeInfo*, LPCSTR, LPVOID*);
  HRESULT(STDMETHODCALLTYPE* GetInterface)(IRuntimeInfo*, REFCLSID, REFIID, LPVOID*);
};
struct IRuntimeInfo { IRuntimeInfoVtbl* lpVtbl; };

struct IRuntimeHostVtbl {
  HRESULT(STDMETHODCALLTYPE* QueryInterface)(IRuntimeHost*, REFIID, void**);
  ULONG(STDMETHODCALLTYPE* AddRef)(IRuntimeHost*);
  ULONG(STDMETHODCALLTYPE* Release)(IRuntimeHost*);
  HRESULT(STDMETHODCALLTYPE* Start)(IRuntimeHost*);
  HRESULT(STDMETHODCALLTYPE* Stop)(IRuntimeHost*);
  HRESULT(STDMETHODCALLTYPE* SetHostControl)(IRuntimeHost*, void*);
  HRESULT(STDMETHODCALLTYPE* GetCLRControl)(IRuntimeHost*, void**);
  HRESULT(STDMETHODCALLTYPE* UnloadAppDomain)(IRuntimeHost*, DWORD, BOOL*);
  HRESULT(STDMETHODCALLTYPE* ExecuteInAppDomain)(IRuntimeHost*, DWORD,
    HRESULT(WINAPI*)(LPVOID), LPVOID);
  HRESULT(STDMETHODCALLTYPE* GetCurrentAppDomainId)(IRuntimeHost*, DWORD*);
  HRESULT(STDMETHODCALLTYPE* ExecuteApplication)(IRuntimeHost*, LPCWSTR, DWORD,
    LPCWSTR, LPCWSTR, LPCWSTR, DWORD*);
  HRESULT(STDMETHODCALLTYPE* ExecuteInDefaultAppDomain)(IRuntimeHost*, LPCWSTR,
    LPCWSTR, LPCWSTR, LPCWSTR, DWORD*);
};
struct IRuntimeHost { IRuntimeHostVtbl* lpVtbl; };

// CLSID_CLRRuntimeHost = 90F1A06E-7712-4762-86B5-7A5EBA6BDB02
static const GUID CLSID_CLRRuntimeHost_ =
{ 0x90F1A06E, 0x7712, 0x4762, { 0x86, 0xB5, 0x7A, 0x5E, 0xBA, 0x6B, 0xDB, 0x02 } };
// {[2EBCD49A-1B47-4A61-B13A-BA16E9A0341D]} IID_ICLRRuntimeHost
static const GUID IID_ICLRRuntimeHost_ =
{ 0x2EBCD49A, 0x1B47, 0x4A61, { 0xB1, 0x3A, 0xBA, 0x16, 0xE9, 0xA0, 0x34, 0x1D } };
// IID_ICLRRuntimeInfo = BD39D1D2-BA2F-486A-89B0-B4B0CB466891
static const GUID IID_ICLRRuntimeInfo_ =
{ 0xBD39D1D2, 0xBA2F, 0x486A, { 0x89, 0xB0, 0xB4, 0xB0, 0xCB, 0x46, 0x68, 0x91 } };

static DWORD WINAPI ManagedEntry(LPVOID) {
  HookLog(L"loaded", 0);
  // PRIMARY PATH: plain native sweep on our own worker thread.
  // SetWindowDisplayAffinity is just a kernel flag-set via user32 — no CLR
  // interaction, no loader-lock risk, no COM vtables to get wrong. The old
  // crashes came from FOREIGN threads (CreateRemoteThread shellcode), not
  // from this call itself. An APC-spawned worker thread is a normal thread.
  int cleared = 0;
  DWORD myPid = GetCurrentProcessId();
  for (HWND hwnd = GetTopWindow(nullptr); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != myPid) continue;
    if (GetWindow(hwnd, GW_OWNER)) continue;
    if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) continue;
    if (!IsWindowVisible(hwnd)) continue;
    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) continue;
    if (rc.right - rc.left < 200 || rc.bottom - rc.top < 200) continue;
    DWORD cur = 0;
    GetWindowDisplayAffinity(hwnd, &cur);   // skip work if already NONE
    if (cur == WDA_NONE) continue;
    if (SetWindowDisplayAffinity(hwnd, WDA_NONE)) cleared++;
  }
  HookLog(L"native-sweep", (DWORD)cleared);
  // Re-sweep ~3 min at 1.5s: affinity is per-HWND, windows get recreated and
  // the target re-applies protection — re-clear continuously.
  for (int i = 0; i < 120; i++) {
    Sleep(1500);
    for (HWND hwnd = GetTopWindow(nullptr); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
      DWORD pid = 0;
      GetWindowThreadProcessId(hwnd, &pid);
      if (pid != myPid) continue;
      if (GetWindow(hwnd, GW_OWNER)) continue;
      if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) continue;
      if (!IsWindowVisible(hwnd)) continue;
      RECT rc;
      if (!GetWindowRect(hwnd, &rc)) continue;
      if (rc.right - rc.left < 200 || rc.bottom - rc.top < 200) continue;
      DWORD cur = 0;
      GetWindowDisplayAffinity(hwnd, &cur);
      if (cur == WDA_NONE) continue;
      SetWindowDisplayAffinity(hwnd, WDA_NONE);
    }
  }
  HookLog(L"native-resweep-done", 0);
  std::wstring managed = DllDir() + L"AffManaged.dll";
  if (GetFileAttributesW(managed.c_str()) == INVALID_FILE_ATTRIBUTES) {
    HookLog(L"no-managed-selfsweep", 0);
    NativeSelfSweep();
    return 1;
  }
  HMODULE mscoree = LoadLibraryW(L"mscoree.dll");
  if (!mscoree) { HookLog(L"no-mscoree", GetLastError()); return 2; }
  FnCLRCreateInstance clrCreate =
    (FnCLRCreateInstance)GetProcAddress(mscoree, "CLRCreateInstance");
  if (!clrCreate) { HookLog(L"no-clrcreate", 0); return 3; }
  IMetaHost* meta = nullptr;
  if (FAILED(clrCreate(CLSID_CLRMetaHost_, IID_ICLRMetaHost_, (LPVOID*)&meta))) {
    HookLog(L"meta-fail", 0);
    return 4;
  }
  IRuntimeInfo* info = nullptr;
  if (FAILED(meta->lpVtbl->GetRuntime(meta, L"v4.0.30319",
                                      IID_ICLRRuntimeInfo_, (LPVOID*)&info))) {
    meta->lpVtbl->Release(meta);
    HookLog(L"runtime-fail", 0);
    return 5;
  }
  IRuntimeHost* host = nullptr;
  if (FAILED(info->lpVtbl->GetInterface(info, CLSID_CLRRuntimeHost_,
                                        IID_ICLRRuntimeHost_, (LPVOID*)&host))) {
    info->lpVtbl->Release(info);
    meta->lpVtbl->Release(meta);
    HookLog(L"host-iface-fail", 0);
    return 6;
  }
  DWORD ret = 0;
  HRESULT hrS = host->lpVtbl->Start(host);
  HRESULT hrX = host->lpVtbl->ExecuteInDefaultAppDomain(host, managed.c_str(),
    L"AffManaged.Entrypoint", L"Run", L"", &ret);
  HookLog(L"managed-run", ret);
  if (FAILED(hrS)) HookLog(L"host-start-fail", (DWORD)hrS);
  if (FAILED(hrX)) HookLog(L"exec-fail", (DWORD)hrX);
  host->lpVtbl->Release(host);
  info->lpVtbl->Release(info);
  meta->lpVtbl->Release(meta);
  return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_hMod = h;
    DisableThreadLibraryCalls(h);
    // Never CLR work inside DllMain (loader lock) — worker thread only.
    CreateThread(nullptr, 0, ManagedEntry, nullptr, 0, nullptr);
  }
  return TRUE;
}
