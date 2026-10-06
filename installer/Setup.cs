// FH1Installer - what the game needs from Windows, where the port comes from, and the launchers.
using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Reflection;

namespace Fh1Installer {

// What must be on the PC before the game (and the installer's own shader step, which runs fh1.exe) can work.
public static class Needs {
  // Microsoft's own download address of the Visual C++ runtime (x64) for Visual Studio 2015-2022.
  public const string RuntimeUrl = "https://aka.ms/vs/17/release/vc_redist.x64.exe";

  static bool InSystem(string name) {
    return File.Exists(Path.Combine(Environment.SystemDirectory, name));
  }

  // fh1.exe is linked against these three.
  public static bool HasRuntime() {
    return InSystem("vcruntime140.dll") && InSystem("vcruntime140_1.dll") && InSystem("msvcp140.dll");
  }

  // They come with the graphics driver (Vulkan) and with Windows 10 / 11 (Direct3D 12).
  public static bool HasVulkan() { return InSystem("vulkan-1.dll"); }
  public static bool HasDirect3D12() { return InSystem("D3D12.dll"); }

  public static string Describe() {
    string text = HasRuntime() ? "Microsoft Visual C++ runtime: installed." : "Microsoft Visual C++ runtime: MISSING (the game needs it).";
    text += HasDirect3D12() ? " Direct3D 12: there." : " Direct3D 12: MISSING (the game needs Windows 10 or 11).";
    text += HasVulkan() ? " Vulkan: there."
                        : " Vulkan: MISSING (FH1.exe draws with it: install your graphics card's current driver; " +
                          "until then only \"FH1 (emulated Direct3D 12).bat\" works).";
    return text;
  }

  // Downloads Microsoft's installer of the runtime and runs it (Windows asks for permission). Returns an
  // error text, or null.
  public static string InstallRuntime(Action<string> stage) {
    string file = Path.Combine(Path.GetTempPath(), "fh1_vc_redist.x64.exe");
    try {
      stage("Downloading the Visual C++ runtime from Microsoft...");
      Web.Download(RuntimeUrl, file, delegate { }, delegate { return false; });
      stage("Installing the Visual C++ runtime (Windows asks for permission)...");
      ProcessStartInfo info = new ProcessStartInfo(file, "/install /passive /norestart");
      info.UseShellExecute = true;  // lets Windows show its permission prompt
      using (Process p = Process.Start(info)) {
        p.WaitForExit();
        // 0 = installed, 3010 = installed, restart wanted, 1638 = a newer one is there already.
        if (p.ExitCode != 0 && p.ExitCode != 3010 && p.ExitCode != 1638) {
          return "Microsoft's installer ended with code " + p.ExitCode + ".";
        }
      }
      return HasRuntime() ? null : "The runtime is still not found after installing it.";
    } catch (Exception e) {
      return e.Message;
    } finally {
      try { File.Delete(file); } catch (IOException) { } catch (UnauthorizedAccessException) { }
    }
  }
}

public static class Web {
  // Downloads to `file` (under another name until complete). `progress(done, total)`: total -1 when unknown.
  public static void Download(string url, string file, Action<long, long> progress, Func<bool> cancelled) {
    ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
    string part = file + ".part";
    WebRequest request = WebRequest.Create(url);
    HttpWebRequest http = request as HttpWebRequest;
    if (http != null) http.UserAgent = "FH1Installer/" + Program.Version;
    using (WebResponse response = request.GetResponse())
    using (Stream input = response.GetResponseStream())
    using (FileStream output = new FileStream(part, FileMode.Create, FileAccess.Write, FileShare.None)) {
      long total = response.ContentLength;
      byte[] buffer = new byte[256 * 1024];
      long done = 0;
      for (;;) {
        if (cancelled()) throw new OperationCanceledException();
        int n = input.Read(buffer, 0, buffer.Length);
        if (n <= 0) break;
        output.Write(buffer, 0, n);
        done += n;
        progress(done, total);
      }
      if (total > 0 && done != total) throw new IOException("The download ended early (" + done + " of " + total + " bytes).");
    }
    if (File.Exists(file)) File.Delete(file);
    File.Move(part, file);
  }
}

// Where the port and the shader tools come from: the folder "package" next to FH1Installer.exe when it is there
// (a developer's build), otherwise the two zips of the release.
public static class Source {
  // Set by --source (tests): another address or folder that holds the two zips.
  public static string Base = "https://github.com/AndryTheBeast/fh1-recomp/releases/download/alpha-" + Program.Version + "/";
  public const string PortZip = "fh1-win64.zip";
  public const string ToolsZip = "fh1-shader-tools.zip";

  public static string LocalPackage() {
    string package = Path.Combine(Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location), "package");
    return File.Exists(Path.Combine(package, "port", "fh1.exe")) ? package : null;
  }

