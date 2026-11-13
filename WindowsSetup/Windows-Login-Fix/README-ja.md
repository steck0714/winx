# Switch上のWindowsでローカルアカウントにログインできない問題の修正手順

## 症状

ロック画面で、`Switch`（他に Administrator / Guest がある）を選び、パスワードを入れてログインしようとすると、「The specified domain either does not exist or could not be contacted.」と表示されて進まない。

## 現状（`LoginState-20261024-191413\REPORT.txt`、SDは読むだけで確認）

**[事実]**
- **セットアップは完了している:** `SYSTEM\Setup` は `OOBEInProgress=0`、`SystemSetupInProgress=0`、`SetupType=0`。ImageStateは `IMAGE_STATE_UNDEPLOYABLE`（通常の完了状態）。
- **アカウントは作られている:** unattendの `oobeSystem` パスは処理済み（Pantherのunattend.xmlに `wasPassProcessed="true"`）。`Switch` アカウントはこのとき作られた。
- **一度もログオンしていない:** ProfileListに、ユーザーのプロファイル（S-1-5-21-…-1000）が無い。
- **PC名は変わっていない:** `DESKTOP-XXXXXXX` のまま。unattendの `<ComputerName>` は oobeSystem パスでは使われないため。
- **Winlogonの設定:** DefaultUserName=Switch、**DefaultDomainName=`.`**、AutoAdminLogon=0、ForceAutoLogon=1。Codexが9月に、オフラインで何度か書き換えた（R19〜R21）。
- **読めなかったもの:** ローカルユーザーの一覧（SAM）と、LSAのローカルドメイン名（SECURITY）。どちらもSYSTEM権限でしか読めない。

**[推測]** ローカルアカウントで「ドメインが存在しない」と出るのは、Windowsがローカルの「アカウントドメイン名」（通常はPC名）と一致しない名前でログオンしようとしたとき。オフラインでの書き換えの影響や、自動ログオンの設定の残りが関係している可能性がある。Switch上で、ログオン前にSYSTEM権限で状態を見て、直すのが確実。

## 手順

### 1. PCで: 起動時にSYSTEMのコマンドプロンプトを出す設定にする（SDに書き込む）

SDをPCに挿し、**管理者のPowerShell**で次を実行する（`F:` はWindowsパーティションのドライブ文字に置き換え）。

```powershell
powershell -ExecutionPolicy Bypass -File D:\Codex\SwitchWOA-Bootstrap\outputs\Windows-Login-Fix\Set-SwitchBootCmd.ps1 -WindowsDrive F: -Mode Enable
```

- 書き込む前に、SDのSYSTEMハイブを `Windows-Login-Fix\SYSTEM-backup-<日時>\` にコピーする。
- `HKLM\SYSTEM\Setup` を `SetupType=2`、`CmdLine=cmd.exe` にする。Windowsは起動の途中、ロック画面の前に、このコマンドをSYSTEM権限で実行する。
- 終わったら、SDを安全に取り外す。

### 2. Switchで: SYSTEMのコマンドプロンプトで調べて直す

R362（またはR360以降）とSD、USBキーボードのレシーバーで起動する。ロック画面の代わりに、黒いコマンドプロンプトの画面が出るはず。

**(a) 調べる**（結果を `C:\login-diag.txt` に保存する。1行ずつ入力）
```bat
hostname > C:\login-diag.txt
whoami >> C:\login-diag.txt
net user >> C:\login-diag.txt
net user Switch >> C:\login-diag.txt
net localgroup Administrators >> C:\login-diag.txt
wmic useraccount get name,domain,sid,disabled,localaccount >> C:\login-diag.txt
wmic computersystem get name,domain,partofdomain,workgroup >> C:\login-diag.txt
type C:\login-diag.txt
```
`wmic useraccount` の **domain** の列が、`hostname`（DESKTOP-XXXXXXX）と一致しているかが重要。

**(b) 直す**
```bat
net user Switch *
net user Switch /active:yes
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v DefaultDomainName /t REG_SZ /d %COMPUTERNAME% /f
reg delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v ForceAutoLogon /f
```
- `net user Switch *` を実行すると、新しいパスワードを2回聞かれる（入力した文字は表示されない）。覚えやすいものにしてよい。
- 予備として、別の管理者アカウントも作っておくと安心（名前とパスワードは自分で決める）。
  ```bat
  net user <新しい名前> * /add
  net localgroup Administrators <新しい名前> /add
  ```

**(c) 通常の起動に戻す**（必ず実行してから閉じる）
```bat
reg add HKLM\SYSTEM\Setup /v SetupType /t REG_DWORD /d 0 /f
reg add HKLM\SYSTEM\Setup /v CmdLine /t REG_SZ /d "" /f
exit
```
`exit` の後、Windowsは起動を続けるか再起動して、ロック画面になる。`Switch`（または予備のアカウント）で、新しいパスワードでログインを試す。

### 2'. 原因（2026-10-24に判明）と修正

**[事実]**
- `swoa-logon-test.ps1` で `Switch` のログオンを試すと、NTLMだけのネットワークログオンでも `1355`（ドメインが存在しない）になる。つまり、ローカルアカウントの検証（MSV1_0）自体が失敗している。
- SECURITYハイブの `Policy\PolPrDmN`（LSAのプライマリドメイン = ワークグループ名）が**長さ0の空の値**になっている。
- 正常なPCでは、ここは `PrimaryDomain='WORKGROUP'`（SIDなし）。

**[推測]** unattendに `specialize` パスが無く、ワークグループ参加（UnattendedJoin）が一度も実行されなかったために、この値が空のまま残った。

**修正:** SYSTEMのコマンドプロンプトで、`swoa-fix-primarydomain.ps1 -Fix` を実行する。`LsaSetInformationPolicy` でワークグループ `WORKGROUP` を設定する（ワークグループ参加と同じ処理）。

### 3. うまくいかなかったとき

- **(c) を実行し忘れた場合:** 毎回コマンドプロンプトが出る。PCで `-Mode Disable` を実行すれば戻せる。
  ```powershell
  powershell -ExecutionPolicy Bypass -File D:\Codex\SwitchWOA-Bootstrap\outputs\Windows-Login-Fix\Set-SwitchBootCmd.ps1 -WindowsDrive F: -Mode Disable
  ```
- **ログインできなかった場合:** SDをPCに挿して `C:\login-diag.txt`（SDのWindowsパーティションのルート）を共有してもらえれば、原因を詰める。
- **SYSTEMハイブを元に戻したい場合:** `SYSTEM-backup-<日時>` のファイルを、SDの `Windows\System32\config\` に戻す。
