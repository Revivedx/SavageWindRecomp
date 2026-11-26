// nxpatch — reads Nintendo Switch NSP files for NSPtoNUP (built on LibHac).
// NCAs are read directly from the NSP; nothing is unpacked to disk first.
//
// Savage Wind invokes this helper from tools/package_import.py. See README.md for attribution.
//
// Title keys: common tickets (.tik) inside the NSP come first (personalized tickets cannot be used),
// then title.keys. Key values are never printed.
//
// The protocol reports stable error codes. File names and key material are kept out of status output.

using System.Buffers.Binary;
using LibHac;
using LibHac.Common;
using LibHac.Common.Keys;
using LibHac.Fs;
using LibHac.Fs.Fsa;
using LibHac.FsSystem;
using LibHac.Spl;
using LibHac.Tools.Fs;
using LibHac.Tools.FsSystem;
using LibHac.Tools.FsSystem.NcaUtils;

const string Usage = """
    Usage:
      nxpatch identify -k <prod.keys> <nsp>...
      nxpatch extract -k <prod.keys> [--titlekeys <file>] [--base <nsp>] [--update <nsp> --version <version>] [--dlc <id|version|nsp>]... --out <dir>
    """;

if (args.Length == 0) { Console.Error.WriteLine(Usage); return 2; }
string command = args[0];
var single = new Dictionary<string, string>();
var multi = new Dictionary<string, List<string>>();
var positional = new List<string>();
for (int i = 1; i < args.Length; i++)
{
    string arg = args[i];
    if (!arg.StartsWith("-")) { positional.Add(arg); continue; }
    if (i + 1 >= args.Length) { Console.Error.WriteLine($"Missing value for {arg}\n{Usage}"); return 2; }
    string value = args[++i];
    single[arg] = value;
    if (!multi.TryGetValue(arg, out var list)) multi[arg] = list = new List<string>();
    list.Add(value);
}
string? Option(string name) => single.TryGetValue(name, out var value) ? value : null;
List<string> Options(string name) => multi.TryGetValue(name, out var values) ? values : new List<string>();

string? keysPath = Option("-k");
if (keysPath is null) { Console.Error.WriteLine(Usage); return 2; }

try
{
    switch (command)
    {
        case "identify":
        {
            if (positional.Count == 0) { Console.Error.WriteLine(Usage); return 2; }
            var keySet = Keys.Load(keysPath, Option("--titlekeys"));
            foreach (string path in positional)
            {
                try
                {
                    using var package = new Package(path, keySet);
                    foreach (var meta in package.ReadMetas())
                        Console.WriteLine($"content\t{meta.Kind}\t{meta.TitleId:X16}\t{meta.Version}");
                }
                catch (Exception error)
                {
                    var failure = Failure.From(error);
                    Console.WriteLine($"error\t{failure.Code}");
                }
            }
            return 0;
        }

        case "extract":
        {
            string? baseNsp = Option("--base"), output = Option("--out");
            if (output is null) { Console.Error.WriteLine(Usage); return 2; }
            var keySet = Keys.Load(keysPath, Option("--titlekeys"));
            uint? version = Option("--version") is string value ? uint.Parse(value) : null;
            long? free = Option("--free") is string bytes ? long.Parse(bytes) : null;
            return Extractor.Run(keySet, baseNsp, Option("--update"), version, Options("--dlc"), output, free,
                                 Option("--versions") == "1", Option("--require-version"));
        }

        default:
            Console.Error.WriteLine(Usage);
            return 2;
    }
}
catch (Exception error)
{
    var failure = Failure.From(error);
    Console.Error.WriteLine($"nxpatch-error\t{failure.Code}");
    return 1;
}

/// An error with a stable code (for callers to localize), an optional detail and an English message.
sealed class Failure(string code, string detail, string message) : Exception(message)
{
    public string Code { get; } = code;
    public string Detail { get; } = detail;

    /// Maps any exception to a failure; only the reason is reported, never a stack trace or key value.
    public static Failure From(Exception error) => error switch
    {
        Failure failure => failure,
        MissingKeyException missing when missing.Type == LibHac.Common.Keys.KeyType.Title => new Failure(
            "missing_title_key", missing.Name ?? "",
            $"No title key: the NSP has no usable common ticket and title.keys has no entry (rights ID {missing.Name})"),
        MissingKeyException missing => new Failure(
            "missing_key", missing.Name ?? "",
            $"prod.keys is missing {missing.Name}; use prod.keys that match the game's firmware version"),
        _ => new Failure("unknown", "", OneLine(error.Message)),
    };

    private static string OneLine(string text) => text.Replace('\t', ' ').Replace('\n', ' ').Replace('\r', ' ');
}

