# nRF54L15 + AD5941 单通道 CV / DPV 固件

本目录是新板适配工程。原固件目录与 Android APP 未修改。目标为 `ecw54/nrf54l15/cpuapp`，不是原工程的 nRF54L10 非安全域配置。使用完整 nRF54L15 应用核资源，不依赖原 TF-M 镜像。

**交付状态：编译和模拟测试通过；已识别 USB J-Link（69730362），VTref 约 3.29 V，但两次 SWD 连接均失败，尚未写入芯片，也未完成 BLE 实连或电化学精度验证。** 编译日志与最终产物见 `build/`，验证结果见 `docs/验证记录.md`。

## VS Code 编译与烧录

1. 在 VS Code 中选择“打开文件夹”，打开本目录 `nrf54l15-1.2.0`。
2. 按 `Ctrl+Shift+B` 执行 `ECW: Build`。默认使用本机已安装的 NCS **3.4.2** 与工具链 `C:/ncs/toolchains/5f5ef0cea0`。
3. 成功后生成 `build/zephyr/zephyr.hex`、`zephyr.elf`、`zephyr.bin`。烧录使用 **HEX**；ELF 用于调试。
4. 连接下载器与供电，运行“终端 → 运行任务 → ECW: Flash”。此任务先编译，再下载、校验并复位运行。
5. 正常上电后绿灯闪烁；APP 搜索 **ECW R1**，连接后绿灯常亮。断开后红灯闪烁 4 秒，P0.02 拉低释放供电保持。

也可在本目录的 PowerShell 执行：

```powershell
.\scripts\build.ps1
.\scripts\build.ps1 -Pristine
.\scripts\flash.ps1
# 多个下载器时明确选择：
.\scripts\flash.ps1 -SerialNumber 123456789
```

更换安装位置时，编译脚本支持 `-Sdk`、`-Toolchain`，烧录脚本支持 `-JLink`。若在 Nordic 插件中创建 Build Configuration，请选择此自定义板，关闭 Sysbuild；不使用开发板默认引脚配置。项目中原有 `start.sh` 不适用 Windows，因此未作为入口保留。

## SWD 接线

|下载器信号|新板信号|
|---|---|
|SWDIO|H1.4 / SWDIO|
|SWCLK|H1.5 / SWCLK|
|GND|H1.2 / GND|
|VTref|板上 **VCC/芯片供电电压** 测试点|

**H1.3 是 VBAT，不是 VCC。** VTref 是电压参考，不等于下载器供电输出。按原理图确认供电后连接；初次下载期间必须维持板子供电，必要时按住 SW1 或用受控外部电源，因为空白芯片还不能拉高 P0.02。不要把 VBAT、VCC 或两个电源输出直接短接。脚本不会自动执行全片恢复或解除读保护。

## 新板引脚

|功能|nRF54L15 引脚|说明|
|---|---|---|
|电源保持|P0.02|高电平保持，低电平关断|
|绿灯 / 红灯|P1.07 / P1.08|低电平点亮|
|电池检测|P1.14 / AIN7|820 kΩ : 1 MΩ 分压；使用 SDK 的 900 mV 内部参考换算|
|AD5941 SCK / MOSI / MISO|P2.06 / P2.08 / P2.09|SPI mode 0，8 MHz|
|AD5941 CS / RESET|P2.10 / P2.04|低有效|
|AD5941 GPIO0 中断|P2.03|下降沿，测量同时使用有超时的寄存器查询|
|AD5941 GPIO1 / GPIO2|P2.02 / P2.01|不按旧板的多路开关脚驱动|

新板 WE0 直接连接 SE0，RE、CE 分别接 RE0、CE0。没有旧板的 ADG704、DAC80501 或 MCU 按键输入，相关模块不参与编译与初始化。32 MHz/32.768 kHz 晶振使用板上外部负载电容。MCU 主稳压器采用 LDO 模式，避免要求未核实的 DC/DC 外部电感。

AD5941 使用复位后的 16 MHz 内部 HFOSC，沿用 ADI Ramp 示例的时钟基础；本版本不启用板上的 AD5941 外部晶体。硬件定时精度与电位、电流误差仍需实测。

## APP 使用与兼容范围

APP 不需要改动。保留设备名、AABB/AAB0/AABF/AABE UUID、会话号、ACK、样本与 END 帧。

- 只选择 **WE0**。其他 WE 返回不支持错误，不会假装切换到不存在的通道。
- 支持 **CV（方法 1）**、**DPV（方法 4）**。APP 仍可能显示 CA/EIS/OCP/STIM，但此板本版本返回不支持。
- 电极电位 `E = V(WE) − V(RE)`，允许 −1000～+1000 mV。DPV 的基线加脉冲也必须在此范围。
- CV 步进必须为 1～2000 mV，扫描速率、循环数非零，两个顶点不同。每步时间及最后不足一步的区间均需 ≥10 ms；超出软件定时能力返回错误，不静默改速率。
- DPV 脉冲幅度为正，允许 1～2000 mV；脉宽 ≥10 ms，周期至少比脉宽大 10 ms。下降基线扫描仍叠加正脉冲，以保持原 APP 协议语义。
- 测量中的再次 START 返回忙；先 STOP 再 START。STOP 会话号必须匹配当前测量。
- 数据缓冲满或 SPI/ADC 错误会终止本次测量，发 ERROR/END（连接与通知可用时），不会用假数据继续画图。

