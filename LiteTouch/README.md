# Switch Lite HDH-001 触屏驱动 R1

适配 Windows ARM64 的 GPIO、I²C3 和 STM FTM4 触屏。诊断镜像使用 WinPE 22621.1。家庭版固件针对本项目这台 Lite 的 Stage 6、当前 WINARM 和测试签名配置。

## 历史 WinPE 测试

原始 WinPE 测试使用 [触屏 R1 提交](https://github.com/ruje0504-2/winx/tree/082f60d9060426bfa87e3262640be6d26859b58f)，当前分支的固件构建直接进入家庭版。

1. 解压测试包，将 **SD 文件夹里面的内容**合并到卡根目录。在 Hekate 的 **More Configs** 选择 **Lite Touch Test R1**。
2. 等界面出现 GPIO、I2C3、FTM4 三行。驱动启动时会读取芯片 ID。期望看到三项 Started、Chip 3670、Stage 4、Status 00000000。
3. 依次点按并抬起三个蓝色圆圈。程序检查来自 Windows 的真实 PT_TOUCH 按下、抬起、位置和驱动报告计数。出现 **TOUCH PASS** 后拍照。如停在灰色圆圈或点偏，请拍全屏，保留 PnP 问题码、Chip、Stage、Status、IRQs、Events、HID reports 和坐标。该测试在 X: 运行，使用只读 SD 驱动。关机可长按电源键。

Stage 1 读取芯片 ID，Stage 2 复位并等待 ready 事件，Stage 3 开启扫描并清理 FIFO，Stage 4 完成驱动初始化。IRQs 为零而 Events/HID reports 增长表示轮询在工作。TOUCH PASS 代表触点已经送到 Windows 应用。

## 家庭版

家庭版的最新合并方式见 [Stage 6 源码说明](../docs/hdh001-home/README.md)。家庭版包分为 **SD** 和 **Windows-Drivers**。把 SD 里的内容合并到卡根目录，Hekate 会新增 **Lite Windows 11 Home**。它启动已有的 Stage 6 系统，目标限于这台机器原来的 WINARM 卷。Windows-Drivers 需置于管理员或维护环境能访问的位置。

- 给已部署的家庭版离线加驱动：在 ARM64 维护 WinPE 中确定 WINARM 的盘符，进入 Windows-Drivers，运行 `Install-LiteTouch.cmd offline W:\`，其中 W: 换成实际盘符。脚本先核对目标是离线 Windows 并已信任 Stage 6 使用的测试证书。
- 在已能操作的家庭版中，以管理员运行 `Install-LiteTouch.cmd online`，然后用 **Lite Windows 11 Home** 开机。安装后可在 Windows 中运行 `touch-check.exe`。测试包附带的证书是匹配 Stage 6 的公钥证书。
- 首次部署时也可以在 Stage 6 的 DISM 操作后对同一离线卷预装这三个 INF。普通 R1 诊断包与家庭版的启动路径彼此独立。

## 本版验证

三个驱动包均已在冷启动时钟下通过 ARM64 Windows SignTool 的 SYS 签名、INF/SYS 目录成员校验及 PnPUtil 入库，随后由 DISM 预装。已从交付 WIM 回读三个包、系统驱动、两个证书存储、启动脚本和检测程序。最终触屏 WinPE 在通用 AArch64 虚拟机上从私有 BCD 冷启动到图形检测窗口，显示了虚拟机缺少这三个 Tegra 设备时的状态。快速按下/抬起、多点、方向边界、提交重试和复位超时通过了 FIFO 回归测试。实机触摸和坐标方向以 HDH-001 的三点结果为准。

## 源码与构建

- [Windows 触屏驱动](../WindowsDrivers/drivers/SwitchTouch/src/switchtouch.c) 在每次自管理 I/O 启动时检查 0x3670、复位并开启多点扫描。它通过 VHF 提交 HID 报告。按下和抬起在同一 FIFO 批次内也分别提交，暂时提交失败会保留待重试的报告。GPIO 和 I²C 控制器沿用上游实现。
- [固件电源初始化](../Drivers/TouchPanelDxe/LiteTouchPower.c) 准备 LDO6、PJ7、I²C3 和 PX1，把 FIFO 留给 Windows。DSDT 提供八个独立的 GPIO bank 中断资源、I²C3（0x7000c500）及触屏（SWOA0502，0x49，PX1）。此分支编译时在原只读 WinPE 参数上加 `-D SWITCH_LITE_TOUCH=1`，保留 `SWITCH_HEKATE=1`、`SWITCH_ACPI_SD_NVTSDHC=1`、`SWITCH_FB_ROTATE=2`、`SWITCH_EXCFB=1`。家庭版使用同一 ACPI 和电源模块，叠加 Stage 6 原有的 NTFS 启动和磁盘身份校验。
- [触点测试应用](touch-check.c) 使用 SetupAPI、CfgMgr32 与 WM_POINTER。Windows ARM64 WDK 26100、KMDF 1.15、clang-cl/lld-link 可运行 [构建脚本](build-drivers.sh)，例如从仓库根目录 `sh LiteTouch/build-drivers.sh WindowsDrivers /path/to/output`。设置 WDK_INCLUDE、WDK_LIB、WDF_INCLUDE、WDF_LIB 指向匹配的头文件和 ARM64 库。用 mingw-w64 头文件编译检测程序时，设置 MINGW_SYSROOT 后运行 [build-check.sh](build-check.sh)。主机 FIFO 回归测试为 `python3 LiteTouch/tests/test-ftm4.py`。
- 板级接线依据为 [Switchroot T210B01 设备树](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/cf785c4c176499b301170d79fe57b77f365b73cd/kernel-dts/nx-platforms/tegra210b01-nx-common.dtsi#L830-L844) 和 [Hekate FTM4 初始化](https://github.com/CTCaer/hekate/blob/v6.5.3/bdk/input/touch.c)。Windows 控制器和 HID 源码来自 [Windows-for-Switch-Drivers](https://github.com/hayato41200/Windows-for-Switch-Drivers)。

