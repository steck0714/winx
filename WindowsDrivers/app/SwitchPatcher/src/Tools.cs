// Running the Windows tools (dism, bcdedit, bcdboot, reg) and PowerShell's
// Storage / DISM cmdlets, with their output going to the log.
using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;

namespace SwitchPatcher
{
    static class Tools
    {
        public static string System32(string exe) => Path.Combine(Environment.SystemDirectory, exe);

        /// <summary>
        /// Runs exe and returns its exit code. Each output line goes to log, except
        /// progress lines ("[==== 12.0% ]") which go to progress.
        /// </summary>
        public static async Task<int> Run(string exe, string args, Action<string> log, Action<string> progress = null)
        {
            Encoding oem = Encoding.GetEncoding(CultureInfo.CurrentCulture.TextInfo.OEMCodePage);
            var psi = new ProcessStartInfo(exe, args)
            {
                UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = oem, StandardErrorEncoding = oem,
            };
            log("> " + Path.GetFileName(exe) + " " + args);
            using Process p = Process.Start(psi);
            Task a = Pump(p.StandardOutput, log, progress), b = Pump(p.StandardError, log, progress);
            await p.WaitForExitAsync();
            await Task.WhenAll(a, b);
            if (p.ExitCode != 0) log($"  (exit {p.ExitCode})");
            return p.ExitCode;
        }

        static async Task Pump(StreamReader r, Action<string> log, Action<string> progress)
        {
            string line;
            while ((line = await r.ReadLineAsync()) != null)
            {
                line = line.Trim();
                if (line.Length == 0) continue;
                if (line.StartsWith("[") && line.Contains("%")) { progress?.Invoke(line); continue; }
                log("  " + line);
            }
        }

        /// <summary>Runs a PowerShell script (Windows PowerShell 5.1, for the Storage and DISM modules) and returns its standard output.</summary>
        public static async Task<string> PowerShell(string script)
        {
            string full = "$ErrorActionPreference = 'Stop'; $ProgressPreference = 'SilentlyContinue'; " +
                          "[Console]::OutputEncoding = [Text.Encoding]::UTF8; " + script;
            var psi = new ProcessStartInfo(System32(@"WindowsPowerShell\v1.0\powershell.exe"),
                "-NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " + Convert.ToBase64String(Encoding.Unicode.GetBytes(full)))
            {
                UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
            };
            using Process p = Process.Start(psi);
            Task<string> output = p.StandardOutput.ReadToEndAsync(), error = p.StandardError.ReadToEndAsync();
            await p.WaitForExitAsync();
            if (p.ExitCode != 0) throw new InvalidOperationException("PowerShell: " + (await error).Trim());
            return await output;
        }

        public static async Task<JsonDocument> PowerShellJson(string script) =>
            JsonDocument.Parse(await PowerShell(script));

        /// <summary>A string for a single-quoted PowerShell literal.</summary>
        public static string Quote(string s) => "'" + s.Replace("'", "''") + "'";
    }
}
