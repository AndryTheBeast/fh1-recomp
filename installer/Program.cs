// FH1Installer - the Windows installer of the Forza Horizon port: it downloads the pre-built port, copies the
// game's files out of the user's own disc image and builds the shader library on the user's PC.
// Modelled on StevensND's installer page for nfsmw-nx. Built by installer\build_installer.ps1 with the C# compiler
// that is part of Windows (.NET Framework 4.8).
//
//   FH1Installer.exe                          the window
//   FH1Installer.exe --check ISO FOLDER       prints both checks and exits (for tests; no window)
//   FH1Installer.exe --extract ISO FOLDER     both checks, then copies the disc's files (for tests; no window)
//   FH1Installer.exe --install ISO FOLDER     the whole installation without the window (for tests)
//   --source ADDRESS (before the rest)        where the two zips are, instead of the release (for tests)
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace Fh1Installer {

public static class Program {
  public const string Version = "0.1.0-pre1";

  [DllImport("user32.dll")]
  static extern bool SetProcessDPIAware();

  [DllImport("kernel32.dll")]
  static extern uint SetErrorMode(uint mode);

  [STAThread]
  public static int Main(string[] args) {
    // A shader tool that stops on a container it cannot read must not open a Windows error box (the tools
    // started from here inherit this): SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX.
    SetErrorMode(0x0001 | 0x0002);
    if (args.Length >= 2 && args[0] == "--source") {
      Source.Base = args[1];
      string[] rest = new string[args.Length - 2];
      Array.Copy(args, 2, rest, 0, rest.Length);
      args = rest;
    }
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
    // The whole installation as the window does it, without the window (for tests).
    if (args.Length == 3 && args[0] == "--install") {
      DateTime start = DateTime.UtcNow;
      CheckResult disc = Checks.Disc(args[1]);
      CheckResult folder = Checks.Folder(args[2], disc.Bytes);
      Console.WriteLine("disc: " + disc.Text);
      Console.WriteLine("folder: " + folder.Text);
      if (!disc.Ok || !folder.Ok) return 1;
      Installation.WriteMarker(args[2]);
      using (XDisc image = new XDisc(args[1])) {
        image.Extract(Path.Combine(args[2], Checks.GameFolder), delegate { }, delegate { return false; });
      }
      Console.WriteLine((DateTime.UtcNow - start).TotalSeconds.ToString("0") + " s: the game's files are copied");
      Console.WriteLine("needs: " + Needs.Describe());
      Console.WriteLine("the port comes from: " + (Source.LocalPackage() ?? Source.Base));
      Source.Get(args[2], delegate { }, delegate { return false; });
      string tools = Path.Combine(args[2], "tools");
      if (Installation.LibraryIsCurrent(args[2], tools)) {
        Console.WriteLine("the shader library was already there");
      } else {
        LibraryResult r = Installation.BuildLibrary(args[2], tools, delegate { }, delegate { return false; });
        Installation.WriteLibraryStamp(args[2], tools);
        Console.WriteLine("library: " + r.Compiled + " of " + r.Containers + " shaders, " + r.Bytes + " bytes");
      }
      Console.WriteLine("desktop shortcut: " + (Launchers.Write(args[2]) ?? "NOT made"));
      Console.WriteLine("installed in " + (DateTime.UtcNow - start).TotalSeconds.ToString("0") + " s");
      return 0;
    }
    if (args.Length == 3 && args[0] == "--library") {
      DateTime start = DateTime.UtcNow;
      string last = "";
      LibraryResult r = Installation.BuildLibrary(args[1], args[2], delegate(string stage, int done, int total) {
        if (stage == last) return;
        last = stage;
        Console.WriteLine((DateTime.UtcNow - start).TotalSeconds.ToString("0") + " s: " + stage);
      }, delegate { return false; });
      Console.WriteLine("library: " + r.Compiled + " of " + r.Containers + " shaders, " + r.Bytes + " bytes, " +
                        r.ArchiveFiles + " shader files from archives (" + r.ArchiveFailed + " failed), " +
                        (DateTime.UtcNow - start).TotalSeconds.ToString("0") + " s");
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

  // The library is built again only when the shader tools changed (an update of the port): a small file next to
  // it names the tools it was made with.
  const string StampName = "fh1_shaders.txt";

  static string ToolsStamp(string toolsFolder) {
    System.Text.StringBuilder s = new System.Text.StringBuilder();
    foreach (string tool in ShaderLibrary.Tools) {
      FileInfo info = new FileInfo(Path.Combine(toolsFolder, tool));
      s.Append(tool).Append(' ').Append(info.Exists ? info.Length : -1).Append(' ')
          .Append(info.Exists ? info.LastWriteTimeUtc.Ticks : 0).Append("\r\n");
    }
    return s.ToString();
  }

  public static bool LibraryIsCurrent(string folder, string toolsFolder) {
    string stamp = Path.Combine(folder, StampName);
    return File.Exists(Path.Combine(folder, ShaderLibrary.LibraryName)) && File.Exists(stamp) &&
           File.ReadAllText(stamp) == ToolsStamp(toolsFolder);
  }

  public static void WriteLibraryStamp(string folder, string toolsFolder) {
    File.WriteAllText(Path.Combine(folder, StampName), ToolsStamp(toolsFolder));
  }

  // The shader library of an installation folder (fh1.exe and the folder "game" are in it already). fh1.exe
  // first writes default.xex's loaded image (about 480 shaders are only there); it is deleted afterwards.
  public static LibraryResult BuildLibrary(string folder, string toolsFolder, Action<string, int, int> progress,
                                           Func<bool> cancelled) {
    string work = Path.Combine(folder, "shader_work");
    string imageFolder = Path.Combine(folder, "shader_image");
    try {
      progress("Reading the game program", 0, 0);
      if (Directory.Exists(imageFolder)) Directory.Delete(imageFolder, true);
      Directory.CreateDirectory(imageFolder);
      string image = Path.Combine(imageFolder, "image.bin");
      string game = Path.Combine(folder, Checks.GameFolder);
      // Its own empty user folder: this run never looks at the saves.
      int code = ShaderLibrary.Run(Path.Combine(folder, Launchers.Exe),
                                   "\"--game_data_root=" + game + "\" \"--user_data_root=" +
                                       Path.Combine(imageFolder, "user") + "\" \"--fh1_unpack_image=" + image + "\"",
                                   180);
      if (code != 0 || !File.Exists(image)) {
        throw new InvalidOperationException(
            Needs.HasRuntime() ? "FH1.exe could not read the game program (code " + code + ")."
                               : "FH1.exe could not start: the Microsoft Visual C++ runtime is missing.");
      }
      return ShaderLibrary.Build(game, toolsFolder, image, work, Path.Combine(folder, ShaderLibrary.LibraryName),
                                 progress, cancelled);
    } finally {
      try {
        if (Directory.Exists(imageFolder)) Directory.Delete(imageFolder, true);
        if (Directory.Exists(work)) Directory.Delete(work, true);
      } catch (IOException) {
      }
    }
  }
}

}  // namespace Fh1Installer