static class Keys
{
    private static string? titleKeysFile;
    private static readonly HashSet<KeySet> loadedTitleKeys = new();

    /// Reads prod.keys only. title.keys is loaded after the NSP's tickets are imported,
    /// so a rights ID that already has a key from a ticket is not overwritten (tickets take priority).
    public static KeySet Load(string prodKeys, string? titleKeys)
    {
        var keySet = KeySet.CreateDefaultKeySet();
        ExternalKeyReader.ReadKeyFile(keySet, prodKeys, null, null, logger: null);
        titleKeysFile = titleKeys;
        return keySet;
    }

    public static void LoadTitleKeysOnce(KeySet keySet)
    {
        if (titleKeysFile is null || !loadedTitleKeys.Add(keySet)) return;
        ExternalKeyReader.ReadKeyFile(keySet, null, titleKeysFile, null, logger: null);
    }

    /// Common ticket (title key type 0): rights ID at body+0x160, encrypted title key at body+0x40.
    public static void ImportTicket(KeySet keySet, byte[] ticket)
    {
        if (ticket.Length < 4) return;
        int body = BinaryPrimitives.ReadUInt32LittleEndian(ticket) switch
        {
            0x10000 or 0x10003 => 0x240,
            0x10001 or 0x10004 => 0x140,
            0x10002 or 0x10005 => 0x80,
            _ => -1,
        };
        if (body < 0 || ticket.Length < body + 0x170 || ticket[body + 0x141] != 0) return;
        var rightsId = new RightsId(ticket.AsSpan(body + 0x160, 0x10));
        var key = new AccessKey(ticket.AsSpan(body + 0x40, 0x10));
        keySet.ExternalKeySet.Add(rightsId, key);
    }
}

/// One NSP: the NCAs and tickets inside its PFS0.
sealed class Package : IDisposable
{
    public readonly string Path;
    private readonly KeySet keySet;
    private readonly LocalStorage storage;
    private readonly PartitionFileSystem pfs = new();
    private readonly List<string> names;

    public Package(string path, KeySet keySet)
    {
        Path = path;
        this.keySet = keySet;
        storage = new LocalStorage(path, FileAccess.Read);
        if (pfs.Initialize(storage).IsFailure()) throw new Failure("invalid_nsp", "", "Not a valid NSP file");
        names = pfs.EnumerateEntries("/", "*").Where(e => e.Type == DirectoryEntryType.File).Select(e => e.Name).ToList();
        if (names.Any(n => n.EndsWith(".ncz")))
            throw new Failure("nsz_unsupported", "", "This is an NSZ package; decompress it to NSP first");
        foreach (string name in names.Where(n => n.EndsWith(".tik"))) Keys.ImportTicket(keySet, ReadAll(name));
        Keys.LoadTitleKeysOnce(keySet);
    }

    private IStorage OpenStorage(string name)
    {
        using var file = new UniqueRef<IFile>();
        pfs.OpenFile(ref file.Ref, ("/" + name).ToU8Span(), OpenMode.Read).ThrowIfFailure();
        return new FileStorage(file.Release());
    }

    private byte[] ReadAll(string name)
    {
        using var file = OpenStorage(name);
        file.GetSize(out long size).ThrowIfFailure();
        if (size > 64 * 1024) throw new Failure("bad_ticket", "", "Ticket is too large");
        var data = new byte[size];
        file.Read(0, data).ThrowIfFailure();
        return data;
    }

    public Nca OpenNca(string name) => new(keySet, OpenStorage(name));

    /// Finds an NCA by content type (skipping the Meta NCA); returns the largest when there are several.
    public Nca? FindNca(Meta meta, params NcaContentType[] types)
    {
        Nca? best = null;
        long bestSize = -1;
        foreach (string name in names.Where(meta.ContentNames.Contains))
        {
            var nca = OpenNca(name);
            if (!types.Contains(nca.Header.ContentType)) continue;
            nca.BaseStorage.GetSize(out long size).ThrowIfFailure();
            if (size > bestSize) { best = nca; bestSize = size; }
        }
        return best;
    }

    public record Meta(string Kind, ulong TitleId, uint Version, HashSet<string> ContentNames);

    /// cnmt header: TitleId u64 @0x0, Version u32 @0x8, Type u8 @0xC (0x80 base, 0x81 update, 0x82 DLC).
    public IEnumerable<Meta> ReadMetas()
    {
        foreach (string metaName in names.Where(n => n.EndsWith(".cnmt.nca", StringComparison.OrdinalIgnoreCase)))
        {
            var meta = ReadMeta(metaName);
            if (meta is not null) yield return meta;
        }
    }

