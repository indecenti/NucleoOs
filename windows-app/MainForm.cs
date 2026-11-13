using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;

namespace NucleoConnect;

/// <summary>
/// Connector window: scan the LAN, pick a NucleoOS device (or type its mDNS name),
/// and host its web shell in WebView2. The shell does the heavy UI work, not the device.
/// </summary>
public class MainForm : Form
{
    private readonly ListBox _list = new() { Dock = DockStyle.Fill, IntegralHeight = false };
    private readonly Button _scan = new() { Text = "Scan network", Dock = DockStyle.Top, Height = 34 };
    private readonly Button _connect = new() { Text = "Connect", Dock = DockStyle.Bottom, Height = 34 };
    private readonly TextBox _host = new() { Dock = DockStyle.Bottom, Text = "nucleo-01.local" };
    private readonly Label _status = new() { Dock = DockStyle.Bottom, Height = 22, Text = "Idle." };
    private readonly WebView2 _web = new() { Dock = DockStyle.Fill };
    private CancellationTokenSource? _cts;
    private bool _webReady;
    private string? _pending;                       // a Connect() that came before WebView2 was ready
    private string? _deviceAuthority;               // "host[:port]" of the connected device, the only place the bridge exists

    private readonly Panel _side = new() { Dock = DockStyle.Left, Width = 300, Padding = new Padding(8) };
    private bool _fullScreen = false;
    private FormWindowState _prevState = FormWindowState.Normal;

    public MainForm(string? startHost = null)
    {
        Text = "NucleoOS Connect";
        Width = 1100; Height = 720; StartPosition = FormStartPosition.CenterScreen;
        KeyPreview = true;
        
        // Dark Mode Theme (Stile NucleoOS)
        BackColor = Color.FromArgb(22, 35, 63);
        ForeColor = Color.White;
        _side.BackColor = Color.FromArgb(16, 24, 38);
        _list.BackColor = Color.FromArgb(22, 35, 63);
        _list.ForeColor = Color.White;
        _list.BorderStyle = BorderStyle.None;
        _host.BackColor = Color.FromArgb(30, 45, 75);
        _host.ForeColor = Color.White;
        _host.BorderStyle = BorderStyle.FixedSingle;

        var sideInner = new Panel { Dock = DockStyle.Fill };
        sideInner.Controls.Add(_list);
        sideInner.Controls.Add(new Label { Dock = DockStyle.Top, Height = 22, Text = "Devices found:", ForeColor = Color.LightGray });
        
        var topControls = new Panel { Dock = DockStyle.Top, Height = 64 };
        var btnHide = new Button { Text = "Hide Panel (F11)", Dock = DockStyle.Right, Width = 100, FlatStyle = FlatStyle.Flat, Cursor = Cursors.Hand };
        btnHide.FlatAppearance.BorderColor = Color.FromArgb(40, 60, 90);
        var btnScan = _scan;
        btnScan.Dock = DockStyle.Fill;
        btnScan.FlatStyle = FlatStyle.Flat;
        btnScan.Cursor = Cursors.Hand;
        btnScan.FlatAppearance.BorderColor = Color.FromArgb(40, 60, 90);
        _connect.FlatStyle = FlatStyle.Flat;
        _connect.Cursor = Cursors.Hand;
        _connect.FlatAppearance.BorderColor = Color.FromArgb(40, 60, 90);

        topControls.Controls.Add(btnScan);
        topControls.Controls.Add(btnHide);
        
        sideInner.Controls.Add(topControls);
        _side.Controls.Add(sideInner);
        _side.Controls.Add(_connect);
        _side.Controls.Add(_host);
        _side.Controls.Add(new Label { Dock = DockStyle.Bottom, Height = 18, Text = "…or connect by name/IP:", ForeColor = Color.LightGray });
        _side.Controls.Add(_status);

        Controls.Add(_web);
        Controls.Add(_side);

        _scan.Click += async (_, _) => await ScanAsync();
        _connect.Click += (_, _) => Connect(_host.Text.Trim());
        _list.DoubleClick += (_, _) => { if (_list.SelectedItem is NucleoDevice d) Connect(d.Ip); };
        _list.SelectedIndexChanged += (_, _) => { if (_list.SelectedItem is NucleoDevice d) _host.Text = d.Ip; };
        
        btnHide.Click += (_, _) => ToggleFullScreen();
        KeyDown += (s, e) => {
            if (e.KeyCode == Keys.F11) ToggleFullScreen();
            else if (e.KeyCode == Keys.Escape && _fullScreen) ToggleFullScreen();
        };

        if (!string.IsNullOrWhiteSpace(startHost)) { _host.Text = startHost!.Trim(); _pending = _host.Text; }
        Load += async (_, _) => await InitWebViewAsync();
    }

    private void ToggleFullScreen()
    {
        _fullScreen = !_fullScreen;
        if (_fullScreen)
        {
            _prevState = WindowState;
            _side.Visible = false;
            FormBorderStyle = FormBorderStyle.None;
            WindowState = FormWindowState.Maximized;
        }
        else
        {
            _side.Visible = true;
            FormBorderStyle = FormBorderStyle.Sizable;
            WindowState = _prevState;
        }
    }

