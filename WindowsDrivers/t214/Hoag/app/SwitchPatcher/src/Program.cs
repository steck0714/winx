// SwitchPatcher: see docs\Design-SwitchPatcher-ja.md.
using System;
using System.Text;
using System.Windows.Forms;
using SwitchCenter;

namespace SwitchPatcher
{
    static class Program
    {
        [STAThread]
        static void Main()
        {
            Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);   // dism and bcdedit print in the OEM code page
            Application.EnableVisualStyles();
            Application.SetHighDpiMode(HighDpiMode.SystemAware);
            Lang.Load();
            Application.Run(new MainForm());
        }
    }
}
