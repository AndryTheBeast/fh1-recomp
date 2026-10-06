// FH1Installer - the window. Modelled on StevensND's installer page for nfsmw-nx (choose your disc, choose where,
// one button, a progress bar): the same idea as a Windows program.
using System;
using System.Drawing;
using System.IO;
using System.Threading;
using System.Windows.Forms;

namespace Fh1Installer {

public sealed class MainForm : Form {
  readonly TextBox isoBox_ = new TextBox();
  readonly TextBox folderBox_ = new TextBox();
  readonly Label isoStatus_ = new Label();
  readonly Label folderStatus_ = new Label();
  readonly Button installButton_ = new Button();
  readonly Button isoButton_ = new Button();
  readonly Button folderButton_ = new Button();
  readonly Button runtimeButton_ = new Button();
  readonly Label needsStatus_ = new Label();
  readonly CheckBox fps60Box_ = new CheckBox();
  Thread worker_;
  volatile bool cancel_;
  bool closeWhenStopped_;
  readonly ProgressBar progress_ = new ProgressBar();
  readonly Label stage_ = new Label();
  bool isoOk_;
  bool folderOk_;
  long gameBytes_;

  static readonly Color Good = Color.FromArgb(0, 120, 40);
  static readonly Color Bad = Color.FromArgb(180, 30, 30);
  static readonly Color Warn = Color.FromArgb(170, 95, 0);

  // Pixels at 100 % scaling -> pixels on this screen (the fonts, in points, scale by themselves).
  static float scale_ = 1f;
  static int S(int pixels) { return (int)Math.Round(pixels * scale_); }

  public MainForm() {
    using (Graphics g = CreateGraphics()) { scale_ = g.DpiX / 96f; }
    Text = "Forza Horizon recomp - installer (" + Program.Version + ")";
    try {
      // The program's own icon in the title bar and on the taskbar.
      Icon = Icon.ExtractAssociatedIcon(System.Reflection.Assembly.GetExecutingAssembly().Location);
    } catch (Exception) {
    }
    Font = new Font("Segoe UI", 9.75f);
    AutoScaleMode = AutoScaleMode.None;
    StartPosition = FormStartPosition.CenterScreen;
    FormBorderStyle = FormBorderStyle.FixedSingle;
    MaximizeBox = false;
    ClientSize = new Size(S(760), S(730));

    TableLayoutPanel table = new TableLayoutPanel();
    table.Dock = DockStyle.Fill;
    table.Padding = new Padding(S(16));
    table.ColumnCount = 2;
    table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
    table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
    Controls.Add(table);

    Label title = new Label();
    title.Text = "Forza Horizon (Xbox 360) for Windows";
    title.Font = new Font("Segoe UI Semibold", 16f);
    title.AutoSize = true;
    AddRow(table, title);

    AddRow(table, Note("An unofficial port. It needs your own disc image of the game: nothing of the game " +
                       "comes with this program, and nothing leaves your computer."));

    AddRow(table, Heading("Needed on this PC"));
    runtimeButton_.Text = "Install runtime";
    runtimeButton_.Click += delegate { InstallRuntime(); };
    Status(needsStatus_, "");
    needsStatus_.MaximumSize = new Size(S(560), 0);
    AddRow(table, needsStatus_, Sized(runtimeButton_));
    ShowNeeds();

    AddRow(table, Heading("1. Your disc image (.iso)"));
    isoButton_.Text = "Choose ISO...";
    isoButton_.Click += delegate { ChooseIso(); };
    AddRow(table, ReadOnly(isoBox_), Sized(isoButton_));
    Label usaOnly = Note("Only the USA version of the game (NTSC-U) works right now.");
    usaOnly.ForeColor = Warn;
    AddRow(table, usaOnly);
    AddRow(table, Status(isoStatus_, "Choose the .iso of your own Forza Horizon disc."));

    AddRow(table, Heading("2. Where to install"));
    folderButton_.Text = "Choose folder...";
    folderButton_.Click += delegate { ChooseFolder(); };
    AddRow(table, ReadOnly(folderBox_), Sized(folderButton_));
    AddRow(table, Status(folderStatus_, "Choose an empty folder (about 9 GB are needed)."));

    AddRow(table, Heading("3. Options"));
    fps60Box_.Text = "60 frames per second (experimental)";
    fps60Box_.AutoSize = true;
    fps60Box_.Margin = new Padding(S(6), S(4), S(3), S(0));
    AddRow(table, fps60Box_);
    AddRow(table, Note("The game runs at 30 frames per second, its limit on the Xbox 360. With this it draws and " +
                       "moves 60 times a second where your PC is fast enough (a strong graphics chip is needed to " +
                       "hold 60 while driving). New and little tested: if something moves too fast or looks " +
                       "wrong, start this installer again, untick it and click Update."));

    AddRow(table, Heading("4. Install"));
    installButton_.Text = "Install";
    installButton_.Enabled = false;
    installButton_.Click += delegate { if (worker_ != null) Cancel(); else Install(); };
    progress_.Maximum = 1000;
    FormClosing += OnClosing;
    progress_.Dock = DockStyle.Fill;
    progress_.Margin = new Padding(S(3), S(6), S(12), S(6));
    AddRow(table, progress_, Sized(installButton_));
    AddRow(table, Status(stage_, "Copies the game's files from your disc image, downloads the port (about 50 MB) " +
                                 "and prepares the shaders (3 to 10 minutes, the PC is busy meanwhile)."));

    Label credits = Note("Installer modelled on StevensND's installer for nfsmw-nx. Port built with ReXGlue; " +
                         "native renderer started from GoatHonks' nfsc-recomp. Not affiliated with Microsoft, " +
                         "Turn 10 or Playground Games.");
    credits.ForeColor = SystemColors.GrayText;
    credits.Dock = DockStyle.Bottom;
    table.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
    table.Controls.Add(credits);
    table.SetColumnSpan(credits, 2);
  }

