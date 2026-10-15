// FathersFallout - online co-op for Fallout 4
// Copyright (C) 2026 FatherLamb. Licensed under the GNU GPL v3 with additional terms: see LICENSE
// and NOTICE ("FathersFallout" and "FatherLamb" are reserved names; modified versions must be renamed).

// FathersFallout launcher: checks the setup, installs/updates the mod, picks Host/Join and a save,
// then starts the game through F4SE. The mod reads F4MP.ini [Launch] LoadSave= and loads that save at
// the main menu, then connects (AutoConnect). Looks like a Pip-Boy terminal (green CRT, scanlines)
// with the FathersFallout logo. Built with the .NET Framework 4 compiler that ships with Windows
// (build.bat), so friends don't need to install anything to run it.
// C# 5 only (that compiler): no $"" strings, no ?. operators.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: AssemblyTitle("FathersFallout")]
[assembly: AssemblyProduct("FathersFallout")]
[assembly: AssemblyDescription("FathersFallout launcher - online co-op for Fallout 4")]
[assembly: AssemblyCompany("FatherLamb")]
[assembly: AssemblyCopyright("Copyright (C) 2026 FatherLamb")]
[assembly: AssemblyTrademark("FathersFallout and FatherLamb are names reserved by FatherLamb")]
[assembly: AssemblyVersion("0.19.0.0")]
[assembly: AssemblyFileVersion("0.19.0.0")]

namespace F4MPLauncher
{
    static class Ini
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        static extern int GetPrivateProfileString(string section, string key, string def, StringBuilder ret, int size, string file);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        static extern bool WritePrivateProfileString(string section, string key, string value, string file);

