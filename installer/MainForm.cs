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

  // Pixels at 100 % scaling -> pixels on this screen (the fonts, in points, scale by themselves).
  static float scale_ = 1f;
  static int S(int pixels) { return (int)Math.Round(pixels * scale_); }

  public MainForm() {
    using (Graphics g = CreateGraphics()) { scale_ = g.DpiX / 96f; }
    Text = "Forza Horizon recomp - installer (" + Program.Version + ")";
    Font = new Font("Segoe UI", 9.75f);
    AutoScaleMode = AutoScaleMode.None;
    StartPosition = FormStartPosition.CenterScreen;
    FormBorderStyle = FormBorderStyle.FixedSingle;
    MaximizeBox = false;
    ClientSize = new Size(S(760), S(500));

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

    AddRow(table, Heading("1. Your disc image (.iso)"));
    isoButton_.Text = "Choose ISO...";
    isoButton_.Click += delegate { ChooseIso(); };
    AddRow(table, ReadOnly(isoBox_), Sized(isoButton_));
    AddRow(table, Status(isoStatus_, "Choose the .iso of your own Forza Horizon disc."));

    AddRow(table, Heading("2. Where to install"));
    folderButton_.Text = "Choose folder...";
    folderButton_.Click += delegate { ChooseFolder(); };
    AddRow(table, ReadOnly(folderBox_), Sized(folderButton_));
    AddRow(table, Status(folderStatus_, "Choose an empty folder (about 9 GB are needed)."));

    AddRow(table, Heading("3. Install"));
    installButton_.Text = "Install";
    installButton_.Enabled = false;
    installButton_.Click += delegate { if (worker_ != null) Cancel(); else Install(); };
    progress_.Maximum = 1000;
    FormClosing += OnClosing;
    progress_.Dock = DockStyle.Fill;
    progress_.Margin = new Padding(S(3), S(6), S(12), S(6));
    AddRow(table, progress_, Sized(installButton_));
    AddRow(table, Status(stage_, "Downloads the port (about 50 MB), copies the game's files from your disc image " +
                                 "and prepares the shaders (about 10 minutes)."));

    Label credits = Note("Installer modelled on StevensND's installer for nfsmw-nx. Port built with ReXGlue; " +
                         "native renderer started from GoatHonks' nfsc-recomp. Not affiliated with Microsoft, " +
                         "Turn 10 or Playground Games.");
    credits.ForeColor = SystemColors.GrayText;
    credits.Dock = DockStyle.Bottom;
    table.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
    table.Controls.Add(credits);
    table.SetColumnSpan(credits, 2);
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
    label.ForeColor = result.Ok ? Good : Bad;
    installButton_.Enabled = isoOk_ && folderOk_;
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
  }

  void Install() {
    string iso = isoBox_.Text;
    string folder = folderBox_.Text;
    cancel_ = false;
    isoButton_.Enabled = false;
    folderButton_.Enabled = false;
    installButton_.Text = "Cancel";
    stage_.ForeColor = SystemColors.ControlText;
    stage_.Text = "Copying the game's files from your disc image...";
    worker_ = new Thread(delegate() { Work(iso, folder); });
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

  // On the worker thread.
  void Work(string iso, string folder) {
    string error = null;
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
          BeginInvoke((MethodInvoker)delegate {
            progress_.Value = (int)Math.Min(1000, done * 1000 / total);
            if (!cancel_) {
              stage_.Text = "Copying the game's files: " + Checks.Size(done) + " of " + Checks.Size(total) +
                            (name.Length > 0 ? "  (" + name + ")" : "");
            }
          });
        }, delegate { return cancel_; });
      }
    } catch (Exception e) {
      error = e.Message;
    }
    BeginInvoke((MethodInvoker)delegate { Stopped(finished, error); });
  }

  void Stopped(bool finished, string error) {
    worker_ = null;
    if (closeWhenStopped_) {
      Close();
      return;
    }
    isoButton_.Enabled = true;
    folderButton_.Enabled = true;
    installButton_.Text = "Install";
    installButton_.Enabled = true;
    if (error != null) {
      stage_.ForeColor = Bad;
      stage_.Text = "Stopped: " + error;
    } else if (!finished) {
      stage_.ForeColor = SystemColors.GrayText;
      stage_.Text = "Cancelled. What was copied stays in the folder: Install goes on from there.";
    } else {
      stage_.ForeColor = Good;
      stage_.Text = "The game's files are copied (this test version stops here: no download and no shaders yet).";
    }
    CheckFolder();
  }
}

}  // namespace Fh1Installer
