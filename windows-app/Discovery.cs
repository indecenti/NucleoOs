using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text.Json;

namespace NucleoConnect;

/// <summary>A NucleoOS device found on the LAN.</summary>
public record NucleoDevice(string Ip, string Version, string Fs, long FreeBytes, long TotalBytes)
{
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
            var json = await http.GetStringAsync($"http://{ip}/api/status", ct);
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            if (root.GetProperty("os").GetString() != "NucleoOS") return;
            var st = root.GetProperty("storage");
            found.Report(new NucleoDevice(
                ip,
                root.GetProperty("version").GetString() ?? "?",
                st.GetProperty("fs").GetString() ?? "?",
                st.GetProperty("free_bytes").GetInt64(),
                st.GetProperty("total_bytes").GetInt64()));
        }
        catch { /* not a NucleoOS host / unreachable / timeout */ }
        finally { gate.Release(); }
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
