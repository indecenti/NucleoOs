using System;
using System.Collections.Generic;
using System.Net.Http;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;

namespace NucleoConnect;

/// <summary>A NucleoOS device found on the LAN.</summary>
public sealed class NucleoDevice
{
    public NucleoDevice(string ip, string version, string fs, long freeBytes, long totalBytes)
    { Ip = ip; Version = version; Fs = fs; FreeBytes = freeBytes; TotalBytes = totalBytes; }
    public string Ip { get; }
    public string Version { get; }
    public string Fs { get; }
    public long FreeBytes { get; }
    public long TotalBytes { get; }

    public override string ToString()
        => $"{Ip}   ·   v{Version}   ·   {Fs} {FreeBytes / 1_000_000_000.0:F1} GB free";
}

/// <summary>
/// Discovers devices by probing GET /api/status across local /24 subnets.
/// Dependency-free and light on the device: each host is hit at most once with a
/// short timeout. The OS must be listening (HTTP server up) to be found.
/// </summary>
public static class Discovery
{
    public static async Task ScanAsync(IProgress<NucleoDevice> found, CancellationToken ct)
    {
        using var http = new HttpClient { Timeout = TimeSpan.FromMilliseconds(600) };
        using var gate = new SemaphoreSlim(64);
        var tasks = new List<Task>();

        foreach (var prefix in LocalPrefixes())
            for (int host = 1; host <= 254; host++)
                tasks.Add(Probe(http, gate, $"{prefix}{host}", found, ct));

        await Task.WhenAll(tasks);
    }

    private static async Task Probe(HttpClient http, SemaphoreSlim gate, string ip,
                                    IProgress<NucleoDevice> found, CancellationToken ct)
    {
        await gate.WaitAsync(ct);
        try
        {
            using var res = await http.GetAsync($"http://{ip}/api/status", ct);
            if (!res.IsSuccessStatusCode) return;
            var json = await res.Content.ReadAsStringAsync();
            if (!(new JavaScriptSerializer().DeserializeObject(json) is Dictionary<string, object> root)) return;
            if (!(root.TryGetValue("os", out var os) && os as string == "NucleoOS")) return;
            var st = root.TryGetValue("storage", out var so) ? so as Dictionary<string, object> : null;
            found.Report(new NucleoDevice(
                ip,
                root.TryGetValue("version", out var v) ? v as string ?? "?" : "?",
                st != null && st.TryGetValue("fs", out var fs) ? fs as string ?? "?" : "?",
                Num(st, "free_bytes"),
                Num(st, "total_bytes")));
        }
        catch { /* not a NucleoOS host / unreachable / timeout */ }
        finally { gate.Release(); }
    }

    private static long Num(Dictionary<string, object>? d, string key)
    {
        if (d == null || !d.TryGetValue(key, out var v) || v == null) return 0;
        try { return Convert.ToInt64(v); } catch { return 0; }
    }

    /// <summary>Distinct "a.b.c." prefixes for each up, non-loopback IPv4 interface.</summary>
    private static IEnumerable<string> LocalPrefixes()
    {
        var seen = new HashSet<string>();
        foreach (var ni in NetworkInterface.GetAllNetworkInterfaces())
        {
            if (ni.OperationalStatus != OperationalStatus.Up ||
                ni.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;
            foreach (var ua in ni.GetIPProperties().UnicastAddresses)
            {
                if (ua.Address.AddressFamily != AddressFamily.InterNetwork) continue;
                var b = ua.Address.GetAddressBytes();
                var prefix = $"{b[0]}.{b[1]}.{b[2]}.";
                if (seen.Add(prefix)) yield return prefix;
            }
        }
    }
}