  // What the game needs from Windows; the button shows only while the Visual C++ runtime is missing.
  void ShowNeeds() {
    bool runtime = Needs.HasRuntime();
    needsStatus_.Text = Needs.Describe();
    needsStatus_.ForeColor = runtime && Needs.HasDirect3D12() && Needs.HasVulkan() ? Good : Bad;
    runtimeButton_.Visible = !runtime;
    installButton_.Enabled = worker_ == null && isoOk_ && folderOk_ && runtime;
  }

  // Microsoft's own installer of the runtime, downloaded from Microsoft and started here.
  void InstallRuntime() {
    runtimeButton_.Enabled = false;
    ThreadPool.QueueUserWorkItem(delegate {
      string error = Needs.InstallRuntime(delegate(string what) {
        BeginInvoke((MethodInvoker)delegate {
          needsStatus_.ForeColor = SystemColors.ControlText;
          needsStatus_.Text = what;
        });
      });
      BeginInvoke((MethodInvoker)delegate {
        runtimeButton_.Enabled = true;
        ShowNeeds();
        if (error != null) needsStatus_.Text = "The runtime was not installed: " + error + " " + needsStatus_.Text;
      });
    });
  }

  static void AddRow(TableLayoutPanel table, Control one) {
    table.RowStyles.Add(new RowStyle(SizeType.AutoSize));
    table.Controls.Add(one);
    table.SetColumnSpan(one, 2);
  }

  static void AddRow(TableLayoutPanel table, Control left, Control right) {
    table.RowStyles.Add(new RowStyle(SizeType.AutoSize));
    table.Controls.Add(left);
    table.Controls.Add(right);
  }

  static Label Heading(string text) {
    Label l = new Label();
    l.Text = text;
    l.Font = new Font("Segoe UI Semibold", 11f);
    l.AutoSize = true;
    l.Margin = new Padding(S(3), S(14), S(3), S(2));
    return l;
  }

  static Label Note(string text) {
    Label l = new Label();
    l.Text = text;
    l.AutoSize = true;
    l.MaximumSize = new Size(S(720), S(0));
    l.Margin = new Padding(S(3), S(2), S(3), S(2));
    return l;
  }

