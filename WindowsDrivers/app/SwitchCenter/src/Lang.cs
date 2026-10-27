// UI language: Japanese or English. Saved per user in
// HKCU\Software\SwitchWOA\SwitchCenter\Language ("ja" / "en"); the first run
// follows the Windows display language (Japanese if it is Japanese).
using System.Globalization;
using Microsoft.Win32;

namespace SwitchCenter
{
    static class Lang
    {
        const string Key = @"Software\SwitchWOA\SwitchCenter";

        public static bool English { get; private set; }

        public static void Load()
        {
            object v;
            using (RegistryKey k = Registry.CurrentUser.OpenSubKey(Key)) v = k?.GetValue("Language");
            English = v is string s ? s == "en" : CultureInfo.CurrentUICulture.TwoLetterISOLanguageName != "ja";
        }

        public static void Set(bool english)
        {
            English = english;
            using RegistryKey k = Registry.CurrentUser.CreateSubKey(Key);
            k.SetValue("Language", english ? "en" : "ja");
        }

        /// <summary>The text for the current language.</summary>
        public static string T(string ja, string en) => English ? en : ja;
    }
}
