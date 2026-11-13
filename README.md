# nrf54l15testapp1 — 1.1.4-cv-audit

这是用户指定的历史版本1.1.4-cv-audit，独立恢复到本文件夹。44个源码、板卡、配置、驱动和测试文件的SHA256均与原版发布清单一致。原APP和当前nrf54l15testapp工程保持原样。

## 直接烧录

使用`firmware/ecw54_cqet_cv_audit.hex`。这是已按原版SHA256核验的历史镜像；配套BIN和ELF在同一目录。不要使用其他文件夹中的同名固定入口，以免混淆版本。

烧录目标为nRF54L15应用核，板卡`ecw54/nrf54l15/cpuapp`。成功后断电重启，原APP中连接CQET，并在设备信息中确认版本`1.1.4-cv-audit`。WE0、原有蓝牙连接、参数和曲线页面继续使用原APP。

## 编译源码

本机使用nRF Connect SDK v3.4.2 / Zephyr 4.4.2及Nordic配套ARM工具链。在本文件夹打开PowerShell：

```powershell
.\scripts\build.ps1 -Pristine
```

脚本默认SDK目录`C:\ncs\v3.4.2`、工具链目录`C:\ncs\toolchains\5f5ef0cea0`。其他电脑可用`-SdkRoot`和`-ToolchainRoot`传入实际安装路径；这不是需要安装到手机上的APP工程。

本次独立重建结果与产物位置见`docs/RESTORE_VERIFICATION.json`和`build_restore.log`。`build/`是本地生成目录，不放进源码下载包；解压后执行上述命令重新生成。

## 本版本范围

- 蓝牙名称CQET；开机绿灯闪烁、连接及测量时绿灯常亮；断开后红灯闪烁约3秒再释放电源保持，实际断电取决于PCB及供电。
- CV采用阳极电流为正的符号约定，完整往返算一圈；包含逐圈、逐单程日志和CSV顺序核查工具。原APP把多圈数据放在同一结果记录，不会自动变成四个列表项。
- 这是1.1.4历史实现，没有合入后续1.1.5至1.1.11的DPV采样、诊断和RRAM档案功能。
- 原发布固件、源码一致性和本机编译/模型验证可以检查；本次没有连接手机或PCB烧录测试，不能把编译通过当成真实电化学精度或噪声通过。

## 文件

- `src/`、`include/`、`vendor/`：完整固件源码及AD5940/AD5941驱动。
- `boards/`、`prj.conf`、`Kconfig`、`CMakeLists.txt`：板卡与构建配置。
- `scripts/build.ps1`：构建入口。
- `firmware/`：原版HEX、BIN、ELF。
- `tests/`、`tools/`：原版回归测试与CSV、PCB核查工具。
- `docs/release_manifest_original_1.1.4.json`：原发布校验清单。
- `docs/RESTORE_VERIFICATION.json`：本次恢复、重新编译与验证记录。

如需保留1.1.10/1.1.11在板内保存的DPV档案，应在重新烧录前先导出；本历史版本没有该归档分区定义。
