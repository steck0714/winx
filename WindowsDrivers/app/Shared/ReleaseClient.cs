// Reading and checking a release of Windows-for-Switch-Drivers, shared by
// SwitchCenter (OTA) and SwitchPatcher (PC). A release is trusted only as far
// as these checks go: HTTPS from this repository's releases, the ZIP's SHA-256
// against manifest.json, each .sys against the manifest, and each .cat signed
// by the project's test certificate.
using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Http;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Cryptography.Pkcs;
using System.Security.Principal;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Windows.Forms;
using static SwitchCenter.Lang;

namespace SwitchCenter
{
    sealed class ManifestDriver
    {
        public string Service, Inf, Hwid, Version, MinUefi, SysSha256, NameJa, NameEn;
        public bool AtNextBoot;           // replaced at the next boot instead of live (GPIO, I2C)
    }

    /// <summary>
    /// SD boot files in the drivers zip (manifest "sd", from drivers-2026.10.29): the loaders
    /// for EFI\Boot\bootaa64.efi and nvtsdhc, which SwitchPatcher puts on Windows 11 SDs only.
    /// </summary>
    sealed class SdBoot
    {
        public string Service, Inf, Version, SysSha256, Loader, LoaderSha256, LoaderNoCid, LoaderNoCidSha256;
        public int MinBuild;
    }

    sealed class Release
    {
        public string Tag, PageUrl, AppVersion, AppFile, AppSha256, AppUrl, DriversFile, DriversSha256, DriversUrl;
        public bool Prerelease;           // a GitHub pre-release: beta drivers (Make-Release.ps1 -Beta)
        public List<ManifestDriver> Drivers = new List<ManifestDriver>();
        public SdBoot Sd;                 // null in releases without it
    }

    static class ReleaseClient
    {
        public const string Repo = "hayato41200/Windows-for-Switch-Drivers";
        /// <summary>Thumbprint of "CN=SwitchWOA Test Driver Signing"; every .cat must be signed with it.</summary>
        public const string Signer = "A9E1F054DFCC96F43010918892C3C30D5A522712";
        static readonly Regex TagPattern = new Regex(@"^drivers-\d{4}\.\d{1,2}\.\d{1,2}(\.\d+)?$");
        static readonly Regex ServicePattern = new Regex(@"^[a-z0-9]{1,32}$");
        /// <summary>Drivers replaced at the next boot when the manifest has no "installAt" (releases up to drivers-2026.10.27).</summary>
        static readonly string[] AtNextBoot = { "tegra210gpio", "tegra210i2c" };
        static readonly Regex HwidPattern = new Regex(@"^ACPI\\[A-Z0-9]{4,8}$");

        public static HttpClient NewClient()
        {
            var c = new HttpClient { Timeout = TimeSpan.FromMinutes(10) };
            c.DefaultRequestHeaders.UserAgent.ParseAdd(Application.ProductName + "/" + Application.ProductVersion.Split('+')[0]);
            return c;
        }

        /// <summary>
        /// The newest release of a channel: stable = releases/latest (GitHub skips pre-releases there),
        /// beta = the newest release including pre-releases.
        /// </summary>
        public static async Task<Release> FetchChannelAsync(bool beta)
        {
            if (!beta) return await FetchAsync(null);
            string newest = null;
            Version newestVersion = null;
            using (HttpClient http = NewClient())
            using (JsonDocument list = JsonDocument.Parse(await http.GetStringAsync($"https://api.github.com/repos/{Repo}/releases?per_page=30")))
            {
                // GitHub orders the list by the tagged commit's date, not by publishing, so
                // releases tagged on the same commit come in any order: compare the tags instead.
                foreach (JsonElement rel in list.RootElement.EnumerateArray())
                {
                    string tag = rel.GetProperty("tag_name").GetString();
                    if (rel.GetProperty("draft").GetBoolean() || tag == null || !TagPattern.IsMatch(tag)) continue;
                    Version v = System.Version.Parse(tag.Substring("drivers-".Length));
                    if (newestVersion == null || v > newestVersion) { newest = tag; newestVersion = v; }
                }
            }
            if (newest == null) throw new InvalidDataException("no release found");
            return await FetchAsync(newest);
        }