  static Label Status(Label l, string text) {
    l.Text = text;
    l.AutoSize = true;
    l.MaximumSize = new Size(S(720), S(0));
    l.ForeColor = SystemColors.GrayText;
    l.Margin = new Padding(S(3), S(2), S(3), S(2));
    return l;
  }

  static TextBox ReadOnly(TextBox box) {
    box.ReadOnly = true;
    box.Dock = DockStyle.Fill;
    box.Margin = new Padding(S(3), S(5), S(12), S(3));
    return box;
  }

  static Button Sized(Button b) {
    b.AutoSize = true;
    b.MinimumSize = new Size(S(140), S(30));
    return b;
  }

  void Show(Label label, CheckResult result) {
    label.Text = result.Text;
    label.ForeColor = !result.Ok ? Bad : result.Warning ? Warn : Good;
    installButton_.Enabled = isoOk_ && folderOk_ && Needs.HasRuntime();
  }

  void ChooseIso() {
    using (OpenFileDialog dialog = new OpenFileDialog()) {
      dialog.Title = "Choose your Forza Horizon disc image";
      dialog.Filter = "Disc images (*.iso)|*.iso|All files (*.*)|*.*";
      if (dialog.ShowDialog(this) != DialogResult.OK) return;
      isoBox_.Text = dialog.FileName;
    }
    isoOk_ = false;
    isoStatus_.Text = "Reading the disc image...";
    isoStatus_.ForeColor = SystemColors.GrayText;
    installButton_.Enabled = false;
    string path = isoBox_.Text;
    // Off the window's thread: reading the file table of an 8 GB image on a slow drive takes a moment.
    ThreadPool.QueueUserWorkItem(delegate {
      CheckResult result = Checks.Disc(path);
      BeginInvoke((MethodInvoker)delegate {
        if (isoBox_.Text != path) return;  // another file was chosen meanwhile
        isoOk_ = result.Ok;
        gameBytes_ = result.Bytes;
        Show(isoStatus_, result);
        if (folderBox_.Text.Length > 0) CheckFolder();
      });
    });
  }

  void ChooseFolder() {
    using (FolderBrowserDialog dialog = new FolderBrowserDialog()) {
      dialog.Description = "Choose an empty folder for the game (or make a new one)";
      dialog.ShowNewFolderButton = true;
      if (dialog.ShowDialog(this) != DialogResult.OK) return;
      folderBox_.Text = dialog.SelectedPath;
    }
    CheckFolder();
  }

  void CheckFolder() {
    // Before a disc is chosen: the size of the USA disc's files.
    CheckResult result = Checks.Folder(folderBox_.Text, gameBytes_ > 0 ? gameBytes_ : 7320L * 1000 * 1000);
    folderOk_ = result.Ok;
    Show(folderStatus_, result);
    // A folder that holds an installation is brought up to date: the same steps, each skipped when done.
    bool again = false;
    try {
      again = File.Exists(Path.Combine(folderBox_.Text, Checks.MarkerName));
    } catch (ArgumentException) {
    }
    if (worker_ == null) installButton_.Text = again ? "Update" : "Install";
    // An installation shows the choice it was made with.
    if (again && worker_ == null) fps60Box_.Checked = Options.ReadFps60(folderBox_.Text);
  }

  void Install() {
    string iso = isoBox_.Text;
    string folder = folderBox_.Text;
    cancel_ = false;
    isoButton_.Enabled = false;
    folderButton_.Enabled = false;
    fps60Box_.Enabled = false;
    bool fps60 = fps60Box_.Checked;
    installButton_.Text = "Cancel";
    stage_.ForeColor = SystemColors.ControlText;
    stage_.Text = "Copying the game's files from your disc image...";
    worker_ = new Thread(delegate() { Work(iso, folder, fps60); });
    worker_.IsBackground = true;
    worker_.Start();
  }

  void Cancel() {
    cancel_ = true;
    installButton_.Enabled = false;
    stage_.Text = "Stopping...";
  }

  // Closing the window while it works: stop the work first, then close.
  void OnClosing(object sender, FormClosingEventArgs e) {
    if (worker_ == null) return;
    e.Cancel = true;
    closeWhenStopped_ = true;
    Cancel();
  }