    private Meta? ReadMeta(string metaName)
    {
        var nca = OpenNca(metaName);
        using var fs = nca.OpenFileSystem(0, IntegrityCheckLevel.None);
        var entry = fs.EnumerateEntries("/", "*.cnmt").FirstOrDefault()
            ?? throw new Failure("bad_cnmt", "", "The content metadata is damaged");
        using var file = new UniqueRef<IFile>();
        fs.OpenFile(ref file.Ref, entry.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
        var header = new byte[0x20];
        file.Get.Read(out long read, 0, header).ThrowIfFailure();
        if (read != header.Length) throw new Failure("bad_cnmt", "", "The content metadata is damaged");
        string kind = header[0x0C] switch
        {
            0x80 => "base",
            0x81 => "update",
            0x82 => "dlc",
            _ => "",
        };
        if (kind.Length == 0) return null;
        int offset = 0x20 + BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(0xE));
        int count = BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(0x10));
        var contentNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var content = new byte[0x38];
        for (int i = 0; i < count; i++)
        {
            file.Get.Read(out read, offset + i * content.Length, content).ThrowIfFailure();
            if (read != content.Length) throw new Failure("bad_cnmt", "", "Content entry is truncated");
            contentNames.Add(Convert.ToHexString(content.AsSpan(0x20, 16)) + ".nca");
        }
        return new Meta(kind, BinaryPrimitives.ReadUInt64LittleEndian(header),
                        BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(8)), contentNames);
    }

    public void Dispose() => storage.Dispose();
}

/// Copies a file system to a local directory and reports progress in bytes.
sealed class Copier : IProgressReport
{
    private readonly Action<long>? onProgress;
    private long done;

    private Copier(Action<long>? onProgress) { this.onProgress = onProgress; }

    public static long TotalSize(IFileSystem fs)
    {
        var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        long total = 0;
        foreach (var entry in fs.EnumerateEntries("/", "*", SearchOptions.RecurseSubdirectories))
        {
            string name = entry.Name;
            string stem = name.Split('.', 2)[0].TrimEnd(' ').ToUpperInvariant();
            if (name.Length == 0 || name.EndsWith('.') || name.EndsWith(' ') ||
                name.Any(c => c < ' ' || "<>:\"/\\|?*".Contains(c)) ||
                stem is "CON" or "PRN" or "AUX" or "NUL" or "CONIN$" or "CONOUT$" ||
                (stem.Length == 4 && (stem.StartsWith("COM") || stem.StartsWith("LPT")) &&
                 "123456789¹²³".Contains(stem[3])) || !paths.Add(entry.FullPath))
                throw new Failure("unsafe_path", "", "Invalid or colliding file name");
            if (entry.Type == DirectoryEntryType.File) total = checked(total + entry.Size);
        }
        return total;
    }

    public static void Copy(IFileSystem source, string directory, Action<long>? onProgress)
    {
        Directory.CreateDirectory(directory);
        using var target = new LocalFileSystem(directory);
        source.CopyDirectory(target, "/", "/", new Copier(onProgress)).ThrowIfFailure();
    }

    public void Report(long value) { }
    public void ReportAdd(long value) { done += value; onProgress?.Invoke(done); }
    public void SetTotal(long value) { }
    public void LogMessage(string message) { }
}

static class Extractor
{
    private static string ReadVersion(IFileSystem fs, string path, long offset, int length)
    {
        using var file = new UniqueRef<IFile>();
        if (fs.OpenFile(ref file.Ref, path.ToU8Span(), OpenMode.Read).IsFailure()) return "";
        if (file.Get.GetSize(out long size).IsFailure() || size <= offset) return "";
        var buffer = new byte[(int)Math.Min(length, size - offset)];
        if (file.Get.Read(out long read, offset, buffer).IsFailure()) return "";
        string version = System.Text.Encoding.UTF8.GetString(buffer, 0, (int)read)
            .Split('\0', 2)[0].TrimStart('\uFEFF').Trim();
        return Version.TryParse(version, out _) ? version : "";
    }

    private static string ControlVersion(Package package, Package.Meta meta)
    {
        var control = package.FindNca(meta, NcaContentType.Control);
        if (control is null || !control.CanOpenSection(NcaSectionType.Data)) return "";
        using var fs = control.OpenFileSystem(NcaSectionType.Data, IntegrityCheckLevel.None);
        return ReadVersion(fs, "/control.nacp", 0x3060, 0x10);  // NACP DisplayVersion
    }