推荐第一次使用 10 kΩ 档和小电位，先测已知电阻。ADC 电池百分比只是 CR2032 的近似电压映射（3.0 V→100%，2.4 V→0%），不能据此判断 AD5941 已处于有效供电范围；若实际安装的是不同电池，需修改 `driver/battery.c` 中的阈值。

## 原理与实现

CV：按 APP 定义执行 `起点→顶点1→顶点2`，随后重复 `顶点2→顶点1→顶点2`，循环结束再走到终点。每段按 `Δt=ΔE/扫描速率` 定时，反向扫描和非整步长终点均保留采样。APP 以电位为横轴、电流为纵轴绘图。

DPV：基线保持 `周期−脉宽`，取 `Ibase`；施加 `Ebase+脉冲幅度`，在脉冲末端取 `Ipulse`；APP 计算 `ΔI=Ipulse−Ibase`。静置时间在首个基线保持段之前执行。两次采样相减有助于抑制充电电流背景，但不能消除所有噪声与电极非理想性。它不是方波伏安法。

AD5941 的 LPDAC 6 位输出建立 WE 虚地，12 位输出给恒电位放大器参考，LPTIA 把电流转换为电压。采用 ADI Ramp 示例的 DAC 码差与补偿，code6=32 对应约 1.3 V，而不是原注释的 1.1 V。电流按校准后的实际 RTIA 换算为 nA，并保留原协议的符号约定；电流正负必须用已知负载再确认。

每次启动用板上 200 Ω RCAL 校准所选 RTIA；测量与官方校准使用同一 100 Ω RLOAD。APP 中“200 Ω”档是历史档位名称，在该 RLOAD 下实际内部反馈阻值接近 110 Ω，计算使用校准实值。200 Ω RCAL 与 256 kΩ/512 kΩ 档差距很大，这些高阻档的校准精度必须用标准负载验证，不能仅凭校准未报错认定准确。

定时采用独立测量工作队列，补偿本次处理耗时；它仍是软件定时，受 SPI、调度和 BLE 中断影响。本版限制了最短时段，但不承诺未经示波器验证的脉宽/扫描速率精度。需要更高速、更严格波形时应使用 AD5941 sequencer/FIFO/WUPT，而不是减小这里的保护阈值。

停止时关闭 ADC、LP 放大器、LPDAC 和内部开关。由于板上没有外部隔离开关，这不代表电极与芯片实现了物理隔离。

## 必须检查的供电条件

原理图为 LP5900-3.3 和 2032 电池座。如果实际使用普通约 3 V CR2032，**3.3 V LDO 不能升压到 3.3 V**。AD5941 的 AVDD 需要满足数据手册的 2.8～3.6 V 范围，电池内阻和无线发送峰值可能进一步降低电压。首次验证请实际测量 VCC、AVDD、DVDD 及 BLE 发射/测量时的跌落。电源不满足要求时，固件无法补偿，应调整供电方案。电极 CE 输出摆幅也受实际供电和负载限制，±1 V 参数许可不代表任意电极都能达到该电位。

完整检查流程见 [实板验收](docs/实板验收.md)。

## 查阅资料

- [ADI AD5940/AD5941 数据手册 Rev. G](https://www.analog.com/media/en/technical-documentation/data-sheets/AD5940-5941.pdf)：LPDAC、LP loop、ADC、供电及 RLOAD/RTIA 关系。
- [ADI 官方 RampTest.c](https://github.com/analogdevicesinc/ad5940-examples/blob/master/examples/AD5940_Ramp/RampTest.c)：CV 阶梯扫描、DAC 码补偿和 LP 开关配置。
- [ADI 官方驱动库](https://github.com/analogdevicesinc/ad5940lib)：本工程复用的驱动接口，增加了实际使用的 AC RTIA 校准等待上限。
- [Nordic nRF54L15 产品页](https://www.nordicsemi.com/Products/nRF54L15)、[产品规范入口](https://docs.nordicsemi.com/bundle/ps_nrf54L15/page/index.html)：官网页面访问受限；本次芯片配置依据已安装的官方 NCS 3.4.2 / Zephyr 4.4.2 板级定义、SAADC 驱动和 nrfx 头文件交叉核对。
- [Pine Research DPV 原理与采样时序](https://pineresearch.com/shop/kb/software/methods-and-techniques/voltammetric-methods/differential-pulse-voltammetry-dpv/)：脉冲末端与基线采样及差分电流。
