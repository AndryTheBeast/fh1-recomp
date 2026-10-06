// FH1Installer - the Windows installer of the Forza Horizon port: it downloads the pre-built port, copies the
// game's files out of the user's own disc image and builds the shader library on the user's PC.
// Modelled on StevensND's installer page for nfsmw-nx. Built by installer\build_installer.ps1 with the C# compiler
// that is part of Windows (.NET Framework 4.8).
//
//   FH1Installer.exe                          the window
//   FH1Installer.exe --check ISO FOLDER       prints both checks and exits (for tests; no window)
//   FH1Installer.exe --extract ISO FOLDER     both checks, then copies the disc's files (for tests; no window)
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace Fh1Installer {

public static class Program {
  public const string Version = "0.1.0-pre1";

  [DllImport("user32.dll")]
  static extern bool SetProcessDPIAware();

  [STAThread]
  public static int Main(string[] args) {
    if (args.Length == 3 && (args[0] == "--check" || args[0] == "--extract")) {
      CheckResult disc = Checks.Disc(args[1]);
      Console.WriteLine("disc: " + (disc.Ok ? "ok" : "REFUSED") + " - " + disc.Text);
      CheckResult folder = Checks.Folder(args[2], disc.Bytes);
      Console.WriteLine("folder: " + (folder.Ok ? "ok" : "REFUSED") + " - " + folder.Text);
      if (!disc.Ok || !folder.Ok) return 1;
      if (args[0] == "--check") return 0;
      Installation.WriteMarker(args[2]);
      DateTime start = DateTime.UtcNow;
      using (XDisc image = new XDisc(args[1])) {
        image.Extract(Path.Combine(args[2], Checks.GameFolder), delegate { }, delegate { return false; });
        Console.WriteLine("extracted: " + image.Files.Count + " files, " + image.TotalBytes + " bytes in " +
                          (DateTime.UtcNow - start).TotalSeconds.ToString("0") + " s");
      }
      return 0;
    }
    SetProcessDPIAware();
    Application.EnableVisualStyles();
    Application.SetCompatibleTextRenderingDefault(false);
    Application.Run(new MainForm());
    return 0;
  }
}

// What an installation leaves in its folder.
public static class Installation {
  public static void WriteMarker(string folder) {
    Directory.CreateDirectory(folder);
    File.WriteAllText(Path.Combine(folder, Checks.MarkerName),
                      "Forza Horizon recomp, installer " + Program.Version + "\r\n" +
                      "This file tells the installer that this folder is one of its installations.\r\n");
  }
}

}  // namespace Fh1Installer
