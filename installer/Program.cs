// FH1Installer - the Windows installer of the Forza Horizon port: it downloads the pre-built port, copies the
// game's files out of the user's own disc image and builds the shader library on the user's PC.
// Modelled on StevensND's installer page for nfsmw-nx. Built by installer\build_installer.ps1 with the C# compiler
// that is part of Windows (.NET Framework 4.8).
//
//   FH1Installer.exe                          the window
//   FH1Installer.exe --check ISO FOLDER       prints both checks and exits (for tests; no window)
using System;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace Fh1Installer {

public static class Program {
  public const string Version = "0.1.0-pre1";

  [DllImport("user32.dll")]
  static extern bool SetProcessDPIAware();

  [STAThread]
  public static int Main(string[] args) {
    if (args.Length == 3 && args[0] == "--check") {
      CheckResult disc = Checks.Disc(args[1]);
      Console.WriteLine("disc: " + (disc.Ok ? "ok" : "REFUSED") + " - " + disc.Text);
      CheckResult folder = Checks.Folder(args[2], disc.Bytes);
      Console.WriteLine("folder: " + (folder.Ok ? "ok" : "REFUSED") + " - " + folder.Text);
      return disc.Ok && folder.Ok ? 0 : 1;
    }
    SetProcessDPIAware();
    Application.EnableVisualStyles();
    Application.SetCompatibleTextRenderingDefault(false);
    Application.Run(new MainForm());
    return 0;
  }
}

}  // namespace Fh1Installer
