// FH1Installer - the two checks of the first screen: is this .iso Forza Horizon, and can this folder take it.
using System;
using System.IO;

namespace Fh1Installer {

public sealed class CheckResult {
  public bool Ok;
  public bool Warning;  // accepted, but the text is a warning
  public string Text;
  public long Bytes;  // disc check: the size of the game's files

  public static CheckResult Good(string text) {
    CheckResult r = new CheckResult();
    r.Ok = true;
    r.Text = text;
    return r;
  }
  public static CheckResult Bad(string text) {
    CheckResult r = new CheckResult();
    r.Text = text;
    return r;
  }
}

public static class Checks {
  public const uint TitleId = 0x4D5309C9;        // Forza Horizon
  public const uint UsaMediaId = 0x2DC7007B;  // the USA disc (NTSC-U)
  public const string TestedVersionText = "0.0.0.10";  // the USA disc's default.xex, the one the port was made from
  // Next to the game's files: the port (about 135 MB), the shader library (about 170 MB) and, while it is being
  // built, the translated and compiled shaders (about 400 MB).
  public const long ExtraBytes = 750L * 1024 * 1024;
  // The file an installation leaves in its folder: a folder that has it may be installed into again.
  public const string MarkerName = "fh1_install.txt";
  // The game's files inside the installation folder.
  public const string GameFolder = "game";

  public static string Size(long bytes) {
    if (bytes >= 1024L * 1024 * 1024) return (bytes / (1024.0 * 1024 * 1024)).ToString("0.0") + " GB";
    return (bytes / (1024.0 * 1024)).ToString("0") + " MB";
  }

  public static CheckResult Disc(string isoPath) {
    try {
      if (!File.Exists(isoPath)) return CheckResult.Bad("This file does not exist.");
      using (XDisc disc = new XDisc(isoPath)) {
        DiscFile xex = disc.Find("default.xex");
        if (xex == null) return CheckResult.Bad("This disc image holds no game program (default.xex).");
        XexInfo info = XexInfo.Read(disc.ReadStart(xex, 64 * 1024));
        if (info == null) return CheckResult.Bad("The game program on this disc image could not be read.");
        if (info.TitleId != TitleId) {
          return CheckResult.Bad("This is another game (title ID " + info.TitleId.ToString("X8") +
                                 "). Forza Horizon is 4D5309C9.");
        }
        bool usa = info.MediaId == UsaMediaId && info.VersionText == TestedVersionText;
        string text = "Forza Horizon found: " + disc.Files.Count + " files, " + Size(disc.TotalBytes) +
                      " (title ID " + info.TitleId.ToString("X8") + ", version " + info.VersionText + ", disc " +
                      info.MediaId.ToString("X8") + (usa ? ": the USA version)." : ").");
        if (!usa) {
          text = "WARNING: this is not the USA version of the game. Only the USA version (NTSC-U) works right " +
                 "now: with this disc the game will probably not start. " + text;
        }
        CheckResult r = CheckResult.Good(text);
        r.Warning = !usa;
        r.Bytes = disc.TotalBytes;
        return r;
      }
    } catch (Exception e) {
      return CheckResult.Bad(e.Message);
    }
  }

  public static CheckResult Folder(string folder, long gameBytes) {
    try {
      if (string.IsNullOrWhiteSpace(folder)) return CheckResult.Bad("Choose a folder.");
      string full = Path.GetFullPath(folder);
      string root = Path.GetPathRoot(full);
      if (string.Equals(full.TrimEnd('\\'), root.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase)) {
        return CheckResult.Bad("Choose a folder, not a whole drive (for example " + root + "Games\\ForzaHorizon).");
      }
      bool again = false;
      if (Directory.Exists(full)) {
        again = File.Exists(Path.Combine(full, MarkerName));
        bool empty = Directory.GetFileSystemEntries(full).Length == 0;
        if (!again && !empty) {
          return CheckResult.Bad("This folder already has other files in it. Choose an empty folder or make a new one.");
        }
      }
      long need = gameBytes + ExtraBytes;
      DriveInfo drive = new DriveInfo(root);
      if (!drive.IsReady) return CheckResult.Bad("Drive " + root + " is not ready.");
      long free = drive.AvailableFreeSpace;
      string space = "needs about " + Size(need) + ", drive " + root + " has " + Size(free) + " free";
      if (!again && free < need) return CheckResult.Bad("Not enough space: " + space + ".");
      return CheckResult.Good((again ? "A previous installation is here: it will be updated; " : "Folder is fine: ") +
                              space + ".");
    } catch (Exception e) {
      return CheckResult.Bad(e.Message);
    }
  }
}

}  // namespace Fh1Installer