    public static int Run(KeySet keySet, string? basePath, string? updatePath, uint? version, List<string> dlcPaths,
                          string output, long? free, bool versions, string? requiredVersion)
    {
        using var basePackage = basePath is null ? null : new Package(basePath, keySet);
        var baseMeta = basePackage?.ReadMetas().Single(meta => meta.Kind == "base" && meta.TitleId == 0x01007EF00011E000UL);
        var baseNca = baseMeta is null ? null : basePackage!.FindNca(baseMeta, NcaContentType.Program)
            ?? throw new Failure("nca_not_found", "program", "No Program NCA found in the base");

        var jobs = new List<(IFileSystem Fs, string Dir)>();
        var packages = new List<Package>();
        void Add(Nca nca, Nca? patch, NcaSectionType type, string dir, string kind = "", Func<string>? fallback = null)
        {
            if (versions && type == NcaSectionType.Code) return;
            bool available = nca.CanOpenSection(type) || (patch?.CanOpenSection(type) ?? false);
            if (!available) throw new Failure("missing_section", "", "Content section is missing");
            var fs = patch is null ? nca.OpenFileSystem(type, IntegrityCheckLevel.None)
                                   : nca.OpenFileSystemWithPatch(patch, type, IntegrityCheckLevel.None);
            if (kind == "update" && requiredVersion is not null &&
                ReadVersion(fs, "/System/Version.txt", 0, 64) != requiredVersion)
            {
                fs.Dispose();
                throw new Failure("bad_game_version", "", "The merged RomFS version changed");
            }
            if (!versions) { jobs.Add((fs, dir)); return; }
            using (fs)
            {
                string path = kind.StartsWith("aoc") ? "/System/AocVersion.txt" : "/System/Version.txt";
                string display = ReadVersion(fs, path, 0, 64);
                if (display.Length == 0 && fallback is not null) display = fallback();
                if (display.Length != 0) Console.WriteLine($"version\t{kind}\t{display}");
            }
        }

        if (updatePath is not null)
        {
            var package = new Package(updatePath, keySet);
            packages.Add(package);
            var meta = package.ReadMetas().Single(m => m.Kind == "update" && m.Version == version &&
                                                       m.TitleId == 0x01007EF00011E800UL);
            if (baseNca is null)
                throw new Failure("update_mismatch", System.IO.Path.GetFileName(updatePath),
                    $"{System.IO.Path.GetFileName(updatePath)} is not an update for this game");
            var patch = package.FindNca(meta, NcaContentType.Program)
                ?? throw new Failure("nca_not_found", "program", "No Program NCA found in the update");
            if (versions)
                Add(baseNca, null, NcaSectionType.Data, output, "base",
                    () => ControlVersion(basePackage!, baseMeta!));
            Add(baseNca, patch, NcaSectionType.Code, System.IO.Path.Combine(output, "exefs"));
            Add(baseNca, patch, NcaSectionType.Data, System.IO.Path.Combine(output, "romfs"), "update");
        }
        else if (baseNca is not null)
        {
            Add(baseNca, null, NcaSectionType.Code, System.IO.Path.Combine(output, "exefs"));
            Add(baseNca, null, NcaSectionType.Data, System.IO.Path.Combine(output, "romfs"), "base",
                () => ControlVersion(basePackage!, baseMeta!));
        }

        foreach (string dlcPath in dlcPaths)
        {
            var parts = dlcPath.Split('|', 3);
            ulong title = Convert.ToUInt64(parts[0], 16);
            uint dlcVersion = uint.Parse(parts[1]);
            var package = new Package(parts[2], keySet);
            packages.Add(package);
            var meta = package.ReadMetas().Single(m => m.Kind == "dlc" && m.TitleId == title && m.Version == dlcVersion
                                                      && (title == 0x01007EF00011F001UL || title == 0x01007EF00011F002UL));
            var data = package.FindNca(meta, NcaContentType.PublicData, NcaContentType.Data)
                ?? throw new Failure("nca_not_found", "data", $"No data NCA found in DLC {meta.TitleId:X16}");
            string kind = title == 0x01007EF00011F001UL ? "aoc1" : "aoc2";
            Add(data, null, NcaSectionType.Data, System.IO.Path.Combine(output, kind), kind);
        }

        if (versions) return 0;
        long total = jobs.Sum(job => Copier.TotalSize(job.Fs));
        if (free < total)
            throw new Failure("disk_space", "", "Not enough free disk space");
        Console.WriteLine($"total\t{total}");
        long finished = 0, reported = 0;
        var clock = System.Diagnostics.Stopwatch.StartNew();
        foreach (var job in jobs)
        {
            long start = finished;
            Copier.Copy(job.Fs, job.Dir, done =>
            {
                finished = start + done;
                // At most one progress line every 0.25 seconds
                if (clock.ElapsedMilliseconds - reported < 250) return;
                reported = clock.ElapsedMilliseconds;
                Console.WriteLine($"progress\t{finished}");
            });
            job.Fs.Dispose();
        }
        Console.WriteLine($"progress\t{total}");
        packages.ForEach(package => package.Dispose());
        return 0;
    }
}
