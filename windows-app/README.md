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

Requires the .NET 8 SDK (Windows). The WebView2 **runtime** ships with Windows 11.

```
cd windows-app
dotnet run -c Release
```

A standalone single-file build:

```
dotnet publish -c Release -r win-x64 --self-contained false ^
  -p:PublishSingleFile=true -o dist
```

## Files

| File | Role |
|---|---|
| `Program.cs` | Entry point |
| `MainForm.cs` | UI: device list, scan/connect, WebView2 host |
| `Discovery.cs` | Dependency-free LAN scan for `/api/status` |
| `app.manifest` | Per-monitor DPI awareness |