        public static string Get(string file, string section, string key, string def)
        {
            var sb = new StringBuilder(1024);
            GetPrivateProfileString(section, key, def, sb, sb.Capacity, file);
            return sb.ToString();
        }
        public static void Set(string file, string section, string key, string value)
        {
            WritePrivateProfileString(section, key, value, file);
        }
    }

    class SaveInfo
    {
        public string File;      // full path of the .fos
        public string Name;      // save name without extension (what the console's 'load' wants)
        public string Player = "?";
        public int Level;
        public string Location = "";
        public DateTime When;
        public override string ToString()
        {
            return string.Format("{0}  LV {1}  {2}  {3:g}", Player.ToUpper(), Level, Location.ToUpper(), When);
        }
    }

    class Check
    {
        public string Label, Detail, Url;
        public bool Ok;
    }

    // ---------- Pip-Boy look ----------

    static class Pip
    {
        public static readonly Color Green = Color.FromArgb(26, 255, 128);
        public static readonly Color Mid = Color.FromArgb(18, 190, 96);
        public static readonly Color Dim = Color.FromArgb(14, 110, 60);
        public static readonly Color Faint = Color.FromArgb(10, 60, 34);
        public static readonly Color Amber = Color.FromArgb(255, 182, 65);
        public static readonly Color Dark = Color.FromArgb(6, 14, 9);
        public static readonly Color Field = Color.FromArgb(9, 24, 15);

        static string family;
        // Monofonto is the font the Fallout games use; most players won't have it, so Consolas.
        public static Font Font(float size, FontStyle style)
        {
            if (family == null) {
                family = "Consolas";
                using (var fonts = new InstalledFontCollection())
                    foreach (var f in fonts.Families)
                        if (f.Name == "Monofonto") family = "Monofonto";
            }
            return new Font(family, size, style);
        }

        // Text with a soft phosphor glow (a few faint offset copies under it).
        public static void GlowText(Graphics g, string text, Font font, Color color, PointF at)
        {
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            using (var glow = new SolidBrush(Color.FromArgb(38, color)))
                for (int dx = -2; dx <= 2; dx++)
                    for (int dy = -2; dy <= 2; dy++)
                        if (Math.Abs(dx) + Math.Abs(dy) == 2) g.DrawString(text, font, glow, at.X + dx, at.Y + dy);
            using (var b = new SolidBrush(color)) g.DrawString(text, font, b, at);
        }
    }

    // A terminal "button": [ TEXT ] that lights up (green fill, dark text) under the mouse.
    class TermButton : Control
    {
        bool hover;
        public bool Selected;      // for option toggles: shown filled like a hover
        public bool Big;
        public Color Accent = Pip.Green;

        public TermButton(string text)
        {
            Text = text;
            SetStyle(ControlStyles.SupportsTransparentBackColor | ControlStyles.OptimizedDoubleBuffer |
                     ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
            BackColor = Color.Transparent;
            Cursor = Cursors.Hand;
            Font = Pip.Font(10.5f, FontStyle.Bold);
        }

        protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }
        protected override void OnTextChanged(EventArgs e) { Invalidate(); base.OnTextChanged(e); }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.None;
            var r = new Rectangle(0, 0, Width - 1, Height - 1);
            bool lit = hover || Selected;
            if (lit) using (var b = new SolidBrush(Accent)) g.FillRectangle(b, r);
            else using (var b = new SolidBrush(Color.FromArgb(150, Pip.Dark))) g.FillRectangle(b, r);
            using (var p = new Pen(lit ? Accent : Color.FromArgb(Big ? 255 : 200, Accent), Big ? 2f : 1f)) g.DrawRectangle(p, Big ? new Rectangle(1, 1, Width - 3, Height - 3) : r);
            var sf = new StringFormat { Alignment = StringAlignment.Center, LineAlignment = StringAlignment.Center };
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            using (var b = new SolidBrush(lit ? Pip.Dark : Accent)) g.DrawString(Text, Font, b, new RectangleF(0, 1, Width, Height), sf);
        }
    }

    // A text box framed like a terminal field.
    class TermField : Panel
    {
        public readonly TextBox Box = new TextBox();
        public TermField()
        {
            SetStyle(ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Pip.Field;
            Padding = new Padding(8, 5, 8, 3);
            Box.BorderStyle = BorderStyle.None;
            Box.BackColor = Pip.Field;
            Box.ForeColor = Pip.Green;
            Box.Font = Pip.Font(11f, FontStyle.Regular);
            Box.Dock = DockStyle.Fill;
            Controls.Add(Box);
            Height = 30;
        }
        protected override void OnPaint(PaintEventArgs e)
        {
            using (var p = new Pen(Pip.Mid)) e.Graphics.DrawRectangle(p, 0, 0, Width - 1, Height - 1);
        }
    }

    // Dark menus for the save list.
    class PipColors : ProfessionalColorTable
    {
        public override Color MenuItemSelected { get { return Pip.Green; } }
        public override Color MenuItemBorder { get { return Pip.Green; } }
        public override Color MenuBorder { get { return Pip.Mid; } }
        public override Color ToolStripDropDownBackground { get { return Pip.Dark; } }
        public override Color ImageMarginGradientBegin { get { return Pip.Dark; } }
        public override Color ImageMarginGradientMiddle { get { return Pip.Dark; } }
        public override Color ImageMarginGradientEnd { get { return Pip.Dark; } }
        public override Color MenuItemSelectedGradientBegin { get { return Pip.Green; } }
        public override Color MenuItemSelectedGradientEnd { get { return Pip.Green; } }
    }
    class PipRenderer : ToolStripProfessionalRenderer
    {
        public PipRenderer() : base(new PipColors()) { }
        protected override void OnRenderItemText(ToolStripItemTextRenderEventArgs e)
        {
            e.TextColor = e.Item.Selected ? Pip.Dark : Pip.Green;
            base.OnRenderItemText(e);
        }
    }

    class MainForm : Form
    {
        const int W = 1000, H = 640, LeftW = 340, TitleH = 34;

        [DllImport("user32.dll")] static extern bool ReleaseCapture();
        [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, int msg, int w, int l);

        readonly string baseDir = AppDomain.CurrentDomain.BaseDirectory;
        readonly string settings;
        string gameDir = "";
        Image logo;
        Bitmap backdrop;
        List<SaveInfo> saves = new List<SaveInfo>();
        SaveInfo save;

        readonly List<Control> checkRows = new List<Control>();
        readonly TermButton joinOpt = new TermButton("[ ] JOIN A FRIEND"), hostOpt = new TermButton("[ ] HOST");
        readonly TermButton steamOpt = new TermButton("[ ] THROUGH STEAM"), ipOpt = new TermButton("[ ] BY IP (GOG)");
        readonly TermField ip = new TermField(), port = new TermField(), name = new TermField(), steamWho = new TermField();
        readonly Label steamLabel = new Label();
        readonly TermButton saveButton = new TermButton(""), play = new TermButton("[  PLAY  ]");
        readonly TermButton install = new TermButton("[ INSTALL / UPDATE FATHERSFALLOUT ]"), ipButton = new TermButton("[ MY IP FOR FRIENDS ]");
        readonly TermButton close = new TermButton("X"), minimize = new TermButton("_"), gameButton = new TermButton("[ GAME FOLDER ]");
        readonly TermButton shareLog = new TermButton("[ SHARE LOG ]");
        const string IssuesUrl = "https://github.com/FLHNM/FathersFallout/issues/new?template=bug_report.yml";
        readonly Label ipLabel = new Label(), portLabel = new Label(), hostInfo = new Label(), status = new Label();
        readonly ContextMenuStrip saveMenu = new ContextMenuStrip();
        readonly Timer blink = new Timer();
        string statusText = "", typing = "";
        bool cursorOn;

        public MainForm()
        {
            settings = Path.Combine(baseDir, "FathersFallout.ini");
            // Settings from before the rename carry over.
            string old = Path.Combine(baseDir, "Fallout 4 Co-op.ini");
            if (!File.Exists(settings) && File.Exists(old)) { try { File.Copy(old, settings); } catch { } }

            Text = "FathersFallout";
            FormBorderStyle = FormBorderStyle.None;
            ClientSize = new Size(W, H);
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = Pip.Dark;
            ForeColor = Pip.Green;
            DoubleBuffered = true;
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }
            try {
                using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("FathersFallout.logo.png"))
                    if (s != null) logo = Image.FromStream(s);
            } catch { }
            backdrop = MakeBackdrop();

            BuildUi();
            LoadSettings();
            Refresh2();
            Say("WELCOME, VAULT DWELLER. PICK A SAVE AND PRESS PLAY.");
            blink.Interval = 30;
            blink.Tick += delegate { Tick(); };
            blink.Start();
        }

        // ---------- look ----------

        // CRT backdrop: dark green glow, scanlines, vignette, the logo on the left. Drawn once.
        Bitmap MakeBackdrop()
        {
            var bmp = new Bitmap(W, H);
            using (var g = Graphics.FromImage(bmp)) {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                using (var b = new LinearGradientBrush(new Rectangle(0, 0, W, H), Color.FromArgb(10, 30, 18), Pip.Dark, 90f)) g.FillRectangle(b, 0, 0, W, H);
                // Left panel: a slightly lifted plate behind the logo.
                using (var b = new SolidBrush(Color.FromArgb(70, 0, 0, 0))) g.FillRectangle(b, 0, TitleH, LeftW, H - TitleH);
                using (var path = new GraphicsPath()) {
                    path.AddEllipse(LeftW / 2 - 230, 70, 460, 460);
                    using (var pg = new PathGradientBrush(path)) {
                        pg.CenterColor = Color.FromArgb(60, Pip.Green);
                        pg.SurroundColors = new[] { Color.FromArgb(0, Pip.Green) };
                        g.FillPath(pg, path);
                    }
                }
                if (logo != null) {
                    float scale = Math.Min((LeftW - 40f) / logo.Width, (H - TitleH - 150f) / logo.Height);
                    int lw = (int)(logo.Width * scale), lh = (int)(logo.Height * scale);
                    int room = H - 130 - TitleH;   // between the title bar and the credits
                    g.DrawImage(logo, (LeftW - lw) / 2, TitleH + (room - lh) / 2, lw, lh);
                }
                using (var p = new Pen(Pip.Faint, 2f)) g.DrawLine(p, LeftW, TitleH + 12, LeftW, H - 12);
                // Title bar.
                using (var b = new SolidBrush(Color.FromArgb(120, 0, 0, 0))) g.FillRectangle(b, 0, 0, W, TitleH);
                using (var p = new Pen(Pip.Dim)) g.DrawLine(p, 0, TitleH, W, TitleH);
                using (var f = Pip.Font(10f, FontStyle.Bold)) Pip.GlowText(g, "FATHERSFALLOUT  //  CO-OP TERMINAL", f, Pip.Green, new PointF(14, 8));
                // Scanlines and vignette over everything.
                using (var p = new Pen(Color.FromArgb(46, 0, 0, 0))) for (int y = 0; y < H; y += 3) g.DrawLine(p, 0, y, W, y);
                using (var path = new GraphicsPath()) {
                    path.AddEllipse(-W * 0.25f, -H * 0.35f, W * 1.5f, H * 1.7f);
                    using (var region = new Region(new Rectangle(0, 0, W, H))) {
                        region.Exclude(path);
                        using (var b = new SolidBrush(Color.FromArgb(90, 0, 0, 0))) g.FillRegion(b, region);
                    }
                }
                // Bezel.
                using (var p = new Pen(Pip.Mid, 2f)) g.DrawRectangle(p, 1, 1, W - 3, H - 3);
                using (var p = new Pen(Pip.Faint)) g.DrawRectangle(p, 5, 5, W - 11, H - 11);
            }
            return bmp;
        }

        protected override void OnPaintBackground(PaintEventArgs e)
        {
            e.Graphics.DrawImageUnscaled(backdrop, 0, 0);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            int x = LeftW + 30;
            using (var h = Pip.Font(12f, FontStyle.Bold)) {
                Pip.GlowText(g, "> SYSTEM CHECK", h, Pip.Green, new PointF(x, 50));
                Pip.GlowText(g, "> MODE", h, Pip.Green, new PointF(x, 282));
                Pip.GlowText(g, "> VAULT DWELLER", h, Pip.Green, new PointF(x, 384));
                Pip.GlowText(g, "> SAVE FILE", h, Pip.Green, new PointF(x, 446));
            }
            using (var f = Pip.Font(9f, FontStyle.Regular)) {
                var sf = new StringFormat { Alignment = StringAlignment.Center };
                using (var b = new SolidBrush(Pip.Mid)) g.DrawString("by FatherLamb", f, b, new RectangleF(0, H - 118, LeftW, 20), sf);
                using (var b = new SolidBrush(Pip.Dim)) {
                    g.DrawString("UPDATE 19  (ALPHA)", f, b, new RectangleF(0, H - 98, LeftW, 20), sf);
                    g.DrawString("Copyright (C) 2026 FatherLamb\nGNU GPL v3 + NOTICE terms\nNot affiliated with Bethesda", f, b, new RectangleF(0, H - 70, LeftW, 60), sf);
                }
            }
        }

        // Drag the window by its title bar or the logo panel.
        protected override void OnMouseDown(MouseEventArgs e)
        {
            base.OnMouseDown(e);
            if (e.Button == MouseButtons.Left && (e.Y < TitleH || e.X < LeftW)) {
                ReleaseCapture();
                SendMessage(Handle, 0xA1, 2, 0);   // WM_NCLBUTTONDOWN, HTCAPTION
            }
        }

        Label AddText(string text, int x, int y, Color color, float size)
        {
            var l = new Label { Text = text, AutoSize = true, Location = new Point(x, y), BackColor = Color.Transparent, ForeColor = color };
            l.Font = Pip.Font(size, FontStyle.Regular);
            Controls.Add(l);
            return l;
        }

        void Place(Control c, int x, int y, int w, int h)
        {
            c.Location = new Point(x, y);
            c.Size = new Size(w, h);
            Controls.Add(c);
        }

        void BuildUi()
        {
            int x = LeftW + 30, right = W - 30;
            Place(minimize, W - 82, 6, 34, 22);
            Place(close, W - 44, 6, 34, 22);
            close.Accent = Pip.Amber;
            minimize.Click += delegate { WindowState = FormWindowState.Minimized; };
            close.Click += delegate { Close(); };

            Place(install, x, 236, 310, 30);
            install.Click += delegate { InstallMod(true); Refresh2(); };
            Place(gameButton, x + 320, 236, 150, 30);
            Place(shareLog, x + 480, 236, right - x - 480, 30);
            shareLog.Click += delegate { ShareLog(); };
            gameButton.Click += delegate { PickGameDir(); };

            Place(joinOpt, x, 312, 220, 32);
            Place(hostOpt, x + 234, 312, 140, 32);
            joinOpt.Click += delegate { SetMode(false); };
            hostOpt.Click += delegate { SetMode(true); };

            ipLabel.Text = "HOST'S IP"; portLabel.Text = "PORT";
            foreach (var l in new[] { ipLabel, portLabel }) {
                l.AutoSize = true; l.BackColor = Color.Transparent; l.ForeColor = Pip.Mid; l.Font = Pip.Font(9.5f, FontStyle.Regular);
                Controls.Add(l);
            }
            ipLabel.Location = new Point(x + 390, 296);
            Place(ip, x + 390, 314, 150, 30);
            portLabel.Location = new Point(x + 550, 296);
            Place(port, x + 550, 314, right - x - 550, 30);
            Place(ipButton, x + 390, 312, right - x - 390, 32);
            // Join through Steam (default where Steam runs the game) or by IP (GOG, or no Steam friend).
            steamLabel.Text = "FRIEND CODE OR NAME (OPTIONAL)";
            steamLabel.AutoSize = true; steamLabel.BackColor = Color.Transparent; steamLabel.ForeColor = Pip.Mid;
            steamLabel.Font = Pip.Font(9.5f, FontStyle.Regular); steamLabel.Location = new Point(x + 390, 296);
            Controls.Add(steamLabel);
            Place(steamWho, x + 390, 314, right - x - 390, 30);
            Place(steamOpt, x, 350, 220, 28);
            Place(ipOpt, x + 234, 350, 170, 28);
            steamOpt.Click += delegate { SetVia(true); };
            ipOpt.Click += delegate { SetVia(false); };
            ipButton.Click += delegate { ShowHostIp(); };
            hostInfo.AutoSize = true; hostInfo.BackColor = Color.Transparent; hostInfo.ForeColor = Pip.Mid;
            hostInfo.Font = Pip.Font(9f, FontStyle.Regular); hostInfo.Location = new Point(x, 352);
            Controls.Add(hostInfo);

            Place(name, x, 410, 260, 30);

            Place(saveButton, x, 472, right - x, 34);
            saveButton.Font = Pip.Font(10.5f, FontStyle.Regular);
            saveMenu.Renderer = new PipRenderer();
            saveMenu.BackColor = Pip.Dark;
            saveMenu.ShowImageMargin = false;
            saveMenu.Font = Pip.Font(10.5f, FontStyle.Regular);
            saveButton.Click += delegate {
                saveMenu.MinimumSize = new Size(saveButton.Width, 0);
                saveMenu.MaximumSize = new Size(saveButton.Width, 330);   // longer lists scroll
                saveMenu.Show(saveButton, new Point(0, saveButton.Height));
            };
            AddText("NO SAVE OF YOUR OWN YET? START FALLOUT 4, CHOOSE NEW, LEAVE VAULT 111, SAVE.", x, 510, Pip.Dim, 8.5f);

            Place(play, right - 220, 540, 220, 52);
            play.Big = true;
            play.Font = Pip.Font(18f, FontStyle.Bold);
            play.Click += delegate { Play(); };

            status.AutoSize = false; status.BackColor = Color.Transparent; status.ForeColor = Pip.Green;
            status.Font = Pip.Font(10f, FontStyle.Regular);
            Place(status, x, 544, right - x - 240, 48);
        }

        bool viaSteam = true;

        void SetMode(bool host)
        {
            hostOpt.Selected = host; joinOpt.Selected = !host;
            hostOpt.Text = host ? "[X] HOST" : "[ ] HOST";
            joinOpt.Text = host ? "[ ] JOIN A FRIEND" : "[X] JOIN A FRIEND";
            ipButton.Visible = hostInfo.Visible = host;
            steamOpt.Visible = ipOpt.Visible = !host;
            if (host) {
                string code = MyFriendCode();
                hostInfo.Text = code != "" ? "STEAM FRIENDS JOIN YOU WITH NO IP.  YOUR FRIEND CODE: " + code + "   (GOG FRIENDS: MY IP)"
                                           : "FRIENDS JOIN BY YOUR IP (PRESS MY IP FOR FRIENDS).";
            }
            SetVia(viaSteam);
            joinOpt.Invalidate(); hostOpt.Invalidate();
        }

        void SetVia(bool steam)
        {
            viaSteam = steam;
            bool join = !hostOpt.Selected;
            steamOpt.Selected = steam; ipOpt.Selected = !steam;
            steamOpt.Text = steam ? "[X] THROUGH STEAM" : "[ ] THROUGH STEAM";
            ipOpt.Text = steam ? "[ ] BY IP (GOG)" : "[X] BY IP (GOG)";
            ip.Visible = port.Visible = ipLabel.Visible = portLabel.Visible = join && !steam;
            steamWho.Visible = steamLabel.Visible = join && steam;
            steamOpt.Invalidate(); ipOpt.Invalidate();
        }

        // This PC's Steam friend code (the number under Steam > Friends > Add a Friend), "" without Steam.
        static string MyFriendCode()
        {
            try {
                using (var k = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam\ActiveProcess")) {
                    object v = k == null ? null : k.GetValue("ActiveUser");
                    if (v is int && (int)v != 0) return ((uint)(int)v).ToString();
                }
            } catch { }
            return "";
        }

        // GOG copies have no Steam in the game folder: those join by IP.
        bool SteamGame() { return gameDir == "" || File.Exists(Path.Combine(gameDir, "steam_api64.dll")); }

        // Status line: typed out a few letters per tick, with a blinking block cursor.
        void Say(string text) { statusText = "> " + text; typing = ""; }

        void Tick()
        {
            if (typing.Length < statusText.Length) {
                typing = statusText.Substring(0, Math.Min(statusText.Length, typing.Length + 2));
                status.Text = typing + "█";
                return;
            }
            if (Environment.TickCount / 500 % 2 == (cursorOn ? 1 : 0)) {
                cursorOn = !cursorOn;
                status.Text = typing + (cursorOn ? "█" : " ");
            }
        }

        // ---------- settings ----------

        void LoadSettings()
        {
            gameDir = Ini.Get(settings, "Launcher", "GameDir", "");
            if (gameDir == "" || !File.Exists(Path.Combine(gameDir, "Fallout4.exe"))) gameDir = FindGameDir();
            ip.Box.Text = Ini.Get(settings, "Launcher", "Server", "");
            port.Box.Text = Ini.Get(settings, "Launcher", "Port", "7779");
            name.Box.Text = Ini.Get(settings, "Launcher", "Name", Environment.UserName);
            steamWho.Box.Text = Ini.Get(settings, "Launcher", "SteamHost", "");
            viaSteam = Ini.Get(settings, "Launcher", "JoinVia", SteamGame() ? "Steam" : "IP") == "Steam";
            SetMode(Ini.Get(settings, "Launcher", "Mode", "Join") == "Host");
        }

        void SaveSettings()
        {
            Ini.Set(settings, "Launcher", "GameDir", gameDir);
            Ini.Set(settings, "Launcher", "Server", ip.Box.Text.Trim());
            Ini.Set(settings, "Launcher", "Port", port.Box.Text.Trim());
            Ini.Set(settings, "Launcher", "Name", name.Box.Text.Trim());
            Ini.Set(settings, "Launcher", "Mode", hostOpt.Selected ? "Host" : "Join");
            Ini.Set(settings, "Launcher", "JoinVia", viaSteam ? "Steam" : "IP");
            Ini.Set(settings, "Launcher", "SteamHost", steamWho.Box.Text.Trim());
        }

        // Where the mod lives. Default: the game's own Data\F4SE\Plugins. A Mod Organizer setup can
        // point PluginsDir at its mod folder instead (and LaunchExe/LaunchArgs at MO2).
        string PluginsDir()
        {
            string over = Ini.Get(settings, "Launcher", "PluginsDir", "");
            return over != "" ? over : Path.Combine(gameDir, "Data", "F4SE", "Plugins");
        }

        static string FindGameDir()
        {
            try {
                using (var k = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\WOW6432Node\Bethesda Softworks\Fallout4")) {
                    string p = k == null ? null : k.GetValue("installed path") as string;
                    if (p != null && File.Exists(Path.Combine(p, "Fallout4.exe"))) return p.TrimEnd('\\');
                }
            } catch { }
            // Steam libraries (libraryfolders.vdf lists every library folder).
            try {
                string steam = null;
                using (var k = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam"))
                    steam = k == null ? null : k.GetValue("SteamPath") as string;
                if (steam != null) {
                    var libs = new List<string> { steam };
                    string vdf = Path.Combine(steam, "steamapps", "libraryfolders.vdf");
                    if (File.Exists(vdf))
                        foreach (var line in File.ReadAllLines(vdf)) {
                            var t = line.Trim();
                            if (t.StartsWith("\"path\"")) {
                                var parts = t.Split('"');
                                if (parts.Length >= 4) libs.Add(parts[3].Replace(@"\\", @"\"));
                            }
                        }
                    foreach (var lib in libs) {
                        string p = Path.Combine(lib, "steamapps", "common", "Fallout 4");
                        if (File.Exists(Path.Combine(p, "Fallout4.exe"))) return p;
                    }
                }
            } catch { }
            return "";
        }

        void PickGameDir()
        {
            using (var d = new FolderBrowserDialog()) {
                d.Description = "Pick your Fallout 4 folder (the one with Fallout4.exe)";
                if (d.ShowDialog(this) == DialogResult.OK) {
                    if (!File.Exists(Path.Combine(d.SelectedPath, "Fallout4.exe"))) {
                        Say("FALLOUT4.EXE ISN'T IN THAT FOLDER.");
                        return;
                    }
                    gameDir = d.SelectedPath;
                    SaveSettings();
                    Refresh2();
                    Say("GAME FOLDER SET.");
                }
            }
        }

        // ---------- checks ----------

        string BundledDll() { return Path.Combine(baseDir, "mod", "F4MPClient.dll"); }

        static string Hash(string file)
        {
            using (var sha = SHA1.Create()) using (var f = File.OpenRead(file))
                return BitConverter.ToString(sha.ComputeHash(f));
        }

        List<Check> RunChecks()
        {
            var list = new List<Check>();
            Func<string, bool, string, string, Check> add = (label, ok, detail, url) => {
                var c = new Check { Label = label, Ok = ok, Detail = detail, Url = url };
                list.Add(c);
                return c;
            };
            bool game = gameDir != "" && File.Exists(Path.Combine(gameDir, "Fallout4.exe"));
            add("FALLOUT 4", game, game ? GameVersion() : "NOT FOUND - USE [ GAME FOLDER ]", null);
            if (!game) return list;
            bool f4se = File.Exists(Path.Combine(gameDir, "f4se_loader.exe")) && File.Exists(Path.Combine(gameDir, "f4se_1_11_240.dll"));
            add("F4SE", f4se, f4se ? "INSTALLED" : "MISSING - LOADER + DLL GO NEXT TO FALLOUT4.EXE", "https://f4se.silverlock.org/");
            string plugins = Path.Combine(gameDir, "Data", "F4SE", "Plugins");
            bool addr = File.Exists(Path.Combine(plugins, "version-1-11-240-0.bin")) || ModOrganizer();
            add("ADDRESS LIBRARY", addr, addr ? "INSTALLED" : "MISSING - .BIN GOES IN DATA\\F4SE\\PLUGINS",
                "https://www.nexusmods.com/fallout4/mods/47327");
            bool menu = File.Exists(Path.Combine(plugins, "F4SEMenuFramework.dll")) || ModOrganizer();
            add("MENU FRAMEWORK", menu, menu ? "INSTALLED" : "MISSING - F4SE, INTERFACE, SCRIPTS INTO DATA",
                "https://www.nexusmods.com/fallout4/mods/105090");
            string installed = Path.Combine(PluginsDir(), "F4MPClient.dll");
            bool have = File.Exists(installed), bundled = File.Exists(BundledDll());
            bool current = have && bundled && Hash(installed) == Hash(BundledDll());
            add("FATHERSFALLOUT", current, !have ? "NOT INSTALLED - PLAY INSTALLS IT" :
                (current ? "UP TO DATE" : (bundled ? "OLDER VERSION - PLAY UPDATES IT" : "INSTALLED")), null);
            var extra = ExtraPlugins();
            add("MOD LIST", extra.Count == 0, extra.Count == 0 ? "GAME + DLC ONLY (MATCHES THE HOST)" :
                "EXTRA: " + string.Join(", ", extra.Take(3)).ToUpper() + (extra.Count > 3 ? " ..." : "") + " - HOST NEEDS THE SAME", null);
            return list;
        }

        bool ModOrganizer() { return Ini.Get(settings, "Launcher", "PluginsDir", "") != ""; }

        string GameVersion()
        {
            try {
                var v = FileVersionInfo.GetVersionInfo(Path.Combine(gameDir, "Fallout4.exe"));
                return "VERSION " + v.FileVersion + (v.FileVersion.StartsWith("1.11.240") ? "" : "  (NEEDS 1.11.240)");
            } catch { return ""; }
        }

        // Active plugins beyond the base game, DLC and Creation Club (the host plays with exactly those).
        List<string> ExtraPlugins()
        {
            var extra = new List<string>();
            try {
                string file = Ini.Get(settings, "Launcher", "PluginsTxt",
                    Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Fallout4", "plugins.txt"));
                if (!File.Exists(file)) return extra;
                var base_ = new HashSet<string>(StringComparer.OrdinalIgnoreCase) {
                    "Fallout4.esm", "DLCRobot.esm", "DLCworkshop01.esm", "DLCCoast.esm", "DLCworkshop02.esm",
                    "DLCworkshop03.esm", "DLCNukaWorld.esm", "DLCUltraHighResolution.esm" };
                foreach (var line in File.ReadAllLines(file)) {
                    var t = line.Trim();
                    if (!t.StartsWith("*")) continue;
                    t = t.Substring(1);
                    if (base_.Contains(t) || t.StartsWith("cc", StringComparison.OrdinalIgnoreCase)) continue;
                    extra.Add(t);
                }
            } catch { }
            return extra;
        }

        // ---------- saves ----------

        static string SavesDir()
        {
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "My Games", "Fallout4", "Saves");
        }

        static string ReadWString(BinaryReader r)
        {
            int n = r.ReadUInt16();
            return Encoding.UTF8.GetString(r.ReadBytes(n));
        }

        // .fos header: "FO4_SAVEGAME", header size, version, save number, player name, level, location...
        static SaveInfo ReadSave(string file)
        {
            var s = new SaveInfo { File = file, Name = Path.GetFileNameWithoutExtension(file), When = File.GetLastWriteTime(file) };
            try {
                using (var r = new BinaryReader(File.OpenRead(file))) {
                    if (Encoding.ASCII.GetString(r.ReadBytes(12)) != "FO4_SAVEGAME") return s;
                    r.ReadUInt32(); r.ReadUInt32(); r.ReadUInt32();
                    s.Player = ReadWString(r);
                    s.Level = (int)r.ReadUInt32();
                    s.Location = ReadWString(r);
                }
            } catch { }
            return s;
        }

        void LoadSaves()
        {
            saves = new List<SaveInfo>();
            saveMenu.Items.Clear();
            var dir = SavesDir();
            if (Directory.Exists(dir))
                saves = new DirectoryInfo(dir).GetFiles("*.fos").OrderByDescending(f => f.LastWriteTime).Take(40)
                    .Select(f => ReadSave(f.FullName)).ToList();
            foreach (var s in saves) {
                var item = new ToolStripMenuItem(s.ToString()) { ForeColor = Pip.Green, Tag = s };
                item.Click += delegate(object o, EventArgs e) { PickSave((SaveInfo)((ToolStripItem)o).Tag); };
                saveMenu.Items.Add(item);
            }
            // The newest save is almost always the one to play (a remembered older one hid new characters).
            PickSave(saves.FirstOrDefault());
        }

        void PickSave(SaveInfo s)
        {
            save = s;
            saveButton.Text = s == null ? "[ NO SAVES FOUND ]" : "[ " + s + "  ▼ ]";
        }

        // ---------- actions ----------

        void Refresh2()
        {
            foreach (var c in checkRows) { Controls.Remove(c); c.Dispose(); }
            checkRows.Clear();
            int x = LeftW + 30, y = 80;
            foreach (var c in RunChecks()) {
                // Things PLAY fixes by itself (the mod's version) or that only might matter (extra mods)
                // are warnings, not failures.
                bool soft = c.Label == "FATHERSFALLOUT" || c.Label == "MOD LIST";
                var mark = AddText(c.Ok ? "[ OK ]" : (soft ? "[ !! ]" : "[FAIL]"), x, y, c.Ok ? Pip.Green : Pip.Amber, 10f);
                var label = AddText(c.Label.PadRight(16, '.'), x + 64, y, c.Ok ? Pip.Mid : Pip.Amber, 10f);
                var detail = AddText(c.Detail, x + 220, y, c.Ok ? Pip.Green : Pip.Amber, 10f);
                checkRows.Add(mark); checkRows.Add(label); checkRows.Add(detail);
                if (!c.Ok && c.Url != null) {
                    var link = new LinkLabel { Text = "DOWNLOAD >", AutoSize = true, BackColor = Color.Transparent, LinkColor = Pip.Amber,
                        ActiveLinkColor = Pip.Green, Font = Pip.Font(10f, FontStyle.Bold), Location = new Point(W - 140, y) };
                    string url = c.Url;
                    link.LinkClicked += delegate { Process.Start(url); };
                    Controls.Add(link);
                    checkRows.Add(link);
                }
                y += 24;
            }
            LoadSaves();
        }

        bool InstallMod(bool tell)
        {
            try {
                if (!File.Exists(BundledDll())) {
                    if (tell) Say("THE LAUNCHER'S 'MOD' FOLDER IS MISSING F4MPCLIENT.DLL.");
                    return false;
                }
                Directory.CreateDirectory(PluginsDir());
                File.Copy(BundledDll(), Path.Combine(PluginsDir(), "F4MPClient.dll"), true);
                if (tell) Say("FATHERSFALLOUT INSTALLED.");
                return true;
            } catch (Exception e) {
                Say("COULDN'T INSTALL: " + e.Message.ToUpper() + " - RUN THE LAUNCHER AS ADMINISTRATOR ONCE.");
                return false;
            }
        }

        static IEnumerable<string> LanAddresses()
        {
            return Dns.GetHostEntry(Dns.GetHostName()).AddressList
                .Where(a => a.AddressFamily == AddressFamily.InterNetwork && !IPAddress.IsLoopback(a)).Select(a => a.ToString());
        }

        void ShowHostIp()
        {
            string lan = string.Join(", ", LanAddresses());
            string pub = "?";
            try {
                ServicePointManager.SecurityProtocol = (SecurityProtocolType)3072;   // TLS 1.2
                using (var wc = new WebClient()) pub = wc.DownloadString("https://api.ipify.org").Trim();
            } catch { }
            hostInfo.Text = "INTERNET: " + pub + "    SAME HOUSE: " + lan;
            if (pub != "?") Clipboard.SetText(pub);
            Say("INTERNET IP COPIED. FRIENDS ONLINE NEED UDP PORT " + port.Box.Text + " FORWARDED TO THIS PC.");
        }

        string ServerExe()
        {
            string over = Ini.Get(settings, "Launcher", "ServerExe", "");
            return over != "" ? over : Path.Combine(baseDir, "server", "F4MPServer.exe");
        }

        // Windows Firewall approves a program by its path. Every update came in a new folder, so the
        // server was a "new" program each time and one cancelled prompt blocked all friends.
        // The server always runs from the same folder; updates are copied over it.
        string StableServerExe()
        {
            string src = Path.GetDirectoryName(ServerExe());
            string dst = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "FathersFallout", "server");
            try {
                Directory.CreateDirectory(dst);
                foreach (string f in Directory.GetFiles(src)) {
                    if (Path.GetFileName(f).ToLowerInvariant() == "logs.log") continue;
                    string to = Path.Combine(dst, Path.GetFileName(f));
                    var a = new FileInfo(f);
                    var b = new FileInfo(to);
                    if (!b.Exists || a.Length != b.Length || a.LastWriteTimeUtc != b.LastWriteTimeUtc) File.Copy(f, to, true);
                }
                return Path.Combine(dst, Path.GetFileName(ServerExe()));
            } catch (Exception) {
                return ServerExe();   // the copy failed (server still running?): use the package's own
            }
        }

        // Bug reports: a copy of the co-op log on the Desktop with personal details blanked out (IP
        // addresses, Steam IDs, friend codes, the Windows user name in folder paths), then the report form.
        void ShareLog()
        {
            string log = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "My Games", "Fallout4", "F4SE", "FathersFallout.log");
            if (!File.Exists(log)) { Say("NO CO-OP LOG YET - PLAY A SESSION FIRST (THE LOG IS MADE WHEN THE GAME STARTS)."); return; }
            try {
                string text;
                using (var f = new FileStream(log, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                using (var r = new StreamReader(f)) text = r.ReadToEnd();
                text = CleanLog(text);
                string outFile = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory),
                    "FathersFallout-log-" + DateTime.Now.ToString("yyyyMMdd-HHmm") + ".txt");
                File.WriteAllText(outFile, "FathersFallout log for a bug report (personal details removed by the launcher)\r\n" +
                    "Launcher " + Assembly.GetExecutingAssembly().GetName().Version + "\r\n\r\n" + text);
                Process.Start("explorer.exe", "/select,\"" + outFile + "\"");
                try { Process.Start(new ProcessStartInfo(IssuesUrl) { UseShellExecute = true }); } catch { }
                Say("LOG SAVED TO YOUR DESKTOP WITH PERSONAL INFO REMOVED. ATTACH IT TO THE BUG REPORT PAGE THAT JUST OPENED.");
            } catch (Exception e) {
                Say("COULDN'T COPY THE LOG: " + e.Message.ToUpper());
            }
        }

        static string CleanLog(string t)
        {
            // IPv4 addresses except 127.0.0.1 (this PC) and version numbers (0.x.x.x).
            t = System.Text.RegularExpressions.Regex.Replace(t, @"\b(?!127\.0\.0\.1\b)([1-9]\d{0,2})\.\d{1,3}\.\d{1,3}\.\d{1,3}\b", "x.x.x.x");
            t = System.Text.RegularExpressions.Regex.Replace(t, @"\b7656119\d{10}\b", "[steam id]");
            t = System.Text.RegularExpressions.Regex.Replace(t, @"(friend code )\d+", "$1[hidden]", System.Text.RegularExpressions.RegexOptions.IgnoreCase);
            t = System.Text.RegularExpressions.Regex.Replace(t, @"([A-Za-z]:\\Users\\)[^\\/\r\n]+", "$1[user]", System.Text.RegularExpressions.RegexOptions.IgnoreCase);
            t = System.Text.RegularExpressions.Regex.Replace(t, @"(steam:)[^\s""']+", "$1[host]", System.Text.RegularExpressions.RegexOptions.IgnoreCase);
            return t;
        }

        void Play()
        {
            if (save == null) { Say("PICK A SAVE FIRST (OR MAKE ONE: NEW GAME, LEAVE VAULT 111, SAVE)."); return; }
            bool host = hostOpt.Selected;
            if (!host && viaSteam && !SteamGame()) { Say("THIS COPY OF FALLOUT 4 ISN'T ON STEAM (GOG?): CHOOSE BY IP."); return; }
            if (!host && !viaSteam && ip.Box.Text.Trim() == "") { Say("TYPE THE HOST'S IP ADDRESS."); return; }
            var failed = RunChecks().Where(c => !c.Ok && c.Label != "MOD LIST" && c.Label != "FATHERSFALLOUT").ToList();
            if (failed.Count > 0) {
                Say("MISSING: " + string.Join(", ", failed.Select(c => c.Label)) + ". USE THE DOWNLOAD LINKS ABOVE.");
                return;
            }
            if (!InstallMod(false)) return;   // always play with the launcher's version
            SaveSettings();

            string ini = Path.Combine(PluginsDir(), "F4MP.ini");
            Ini.Set(ini, "Connect", "AutoConnect", "1");
            Ini.Set(ini, "Connect", "Host", host ? "1" : "0");
            string who = steamWho.Box.Text.Trim();
            Ini.Set(ini, "Connect", "Server", host ? "127.0.0.1" : viaSteam ? (who == "" ? "steam" : "steam:" + who) : ip.Box.Text.Trim());
            Ini.Set(ini, "Connect", "Steam", "1");   // a host also takes Steam friends
            Ini.Set(ini, "Connect", "Port", port.Box.Text.Trim());
            Ini.Set(ini, "Connect", "Name", name.Box.Text.Trim());
            if (host && File.Exists(ServerExe())) Ini.Set(ini, "Connect", "ServerExe", StableServerExe());
            Ini.Set(ini, "Launch", "LoadSave", save.Name);

            string exe = Ini.Get(settings, "Launcher", "LaunchExe", Path.Combine(gameDir, "f4se_loader.exe"));
            string args = Ini.Get(settings, "Launcher", "LaunchArgs", "");
            try {
                var psi = new ProcessStartInfo(exe, args) { WorkingDirectory = Path.GetDirectoryName(exe), UseShellExecute = true };
                Process.Start(psi);
                Say("STARTING FALLOUT 4... LOADING " + save.Player.ToUpper() + " AND " + (host ? "HOSTING." : viaSteam ? "JOINING THROUGH STEAM." : "JOINING.") + " GOOD LUCK OUT THERE.");
            } catch (Exception e) {
                Say("COULDN'T START THE GAME: " + e.Message.ToUpper());
            }
        }
    }

    static class Program
    {
        [STAThread]
        static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
        }
    }
}
