# switchaudio を GitHub Actions でビルドして署名する

hayato41200/Windows-for-Switch-Drivers の `switchaudio`(スピーカー用の PortCls / WaveRT ドライバ、GPL-2.0)の、ビルドと署名の一式。
ソースは 1.0.0.2(2026-11-01)のまま。変えたのは、GitHub Actions のワークフロー(`.github/workflows/build-switchaudio.yml`)を足しただけ。

## 使い方

1. この zip の中身を、自分の GitHub のリポジトリに置く(`.github` を含めて、そのままのフォルダ構成で)。
2. Actions タブで `build-switchaudio` を選び、`Run workflow` を押す。
3. 終わると、`Switch-Audio` という Artifact ができる。中身:
   - `Switch-Audio\switchaudio.sys / .inf / .cat / Audio.ps1`
   - `SwitchWOA-Test.cer`(署名の証明書)

## 署名の証明書

- **何もしない場合:** 実行のたびに、新しい自己署名証明書(`CN=SwitchWOA Test Driver Signing`)ができる。毎回違う証明書なので、
  その回の `SwitchWOA-Test.cer` を、Windows の Trusted Root と Trusted Publishers に入れる必要がある(オフラインなら `add-cert-offline.ps1`)。
  すでに hayato41200 の証明書を入れている場合も、別の証明書なので、もう一度入れる。
- **同じ証明書を使い続ける場合:** 証明書を 1 回だけ作って、`.pfx` にする。Settings > Secrets に、
  `SWOA_PFX_BASE64`(`.pfx` の base64)と `SWOA_PFX_PASSWORD` を入れる。以降は、同じ証明書で署名される。
  - `.pfx` を Artifact やリポジトリに置かないこと(秘密鍵が入っている)。

## 入れ方

Windows(テストモード、証明書の登録済み)で、管理者の PowerShell から `Audio.ps1 -Mode Install`。
UEFI は R402 以降の bl33 が必要(`ACPI\SWOA0C01` と、起動時のクロックとコーデックの初期化)。

## 注意

- このワークフローは、**書いただけで、まだ動かしていない**。WDK 10.0.19041 のインストールや、LLVM のバージョンで、止まるかもしれない。
  止まったら、ログに出たパスやメッセージを見て直す。
- ドライバは、まだ試験中(作者の README より)。実機での動作は、私は確認していない。