  // Puts the port into `folder` and the shader tools into `folder`\tools. `progress(fraction, text)`.
  public static void Get(string folder, Action<double, string> progress, Func<bool> cancelled) {
    string package = LocalPackage();
    string download = Path.Combine(folder, "download");
    try {
      if (package == null) {
        if (Directory.Exists(download)) Directory.Delete(download, true);
        Directory.CreateDirectory(download);
        string[] zips = { PortZip, ToolsZip };
        string[] parts = { "port", "tools" };
        for (int i = 0; i < 2; i++) {
          string zip = Path.Combine(download, zips[i]);
          string address = Base.EndsWith("/") || Base.EndsWith("\\") ? Base + zips[i] : Base + "/" + zips[i];
          int index = i;
          try {
            Web.Download(new Uri(address).AbsoluteUri, zip, delegate(long done, long total) {
              progress(total > 0 ? (index + (double)done / total) / 2 : index / 2.0,
                       "Downloading " + zips[index] + ": " + Checks.Size(done) + (total > 0 ? " of " + Checks.Size(total) : ""));
            }, cancelled);
          } catch (WebException e) {
            // Plain words instead of "The remote server returned an error: (404) Not Found."
            HttpWebResponse response = e.Response as HttpWebResponse;
            if (response != null && response.StatusCode == HttpStatusCode.NotFound) {
              throw new InvalidOperationException("could not find " + zips[i] + " of version " + Program.Version +
                                                  " on GitHub. Get the newest FH1Installer.exe from the release page.");
            }
            throw new InvalidOperationException("could not download " + zips[i] + " (" + e.Message +
                                                "). Check the internet connection and click Install again.");
          }
          ZipFile.ExtractToDirectory(zip, Path.Combine(download, parts[i]));
        }
        package = download;
      }
      progress(1, "Copying the port...");
      foreach (string file in Directory.GetFiles(Path.Combine(package, "port"))) {
        string name = Path.GetFileName(file);
        // The program is installed as FH1.exe (Windows keeps the old spelling of a file it overwrites).
        if (string.Equals(name, "fh1.exe", StringComparison.OrdinalIgnoreCase)) {
          name = Launchers.Exe;
          string old = Path.Combine(folder, name);
          if (File.Exists(old)) File.Delete(old);
        }
        File.Copy(file, Path.Combine(folder, name), true);
      }
      string tools = Path.Combine(folder, "tools");
      Directory.CreateDirectory(tools);
      foreach (string file in Directory.GetFiles(Path.Combine(package, "tools"))) {
        File.Copy(file, Path.Combine(tools, Path.GetFileName(file)), true);
      }
    } finally {
      try {
        if (Directory.Exists(download)) Directory.Delete(download, true);
      } catch (IOException) {
      }
    }
  }
}

// The installer's options, kept in the port's own settings file next to FH1.exe (fh1.toml: one "name = value"
// per line, read at every start and rewritten by the F4 menu). Only the option's own line is touched.
public static class Options {
  public const string SettingsName = "fh1.toml";
  const string Fps60 = "fh1_fps60";

  static bool IsLine(string line, string name) {
    string t = line.TrimStart();
    return t.StartsWith(name, StringComparison.Ordinal) && t.Substring(name.Length).TrimStart().StartsWith("=");
  }

  public static bool ReadFps60(string folder) {
    try {
      string path = Path.Combine(folder, SettingsName);
      if (!File.Exists(path)) return false;
      foreach (string line in File.ReadAllLines(path)) {
        if (IsLine(line, Fps60)) return line.Substring(line.IndexOf('=') + 1).Trim().StartsWith("true");
      }
    } catch (Exception) {
    }
    return false;
  }

  public static void WriteFps60(string folder, bool on) {
    string path = Path.Combine(folder, SettingsName);
    System.Collections.Generic.List<string> lines = new System.Collections.Generic.List<string>();
    if (File.Exists(path)) {
      foreach (string line in File.ReadAllLines(path)) {
        if (!IsLine(line, Fps60)) lines.Add(line);
      }
    } else if (!on) {
      return;  // nothing to say: the port's default is off
    }
    if (on) lines.Add(Fps60 + " = true");
    File.WriteAllLines(path, lines.ToArray());
  }
}

// How the game is started: FH1.exe by itself draws with the native Vulkan renderer (the default since
// 2026-10-06, the user's decision); two .bat files in the folder start the emulated Xbox 360 GPU, on
// Direct3D 12 and on Vulkan.
public static class Launchers {
  public const string Exe = "FH1.exe";
  public const string Direct3DBat = "FH1 (emulated Direct3D 12).bat";
  public const string VulkanBat = "FH1 (emulated Vulkan).bat";
  // What earlier versions of the installer wrote: removed by an update.
  static readonly string[] OldFiles = { "FH1 (native Vulkan, experimental).bat" };
  public const string ShortcutName = "Forza Horizon.lnk";
  public const string ReadMeName = "Read me.txt";

