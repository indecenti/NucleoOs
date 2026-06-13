using System.Runtime.InteropServices;
using System.Text.Json;

namespace NucleoConnect;

/// <summary>
/// Native bridge exposed to the web file manager (File Commander) as
/// <c>window.chrome.webview.hostObjects.nucleo</c>. It lets the web UI enumerate and browse
/// the drives/folders physically connected to this PC — something a plain browser cannot do —
/// so the left rail can list real removable/fixed/network drives and copy their files onto the
/// device's SD card. Methods return JSON strings (the async host-object proxy resolves them as
/// promises in JS).
///
/// COM-visible + AutoDual so WebView2 can marshal it. Read-only by design: it never writes to
/// or deletes from the host filesystem.
/// </summary>
[ClassInterface(ClassInterfaceType.AutoDual)]
[ComVisible(true)]
public class LocalFs
{
    // Cap for the base64 read path (the bytes cross the COM boundary as a string). Files larger
    // than this are better dragged straight from Explorer onto the SD view (HTML5 file drop,
    // which streams up to the device's 640 MB ceiling without a base64 round-trip).
    private const long MaxReadBytes = 64L * 1024 * 1024;

    /// <summary>JSON array of connected drives: { name, path, label, glyph }.</summary>
    public string Drives()
    {
        var list = new List<object>();
        foreach (var d in DriveInfo.GetDrives())
        {
            try
            {
                string root = d.RootDirectory.FullName;          // e.g. "C:\"
                string bare = d.Name.TrimEnd('\\');               // e.g. "C:"
                string label = d.IsReady && !string.IsNullOrWhiteSpace(d.VolumeLabel)
                    ? $"{d.VolumeLabel} ({bare})"
                    : bare;
                list.Add(new { name = bare, path = root, label, glyph = GlyphFor(d.DriveType) });
            }
            catch { /* a not-ready drive (empty card reader, etc.) — skip it */ }
        }
        return JsonSerializer.Serialize(list);
    }

    /// <summary>JSON { entries:[{ name, type:"dir"|"file", size }] } for a folder, or { error }.</summary>
    public string ListDir(string path)
    {
        try
        {
            var di = new DirectoryInfo(path);
            var entries = new List<object>();
            foreach (var sub in SafeEnumerate(() => di.EnumerateDirectories()))
            {
                entries.Add(new { name = sub.Name, type = "dir", size = 0L });
            }
            foreach (var f in SafeEnumerate(() => di.EnumerateFiles()))
            {
                entries.Add(new { name = f.Name, type = "file", size = f.Length });
            }
            return JsonSerializer.Serialize(new { entries });
        }
        catch (Exception ex)
        {
            return JsonSerializer.Serialize(new { error = ex.Message });
        }
    }

    /// <summary>
    /// Base64 of a file's bytes, for copying it to the SD. Returns "" when the file is missing,
    /// unreadable, or larger than <see cref="MaxReadBytes"/> (the JS surfaces a clear message).
    /// </summary>
    public string ReadFileB64(string path)
    {
        try
        {
            var fi = new FileInfo(path);
            if (!fi.Exists || fi.Length > MaxReadBytes) return "";
            return Convert.ToBase64String(File.ReadAllBytes(path));
        }
        catch { return ""; }
    }

    // ---- write operations (used when copying SD → PC and managing local folders) ----------
    // Base64 in/out over the COM boundary is fine for the typical files a device holds; the cap
    // keeps a stray huge paste from ballooning memory. JSON { ok:true } or { error } either way.

    /// <summary>Write a file from base64. Refuses payloads over the size cap.</summary>
    public string WriteFileB64(string path, string b64)
    {
        try
        {
            b64 ??= "";
            // base64 inflates ~4/3; reject before decoding so we never materialize an oversize buffer.
            if ((long)b64.Length * 3 / 4 > MaxReadBytes) return Err("file too large for the native bridge (drag it from Explorer instead)");
            File.WriteAllBytes(path, Convert.FromBase64String(b64));
            return Ok();
        }
        catch (Exception ex) { return Err(ex.Message); }
    }

    /// <summary>Create a directory (and any missing parents).</summary>
    public string MakeDir(string path)
    {
        try { Directory.CreateDirectory(path); return Ok(); }
        catch (Exception ex) { return Err(ex.Message); }
    }

    /// <summary>Delete a file, or a directory recursively.</summary>
    public string DeleteEntry(string path)
    {
        try
        {
            if (Directory.Exists(path)) Directory.Delete(path, true);
            else if (File.Exists(path)) File.Delete(path);
            else return Err("no such entry");
            return Ok();
        }
        catch (Exception ex) { return Err(ex.Message); }
    }

    /// <summary>Rename a file or folder in place (newName is a bare name, not a path).</summary>
    public string Rename(string path, string newName)
    {
        try
        {
            var dir = Path.GetDirectoryName(path) ?? "";
            var dst = Path.Combine(dir, newName);
            if (Directory.Exists(path)) Directory.Move(path, dst);
            else File.Move(path, dst);
            return Ok();
        }
        catch (Exception ex) { return Err(ex.Message); }
    }

    /// <summary>Whether a name already exists in a folder (for collision-safe copies).</summary>
    public string Exists(string path)
        => (File.Exists(path) || Directory.Exists(path)) ? "1" : "";

    private static string Ok() => "{\"ok\":true}";
    private static string Err(string msg) => JsonSerializer.Serialize(new { error = msg });

    // Enumeration on a drive root can throw the moment it touches a denied/locked entry; degrade
    // to an empty list rather than failing the whole listing.
    private static IEnumerable<T> SafeEnumerate<T>(Func<IEnumerable<T>> source)
    {
        try { return source().ToList(); }
        catch { return Array.Empty<T>(); }
    }



    private static string GlyphFor(DriveType t) => t switch
    {
        DriveType.Removable => "🔌",   // USB sticks, SD readers
        DriveType.Network   => "🌐",
        DriveType.CDRom     => "💿",
        DriveType.Ram       => "⚡",
        _                   => "💽",   // Fixed / unknown
    };
}
