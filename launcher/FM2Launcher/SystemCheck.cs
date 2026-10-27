using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace FM2Launcher;

/// <summary>Whether this PC's Direct3D 12 can map the game's memory for the GPU. The game records what it
/// got (zero_copy.txt in the user folder, after its own self-test); before its first start, a device with
/// ID3D12Device13 (Windows 11 23H2+, a 2024+ driver) or ID3D12Device3 (Windows 10 1709+) counts as able.
/// Without it the game copies vertex data every frame: slower, the reason behind most "lower fps" reports.</summary>
internal static class SystemCheck
{
    public sealed record Result(bool ZeroCopy, string Os, string Gpu, string Driver);

    static readonly Lazy<Result> result = new(Probe);
    public static Result Get() => result.Value;

    [DllImport("d3d12.dll")]
    static extern int D3D12CreateDevice(IntPtr adapter, int minimumFeatureLevel, ref Guid riid, out IntPtr device);

    static readonly Guid IID_ID3D12Device = new("189819f1-1db6-4b57-be54-1821339b85f7");
    static readonly Guid IID_ID3D12Device13 = new("14eecffc-4df8-40f7-a118-5c816f45695e");
    static readonly Guid IID_ID3D12Device3 = new("81dadc15-2bad-4392-93c5-101345c4aa98");
    const int D3D_FEATURE_LEVEL_12_0 = 0xc000;

    static Result Probe() => OperatingSystem.IsWindows() ? ProbeWindows() : ProbeLinux();

    /// <summary>Linux: vkd3d-proton gives the game what it needs, so it counts as able until the game's own self-test
    /// (zero_copy.txt) says otherwise; the distribution, kernel and GPU for the report page.</summary>
    static Result ProbeLinux()
    {
        bool zeroCopy = true;
        try
        {
            var recorded = Path.Combine(Paths.User, "zero_copy.txt");
            if (File.Exists(recorded)) zeroCopy = File.ReadAllText(recorded).Trim() == "1";
        }
        catch { }
        string os = "Linux";
        try
        {
            var pretty = File.ReadLines("/etc/os-release").FirstOrDefault(l => l.StartsWith("PRETTY_NAME="));
            if (pretty is not null) os = pretty["PRETTY_NAME=".Length..].Trim('"');
            os += $" (kernel {File.ReadAllText("/proc/sys/kernel/osrelease").Trim()})";
        }
        catch { }
        string gpu = "", driver = "";
        try
        {
            var psi = new System.Diagnostics.ProcessStartInfo("lspci") { RedirectStandardOutput = true, UseShellExecute = false };
            using var p = System.Diagnostics.Process.Start(psi)!;
            var line = p.StandardOutput.ReadToEnd().Split('\n').FirstOrDefault(l => l.Contains("VGA") || l.Contains("3D controller"));
            if (line is not null) gpu = line[(line.IndexOf(':', line.IndexOf(' ')) + 1)..].Trim();
        }
        catch { }
        foreach (var module in new[] { "nvidia", "amdgpu", "i915", "xe" })
            try
            {
                var version = $"/sys/module/{module}/version";
                if (Directory.Exists($"/sys/module/{module}"))
                {
                    driver = File.Exists(version) ? $"{module} {File.ReadAllText(version).Trim()}" : module;
                    break;
                }
            }
            catch { }
        return new Result(zeroCopy, os, gpu, driver);
    }

    [System.Runtime.Versioning.SupportedOSPlatform("windows")]
    static Result ProbeWindows()
    {
        bool zeroCopy = false;
        try
        {
            var iid = IID_ID3D12Device;
            if (D3D12CreateDevice(IntPtr.Zero, D3D_FEATURE_LEVEL_12_0, ref iid, out var device) >= 0 && device != IntPtr.Zero)
            {
                foreach (var id in new[] { IID_ID3D12Device13, IID_ID3D12Device3 })
                {
                    var iid13 = id;
                    if (Marshal.QueryInterface(device, ref iid13, out var d13) >= 0)
                    {
                        zeroCopy = true;
                        Marshal.Release(d13);
                        break;
                    }
                }
                Marshal.Release(device);
            }
        }
        catch { } // no D3D12 at all: reported as unavailable
        // What the game got last time it ran, when it has run (its self-test is the real answer).
        try
        {
            var recorded = Path.Combine(Paths.User, "zero_copy.txt");
            if (File.Exists(recorded))
                zeroCopy = File.ReadAllText(recorded).Trim() == "1";
        }
        catch { }

        string os = "Windows";
        try
        {
            using var key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion");
            var build = int.TryParse(key?.GetValue("CurrentBuildNumber") as string, out var b) ? b : Environment.OSVersion.Version.Build;
            var name = build >= 22000 ? "Windows 11" : "Windows 10";
            os = $"{name} {key?.GetValue("DisplayVersion") ?? ""} (build {build}.{key?.GetValue("UBR") ?? 0})".Replace("  ", " ");
        }
        catch { }

        string gpu = "", driver = "";
        try
        {
            // The display adapter class: the first one with a driver, as Device Manager lists them.
            using var cls = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}");
            foreach (var sub in cls?.GetSubKeyNames() ?? [])
            {
                if (!int.TryParse(sub, out _))
                    continue;
                using var k = cls!.OpenSubKey(sub);
                if (k?.GetValue("DriverDesc") is string desc && !desc.Contains("Basic", StringComparison.OrdinalIgnoreCase))
                {
                    gpu = desc;
                    driver = $"{k.GetValue("DriverVersion")} ({k.GetValue("DriverDate")})";
                    break;
                }
            }
        }
        catch { }
        return new Result(zeroCopy, os, gpu, driver);
    }
}
