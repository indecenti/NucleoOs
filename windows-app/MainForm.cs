using System.ComponentModel;
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

    private readonly Panel _side = new() { Dock = DockStyle.Left, Width = 300, Padding = new Padding(8) };
    private bool _fullScreen = false;
    private FormWindowState _prevState = FormWindowState.Normal;

    public MainForm()
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
    /// reachable as <c>window.chrome.webview.hostObjects.nucleo</c> everywhere.
    /// </summary>
    private async Task InitWebViewAsync()
    {
        await _web.EnsureCoreWebView2Async();
        var core = _web.CoreWebView2;
        core.AddHostObjectToScript("nucleo", new LocalFs());
        // Same object for every iframe, regardless of which device IP/hostname is loaded.
        core.FrameCreated += (_, e) =>
        {
            try { e.Frame.AddHostObjectToScript("nucleo", new LocalFs(), new[] { "*" }); }
            catch { /* older runtime without per-frame host objects: top-level injection still works */ }
        };
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
        _status.Text = $"Connecting to {hostOrIp}…";
        _web.Source = new Uri($"http://{hostOrIp}/");
    }

    protected override void OnClosing(CancelEventArgs e) { _cts?.Cancel(); base.OnClosing(e); }
}