    /// <summary>
    /// Boot WebView2 and inject the native filesystem bridge so the web file manager can browse
    /// the drives connected to this PC. The bridge is exposed to the top-level document AND to
    /// iframes (the File Commander app runs in an iframe inside the device shell), so it is
    /// reachable as <c>window.chrome.webview.hostObjects.nucleo</c> on the device's pages.
    /// The bridge reaches this PC's files, so it exists ONLY for the device you connected to: navigation
    /// elsewhere opens in your normal browser, and iframes get it only when they are the device's own origin.
    /// </summary>
    private async Task InitWebViewAsync()
    {
        try
        {
            CoreWebView2Environment.SetLoaderDllFolderPath(Program.UnpackLoader());
            var env = await CoreWebView2Environment.CreateAsync(null, Path.Combine(Program.DataDir, "WebView2"));
            await _web.EnsureCoreWebView2Async(env);
        }
        catch (WebView2RuntimeNotFoundException)
        {
            _status.Text = "Microsoft Edge WebView2 Runtime is missing.";
            if (MessageBox.Show(this,
                    "NucleoOS Connect needs the Microsoft Edge WebView2 Runtime (already part of Windows 11).\n\n" +
                    "Open the download page now?", "WebView2 Runtime missing",
                    MessageBoxButtons.YesNo, MessageBoxIcon.Information) == DialogResult.Yes)
                OpenExternal("https://go.microsoft.com/fwlink/p/?LinkId=2124703");
            return;
        }
        catch (Exception ex)
        {
            _status.Text = "WebView2 failed to start.";
            MessageBox.Show(this, "The embedded browser could not start:\n" + ex.Message, "NucleoOS Connect",
                MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }
        var core = _web.CoreWebView2;
        core.AddHostObjectToScript("nucleo", new LocalFs());
        core.NavigationStarting += (_, e) =>
        {
            if (!IsDevice(e.Uri)) { e.Cancel = true; OpenExternal(e.Uri); }
        };
        core.NewWindowRequested += (_, e) =>
        {
            e.Handled = true;
            if (IsDevice(e.Uri)) _web.Source = new Uri(e.Uri); else OpenExternal(e.Uri);
        };
        core.FrameCreated += (_, e) =>
        {
            if (_deviceAuthority == null) return;
            try { e.Frame.AddHostObjectToScript("nucleo", new LocalFs(), new[] { "http://" + _deviceAuthority }); }
            catch { /* older runtime without per-frame host objects: top-level injection still works */ }
        };
        core.NavigationCompleted += (_, e) =>
        {
            if (_deviceAuthority != null)
                _status.Text = e.IsSuccess ? $"Connected to {_deviceAuthority}." : $"Cannot reach {_deviceAuthority}.";
        };
        _webReady = true;
        if (_pending != null) { var p = _pending; _pending = null; Connect(p); }
    }

    private bool IsDevice(string uri)
    {
        if (uri == "about:blank") return true;
        return _deviceAuthority != null && Uri.TryCreate(uri, UriKind.Absolute, out var u)
            && u.Scheme == Uri.UriSchemeHttp
            && string.Equals(u.Authority, _deviceAuthority, StringComparison.OrdinalIgnoreCase);
    }

    private static void OpenExternal(string uri)
    {
        if (!Uri.TryCreate(uri, UriKind.Absolute, out var u) || (u.Scheme != Uri.UriSchemeHttp && u.Scheme != Uri.UriSchemeHttps)) return;
        try { Process.Start(new ProcessStartInfo(u.AbsoluteUri) { UseShellExecute = true }); } catch { }
    }

    private async Task ScanAsync()
    {
        _cts?.Cancel();
        _cts = new CancellationTokenSource();
        _list.Items.Clear();
        _scan.Enabled = false;
        _status.Text = "Scanning…";
        var seen = new HashSet<string>();
        var progress = new Progress<NucleoDevice>(d =>
        {
            if (seen.Add(d.Ip)) { _list.Items.Add(d); _status.Text = $"Found {_list.Items.Count} device(s)."; }
        });
        try { await Discovery.ScanAsync(progress, _cts.Token); }
        catch (OperationCanceledException) { }
        _status.Text = _list.Items.Count == 0 ? "No NucleoOS device found." : $"Done: {_list.Items.Count} device(s).";
        
        // Auto-connect se abbiamo trovato esattamente 1 dispositivo
        if (_list.Items.Count == 1 && _list.Items[0] is NucleoDevice singleDevice)
        {
            _status.Text += " Auto-connecting...";
            Connect(singleDevice.Ip);
        }
        
        _scan.Enabled = true;
    }

    private void Connect(string hostOrIp)
    {
        if (string.IsNullOrWhiteSpace(hostOrIp)) return;
        if (!Uri.TryCreate($"http://{hostOrIp.Trim()}/", UriKind.Absolute, out var target) || target.AbsolutePath != "/")
        {
            _status.Text = "Type a device name or IP address (e.g. 192.168.1.20).";
            return;
        }
        if (!_webReady) { _pending = hostOrIp; return; }
        _deviceAuthority = target.Authority;
        _status.Text = $"Connecting to {target.Authority}…";
        _web.Source = target;
    }

    protected override void OnClosing(CancelEventArgs e) { _cts?.Cancel(); base.OnClosing(e); }
}