  // From the worker thread: where the bar stands (0 to 1) and what is being done.
  void Report(double fraction, string what) {
    BeginInvoke((MethodInvoker)delegate {
      progress_.Value = (int)Math.Max(0, Math.Min(1000, fraction * 1000));
      if (!cancel_) stage_.Text = what;
    });
  }

  // On the worker thread. The bar: the disc copy is its first quarter, the shaders the rest.
  void Work(string iso, string folder, bool fps60) {
    string error = null;
    string summary = null;
    bool finished = false;
    try {
      Installation.WriteMarker(folder);
      using (XDisc disc = new XDisc(iso)) {
        long total = Math.Max(1, disc.TotalBytes);
        DateTime shown = DateTime.MinValue;
        finished = disc.Extract(Path.Combine(folder, Checks.GameFolder), delegate(long done, string name) {
          // Ten times a second is enough for the eye, and the window stays responsive.
          DateTime now = DateTime.UtcNow;
          if (done < total && (now - shown).TotalMilliseconds < 100) return;
          shown = now;
          Report(0.25 * done / total, "Copying the game's files: " + Checks.Size(done) + " of " + Checks.Size(total) +
                                          (name.Length > 0 ? "  (" + name + ")" : ""));
        }, delegate { return cancel_; });
      }
      if (finished) {
        Report(0.25, "Getting the port...");
        Source.Get(folder, delegate(double fraction, string what) { Report(0.25 + 0.03 * fraction, what); },
                   delegate { return cancel_; });
        string tools = Path.Combine(folder, "tools");
        if (Installation.LibraryIsCurrent(folder, tools)) {
          summary = "The shader library was already there.";
        } else {
          DateTime shown = DateTime.MinValue;
          LibraryResult library = Installation.BuildLibrary(folder, tools, delegate(string stage, int done, int total) {
            DateTime now = DateTime.UtcNow;
            if (done < total && (now - shown).TotalMilliseconds < 100) return;
            shown = now;
            bool shaders = stage == "Preparing shaders";
            Report(shaders ? 0.3 + 0.7 * done / Math.Max(1, total) : (total > 0 ? 0.28 + 0.02 * done / total : 0.28),
                   stage + (total > 0 ? ": " + done + " of " + total : "..."));
          }, delegate { return cancel_; });
          finished = library != null;
          if (finished) {
            Installation.WriteLibraryStamp(folder, tools);
            summary = library.Compiled + " shaders prepared.";
          }
        }
        if (finished) {
          summary += Launchers.Write(folder) != null
                         ? " Start the game with the \"Forza Horizon\" shortcut on your desktop, or " + Launchers.Exe +
                               " in the folder; the two .bat files there start the emulated picture."
                         : " Start the game with " + Launchers.Exe + " in the folder (the desktop shortcut could " +
                               "not be made); the two .bat files there start the emulated picture.";
          Options.WriteFps60(folder, fps60);
          if (fps60) summary += " 60 frames per second is on.";
        }
      }
    } catch (OperationCanceledException) {
      finished = false;
    } catch (Exception e) {
      error = e.Message;
    }
    BeginInvoke((MethodInvoker)delegate { Stopped(finished, error, summary); });
  }

  void Stopped(bool finished, string error, string summary) {
    worker_ = null;
    if (closeWhenStopped_) {
      Close();
      return;
    }
    isoButton_.Enabled = true;
    folderButton_.Enabled = true;
    fps60Box_.Enabled = true;
    installButton_.Text = "Install";
    installButton_.Enabled = true;
    if (error != null) {
      stage_.ForeColor = Bad;
      stage_.Text = "Stopped: " + error;
    } else if (!finished) {
      stage_.ForeColor = SystemColors.GrayText;
      stage_.Text = "Cancelled. What was done stays in the folder: Install goes on from there.";
    } else {
      progress_.Value = 1000;
      stage_.ForeColor = Good;
      stage_.Text = "Installed. " + summary;
    }
    CheckFolder();
  }
}

}  // namespace Fh1Installer
