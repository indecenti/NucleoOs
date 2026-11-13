# NucleoConnect (Windows app)

A lightweight desktop connector for NucleoOS. It **scans the LAN**, lists the NucleoOS
devices it finds, and hosts the selected device's web shell in an embedded WebView2 — so
the PC does the rendering and the ESP32 only serves files and live events.

## Why this design

- **Light on the device:** discovery probes `GET /api/status` once per host with a short
  timeout; the shell UI runs in WebView2 on the PC, not on the ESP32.
- **Light on the PC:** WinForms + WebView2 reuse the Edge runtime already on Windows 11,
  so the build is tiny and needs no bundled browser.
- **Reuses the web shell:** identical UI to any browser; no duplicate codebase.

## How it connects

1. **Scan network** probes every host on your local /24 subnets for `/api/status`
   and keeps the ones that report `"os":"NucleoOS"`.
2. Pick a device (double-click) — or type its mDNS name (`nucleo-01.local`) or IP and
   press **Connect**.
3. WebView2 navigates to `http://<device>/` and the desktop shell loads.

The device must be powered on with its HTTP server up (it advertises itself via mDNS as
`_nucleoos._tcp`, which also lets Windows resolve `nucleo-01.local`).

## Build & run

Targets **.NET Framework 4.8**, which every Windows 10 (1903+) and Windows 11 already has: users install
nothing. The WebView2 assemblies and the native `WebView2Loader.dll` (x64, x86, arm64) are embedded in the exe,
so the download is one file (~1.1 MB). On first start the loader is unpacked to
`%LOCALAPPDATA%\NucleoConnect\loader\<arch>-<hash>` and the browser profile lives in
`%LOCALAPPDATA%\NucleoConnect\WebView2`. Without the WebView2 Runtime (older Windows 10) the app offers
Microsoft's download page.

Requires a .NET SDK (any recent one) on the build machine:

```
cd windows-app
dotnet build -c Release
copy bin\Release\net48\NucleoConnect.exe ..\web\downloads\NucleoConnect.exe
```

Only `NucleoConnect.exe` ships (`web/downloads/` -> the SD's `www/shell/downloads/`); the DLLs next to it in
`bin/` are not needed. `NucleoConnect.exe <host>` opens that device directly (handy in a shortcut).

The exe is not code-signed: Windows SmartScreen shows "Windows protected your PC" the first time:
**More info -> Run anyway**.

## Security

The PC file bridge (`LocalFs`, `window.chrome.webview.hostObjects.nucleo`) can read and write this PC's files,
so it exists only for the device you connected to: any navigation to another site opens in the default browser,
and iframes get the bridge only when they are the device's own origin.

## Files

| File | Role |
|---|---|
| `Program.cs` | Entry point |
| `MainForm.cs` | UI: device list, scan/connect, WebView2 host |
| `Discovery.cs` | Dependency-free LAN scan for `/api/status` |
| `LocalFs.cs` | PC file bridge for File Commander (device origin only) |
| `app.manifest` | Per-monitor DPI awareness |