        /// <summary>Reads releases/latest (tag == null) or the given tag, and its manifest.json.</summary>
        public static async Task<Release> FetchAsync(string tag)
        {
            if (tag != null && !TagPattern.IsMatch(tag)) throw new InvalidDataException("bad tag " + tag);
            using HttpClient http = NewClient();
            string api = $"https://api.github.com/repos/{Repo}/releases/" + (tag == null ? "latest" : "tags/" + tag);
            using JsonDocument rel = JsonDocument.Parse(await http.GetStringAsync(api));
            var r = new Release
            {
                Tag = rel.RootElement.GetProperty("tag_name").GetString(),
                PageUrl = rel.RootElement.GetProperty("html_url").GetString(),
                Prerelease = rel.RootElement.TryGetProperty("prerelease", out JsonElement pre) && pre.GetBoolean(),
            };
            if (!TagPattern.IsMatch(r.Tag) || (tag != null && r.Tag != tag)) throw new InvalidDataException("unexpected tag " + r.Tag);
            var assets = new Dictionary<string, string>();
            foreach (JsonElement a in rel.RootElement.GetProperty("assets").EnumerateArray())
            {
                string url = a.GetProperty("browser_download_url").GetString();
                // Only files attached to a release of this repository, over HTTPS.
                if (url.StartsWith($"https://github.com/{Repo}/releases/download/", StringComparison.Ordinal))
                    assets[a.GetProperty("name").GetString()] = url;
            }
            if (!assets.TryGetValue("manifest.json", out string manifestUrl)) throw new InvalidDataException("manifest.json is missing in " + r.Tag);

            using JsonDocument m = JsonDocument.Parse(await http.GetStringAsync(manifestUrl));
            JsonElement root = m.RootElement;
            if (root.GetProperty("release").GetString() != r.Tag) throw new InvalidDataException("manifest.json is for another release");
            JsonElement app = root.GetProperty("app");
            r.AppVersion = app.GetProperty("version").GetString();
            r.AppFile = app.GetProperty("file").GetString();
            r.AppSha256 = app.GetProperty("sha256").GetString().ToLowerInvariant();
            assets.TryGetValue(r.AppFile, out r.AppUrl);
            JsonElement drv = root.GetProperty("drivers");
            r.DriversFile = drv.GetProperty("file").GetString();
            r.DriversSha256 = drv.GetProperty("sha256").GetString().ToLowerInvariant();
            if (!assets.TryGetValue(r.DriversFile, out r.DriversUrl)) throw new InvalidDataException(r.DriversFile + " is missing in " + r.Tag);
            foreach (JsonElement i in drv.GetProperty("items").EnumerateArray())
            {
                var d = new ManifestDriver
                {
                    Service = i.GetProperty("service").GetString(),
                    Inf = i.GetProperty("inf").GetString(),
                    Hwid = i.GetProperty("hwid").GetString(),
                    Version = i.GetProperty("version").GetString(),
                    MinUefi = i.GetProperty("minUefi").GetString(),
                    SysSha256 = i.GetProperty("sysSha256").GetString().ToLowerInvariant(),
                    NameJa = i.TryGetProperty("nameJa", out JsonElement ja) ? ja.GetString() : null,
                    NameEn = i.TryGetProperty("nameEn", out JsonElement en) ? en.GetString() : null,
                };
                d.AtNextBoot = i.TryGetProperty("installAt", out JsonElement at) ? at.GetString() == "boot" : Array.IndexOf(AtNextBoot, d.Service) >= 0;
                if (!ServicePattern.IsMatch(d.Service) || !System.Version.TryParse(d.Version, out _) || !HwidPattern.IsMatch(d.Hwid))
                    throw new InvalidDataException("bad manifest entry " + d.Service);
                r.Drivers.Add(d);
            }
            if (root.TryGetProperty("sd", out JsonElement sd))
            {
                r.Sd = new SdBoot
                {
                    Service = sd.GetProperty("service").GetString(),
                    Inf = sd.GetProperty("inf").GetString(),
                    Version = sd.GetProperty("version").GetString(),
                    SysSha256 = sd.GetProperty("sysSha256").GetString().ToLowerInvariant(),
                    MinBuild = sd.GetProperty("minBuild").GetInt32(),
                    Loader = sd.GetProperty("loader").GetProperty("file").GetString(),
                    LoaderSha256 = sd.GetProperty("loader").GetProperty("sha256").GetString().ToLowerInvariant(),
                    LoaderNoCid = sd.GetProperty("loaderNoCid").GetProperty("file").GetString(),
                    LoaderNoCidSha256 = sd.GetProperty("loaderNoCid").GetProperty("sha256").GetString().ToLowerInvariant(),
                };
                if (!ServicePattern.IsMatch(r.Sd.Service) || !System.Version.TryParse(r.Sd.Version, out _))
                    throw new InvalidDataException("bad manifest entry sd");
            }
            return r;
        }