  const string ReadMe =
      "Forza Horizon (Xbox 360) for Windows - unofficial port, pre-release\r\n" +
      "\r\n" +
      "Start the game\r\n" +
      "  FH1.exe (or the \"Forza Horizon\" shortcut on the desktop)\r\n" +
      "      the default: the native Vulkan renderer (lighter on the PC; it still has some picture\r\n" +
      "      faults, and its first start shows \"Preparing shaders\" for some tens of seconds)\r\n" +
      "  FH1 (emulated Direct3D 12).bat\r\n" +
      "      the Xbox 360's exact picture, drawn through Direct3D 12: use it if something looks wrong\r\n" +
      "  FH1 (emulated Vulkan).bat\r\n" +
      "      the same exact picture drawn through Vulkan\r\n" +
      "\r\n" +
      "The first run is rough: the first time the game shows something new it stutters, and some objects\r\n" +
      "appear a moment late (the shaders are prepared on your PC right then, once). The same place is fine\r\n" +
      "the next time, and the game gets smoother the more you play.\r\n" +
      "\r\n" +
      "In the game: F3 shows the frame rate, F4 the port's settings. The game runs at 30 frames per second,\r\n" +
      "its own limit on the Xbox 360. Your saves are in your Documents folder, in \"fh1\".\r\n" +
      "\r\n" +
      "60 frames per second (experimental, native renderer): tick it in the installer (start FH1Installer.exe\r\n" +
      "again, choose this folder, Update), or untick it there if something moves too fast or looks wrong. The\r\n" +
      "game then draws and moves 60 times a second where your PC is fast enough, and less where it is not.\r\n" +
      "The choice is the line fh1_fps60 in fh1.toml here.\r\n" +
      "\r\n" +
      "Something went wrong?\r\n" +
      "  Every start of the game writes a log into the folder \"logs\" here (fh1_001.log, fh1_002.log, ...).\r\n" +
      "  Open an issue at https://github.com/AndryTheBeast/fh1-recomp/issues and attach the newest one,\r\n" +
      "  plus logs\\fh1.crash.txt if the game crashed. A log can contain your Windows user name inside\r\n" +
      "  file paths. Never attach files of the game itself.\r\n" +
      "\r\n" +
      "Guide and known issues: https://github.com/AndryTheBeast/fh1-recomp/blob/main/docs/install.md\r\n" +
      "To update: start a newer FH1Installer.exe and choose this same folder.\r\n";

  static string Bat(string what, string option) {
    return "@echo off\r\n" +
           "rem Forza Horizon recomp: " + what + "\r\n" +
           "rem (FH1.exe by itself uses the native Vulkan renderer.)\r\n" +
           "cd /d \"%~dp0\"\r\n" +
           "start \"\" \"%~dp0" + Exe + "\" " + option + " %*\r\n";
  }

  // Returns the path of the desktop shortcut, or null when it could not be made (the game works without it).
  public static string Write(string folder) {
    File.WriteAllText(Path.Combine(folder, ReadMeName), ReadMe);
    foreach (string old in OldFiles) {
      if (File.Exists(Path.Combine(folder, old))) File.Delete(Path.Combine(folder, old));
    }
    File.WriteAllText(Path.Combine(folder, Direct3DBat),
                      Bat("the emulated Xbox 360 GPU on Direct3D 12 (the Xbox 360's exact picture).",
                          "--fh1_renderer=xenos"));
    File.WriteAllText(Path.Combine(folder, VulkanBat),
                      Bat("the emulated Xbox 360 GPU on Vulkan.", "--fh1_renderer=xenos --gpu_backend=vulkan"));
    try {
      string shortcut = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), ShortcutName);
      // Windows' own scripting object writes .lnk files.
      Type shellType = Type.GetTypeFromProgID("WScript.Shell");
      object shell = Activator.CreateInstance(shellType);
      object link = shellType.InvokeMember("CreateShortcut", BindingFlags.InvokeMethod, null, shell, new object[] { shortcut });
      Type linkType = link.GetType();
      linkType.InvokeMember("TargetPath", BindingFlags.SetProperty, null, link, new object[] { Path.Combine(folder, Exe) });
      linkType.InvokeMember("WorkingDirectory", BindingFlags.SetProperty, null, link, new object[] { folder });
      linkType.InvokeMember("Description", BindingFlags.SetProperty, null, link,
                            new object[] { "Forza Horizon (Xbox 360) for Windows - unofficial port" });
      linkType.InvokeMember("Save", BindingFlags.InvokeMethod, null, link, null);
      return shortcut;
    } catch (Exception) {
      return null;
    }
  }
}

}  // namespace Fh1Installer
