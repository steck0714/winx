deliver16 = deliver14(全機能+SD修正) + SDMMC診断/静止処理
bl33.bin            : 通常(XHC0 ACPIなし)。SDM1(NVDA0212/PNP0D40)は従来どおりACPIに出る
bl33_xhci_acpi.bin  : 上に加え -D SWITCH_ACPI_XHCI=1 (XHC0=PNP0D10 をACPIに出す。実機未検証)
追加: SdDiag変数/起動前コンソールに SDHCI caps/ver/pwr/clk とTegra vendor reg(0x100,0x120,0x10c...)を出力
      ExitBootServicesでSDMMC1の割込み/ステータスを静止(電源・クロック・カードは触らない)
未検証: 実機未テスト。これは「Windows標準ドライバ(sdport/usbxhci)が通る条件を探る足場」で、ドライバ本体ではない。

winx-bl33.patch: steck0714/winx (branch usually, 9eba951) に対する差分(release/ patches/ を除く)。
  適用: git apply winx-bl33.patch  (winx-FULL本体のツリーで確認済み: 適用後は私のツリーと完全一致)
