using System;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Windows.Forms;

namespace NucleoConnect;

internal static class Program
{
    [STAThread]
    private static void Main(string[] args)
    {
        // The WebView2 assemblies travel inside this exe (one-file download): resolve them from the resources
        // BEFORE any type that uses them is touched — Run() is a separate, non-inlined method for that reason.
        AppDomain.CurrentDomain.AssemblyResolve += ResolveEmbedded;
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Run(args.Length > 0 ? args[0] : null);
    }

    /// <param name="host">Optional device name/IP from the command line (a shortcut can open one device directly).</param>
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Run(string? host) => Application.Run(new MainForm(host));

    /// <summary>Per-user folder for the WebView2 profile and the unpacked native loader.</summary>
    internal static string DataDir
    {
        get
        {
            var d = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NucleoConnect");
            Directory.CreateDirectory(d);
            return d;
        }
    }

    private static Assembly? ResolveEmbedded(object? sender, ResolveEventArgs e)
    {
        var name = new AssemblyName(e.Name).Name;
        using var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("lib." + name + ".dll");
        if (s == null) return null;
        var buf = new byte[s.Length];
        int off = 0, n;
        while (off < buf.Length && (n = s.Read(buf, off, buf.Length - off)) > 0) off += n;
        return Assembly.Load(buf);
    }

    /// <summary>
    /// Writes the native WebView2Loader.dll for this process's CPU into a folder named after its hash (so a
    /// newer build never reuses an older loader, and a tampered file is never trusted) and returns the folder.
    /// </summary>
    internal static string UnpackLoader()
    {
        var arch = RuntimeInformation.ProcessArchitecture switch
        {
            Architecture.Arm64 => "arm64",
            Architecture.X86 => "x86",
            _ => "x64",
        };
        byte[] bytes;
        using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream($"native.{arch}.WebView2Loader.dll")
                       ?? throw new InvalidOperationException("WebView2Loader missing for " + arch))
        using (var ms = new MemoryStream())
        {
            s.CopyTo(ms);
            bytes = ms.ToArray();
        }
        string hash;
        using (var sha = SHA256.Create()) hash = BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-", "").Substring(0, 16);
        var dir = Path.Combine(DataDir, "loader", arch + "-" + hash);
        var file = Path.Combine(dir, "WebView2Loader.dll");
        bool ok = false;
        if (File.Exists(file))
        {
            using var sha = SHA256.Create();
            ok = BitConverter.ToString(sha.ComputeHash(File.ReadAllBytes(file))).Replace("-", "").StartsWith(hash);
        }
        if (!ok)
        {
            Directory.CreateDirectory(dir);
            var tmp = file + ".tmp";
            File.WriteAllBytes(tmp, bytes);
            if (File.Exists(file)) File.Delete(file);
            File.Move(tmp, file);
        }
        return dir;
    }
}