        /// <summary>
        /// Checks the SD boot files unpacked under files: nvtsdhc like any driver, and both
        /// loaders against the manifest. Returns the full paths (.inf, loader, NOCID loader).
        /// </summary>
        public static (string Inf, string Loader, string LoaderNoCid) VerifySd(string files, SdBoot sd)
        {
            string inf = VerifyDriver(files, new ManifestDriver { Service = sd.Service, Inf = sd.Inf, SysSha256 = sd.SysSha256 });
            string Checked(string rel, string sha256)
            {
                string p = Path.GetFullPath(Path.Combine(files, rel));
                if (!p.StartsWith(files + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || !File.Exists(p) || Sha256(p) != sha256)
                    throw new InvalidDataException(T($"{rel} が manifest と合いません。", $"{rel} does not match the manifest."));
                return p;
            }
            return (inf, Checked(sd.Loader, sd.LoaderSha256), Checked(sd.LoaderNoCid, sd.LoaderNoCidSha256));
        }

        /// <summary>Downloads url to path and checks its SHA-256 (lower-case hex).</summary>
        public static async Task DownloadAsync(string url, string path, string sha256)
        {
            using (HttpClient http = NewClient())
            using (Stream src = await http.GetStreamAsync(url))
            using (FileStream dst = File.Create(path))
                await src.CopyToAsync(dst);
            string hash = Sha256(path);
            if (hash != sha256)
                throw new InvalidDataException(T($"{Path.GetFileName(path)} の SHA-256 が manifest と合いません（{hash}）。",
                                                 $"The SHA-256 of {Path.GetFileName(path)} does not match the manifest ({hash})."));
        }

        /// <summary>Checks one driver unpacked under files: its .inf exists, the .sys matches the manifest and the .cat signer. Returns the .inf.</summary>
        public static string VerifyDriver(string files, ManifestDriver d)
        {
            string inf = Path.GetFullPath(Path.Combine(files, d.Inf));
            if (!inf.StartsWith(files + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || !File.Exists(inf))
                throw new InvalidDataException(d.Inf + T(" がありません。", " is missing."));
            string dir = Path.GetDirectoryName(inf);
            string sys = Path.Combine(dir, d.Service + ".sys"), cat = Path.Combine(dir, d.Service + ".cat");
            if (!File.Exists(sys) || Sha256(sys) != d.SysSha256)
                throw new InvalidDataException(T($"{d.Service}.sys が manifest と合いません。", $"{d.Service}.sys does not match the manifest."));
            CheckCatalogSigner(cat);
            return inf;
        }

        public static string Sha256(string path)
        {
            using FileStream f = File.OpenRead(path);
            return Convert.ToHexString(SHA256.HashData(f)).ToLowerInvariant();
        }

        /// <summary>The catalog must be a valid PKCS #7 signature by the project's test certificate.</summary>
        public static void CheckCatalogSigner(string cat)
        {
            if (!File.Exists(cat)) throw new InvalidDataException(Path.GetFileName(cat) + T(" がありません。", " is missing."));
            var cms = new SignedCms();
            cms.Decode(File.ReadAllBytes(cat));
            cms.CheckSignature(true);                                    // the signature itself; trust is Windows' job
            if (cms.SignerInfos.Count != 1 || !string.Equals(cms.SignerInfos[0].Certificate?.Thumbprint, Signer, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException(T($"{Path.GetFileName(cat)} の署名が、このプロジェクトの証明書ではありません。",
                                                 $"{Path.GetFileName(cat)} is not signed with this project's certificate."));
        }

        /// <summary>A new folder under %windir%\Temp that only SYSTEM and Administrators can access.</summary>
        public static string CreateAdminOnlyFolder()
        {
            var sec = new DirectorySecurity();
            sec.SetAccessRuleProtection(true, false);
            foreach (WellKnownSidType sid in new[] { WellKnownSidType.LocalSystemSid, WellKnownSidType.BuiltinAdministratorsSid })
                sec.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(sid, null), FileSystemRights.FullControl,
                    InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit, PropagationFlags.None, AccessControlType.Allow));
            string path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "Temp", Application.ProductName + "-" + Guid.NewGuid().ToString("N"));
            sec.CreateDirectory(path);
            return path;
        }
    }
}
