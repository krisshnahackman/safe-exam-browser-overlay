// AffManaged — managed helper loaded INSIDE the target via CLR hosting.
// Runs SetWindowDisplayAffinity(hwnd, WDA_NONE) as an ordinary managed
// P/Invoke call on an ordinary thread: the CLR tolerates this fine, unlike
// native shellcode / foreign threads. Runtime-only: no disk patch, BEK and
// config keys unchanged.
// Build: csc /target:library /out:AffManaged.dll affmanaged.cs  (net40-safe,
// no extra refs)
using System;
using System.Runtime.InteropServices;

namespace AffManaged
{
    public class Entrypoint
    {
        [DllImport("user32.dll")]
        private static extern bool SetWindowDisplayAffinity(IntPtr hwnd, uint affinity);
        [DllImport("user32.dll")]
        private static extern bool IsWindowVisible(IntPtr hwnd);
        [DllImport("user32.dll")]
        private static extern IntPtr GetTopWindow(IntPtr h);
        [DllImport("user32.dll")]
        private static extern IntPtr GetWindow(IntPtr h, uint cmd);
        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
        [DllImport("user32.dll")]
        private static extern int GetWindowLong(IntPtr h, int idx);
        [DllImport("user32.dll")]
        private static extern bool GetWindowRect(IntPtr h, out RECT r);

        [StructLayout(LayoutKind.Sequential)]
        private struct RECT { public int l, t, r, b; }

        private const uint GW_OWNER = 4, GW_HWNDNEXT = 2;
        private const int GWL_STYLE = -16;
        private const int WS_CHILD = 0x40000000;
        private const uint WDA_NONE = 0;

        public static int Run(string arg)
        {
            try
            {
                uint myPid = (uint)System.Diagnostics.Process.GetCurrentProcess().Id;
                int cleared = 0;
                for (IntPtr hwnd = GetTopWindow(IntPtr.Zero); hwnd != IntPtr.Zero; hwnd = GetWindow(hwnd, GW_HWNDNEXT))
                {
                    try
                    {
                        uint pid;
                        GetWindowThreadProcessId(hwnd, out pid);
                        if (pid != myPid) continue;
                        if (GetWindow(hwnd, GW_OWNER) != IntPtr.Zero) continue;
                        if ((GetWindowLong(hwnd, GWL_STYLE) & WS_CHILD) != 0) continue;
                        if (!IsWindowVisible(hwnd)) continue;
                        RECT rc;
                        if (!GetWindowRect(hwnd, out rc)) continue;
                        if (rc.r - rc.l < 200 || rc.b - rc.t < 200) continue;
                        if (SetWindowDisplayAffinity(hwnd, WDA_NONE)) cleared++;
                    }
                    catch { }
                }
                // Re-sweep in background: affinity is per-HWND, windows recur.
                var t = new System.Threading.Thread(() => {
                    try {
                        for (int i = 0; i < 40; i++) {
                            System.Threading.Thread.Sleep(3000);
                            for (IntPtr hwnd = GetTopWindow(IntPtr.Zero); hwnd != IntPtr.Zero; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
                                try {
                                    uint pid;
                                    GetWindowThreadProcessId(hwnd, out pid);
                                    if (pid != myPid) continue;
                                    if (GetWindow(hwnd, GW_OWNER) != IntPtr.Zero) continue;
                                    if ((GetWindowLong(hwnd, GWL_STYLE) & WS_CHILD) != 0) continue;
                                    if (!IsWindowVisible(hwnd)) continue;
                                    RECT rc;
                                    if (!GetWindowRect(hwnd, out rc)) continue;
                                    if (rc.r - rc.l < 200 || rc.b - rc.t < 200) continue;
                                    SetWindowDisplayAffinity(hwnd, WDA_NONE);
                                } catch { }
                            }
                        }
                    } catch { }
                });
                t.IsBackground = true;
                t.Start();
                return cleared;
            }
            catch { return -1; }
        }
    }
}
