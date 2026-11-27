# 参考项目与致谢

项目能够逐步落地，离不开公开研究、开源代码、刷写工具和参与实测的车友。下表说明各项帮助的具体位置，而不是把所有链接都写成运行依赖。

## 实际复用的代码

| 项目与作者 | 本项目使用的位置 | 来源与修改说明 |
|---|---|---|
| [mib2q-carplay-rgi / LuKa](https://github.com/luka-dev/mib2q-carplay-rgi) | `src/ble-r18/core/vendor/rgd/` 的路线引导 TLV 定义与解析 | 基线提交 [a63d547](https://github.com/luka-dev/mib2q-carplay-rgi/tree/a63d54741839c7d30d32dd4e149f693856bcb0db)，原路径 hook/routeguidance；保留版权和 GPL 许可，本地修改见 core/NOTICE.md。该项目的 Audi/QNX 安装与 BAP 输出不等于信界 BLE。 |

## 研究参考与行为对照

| 项目或讨论 | 研究中参考了什么 | 适用边界 |
|---|---|---|
| [mazda-carplay-hud / KidMixer](https://github.com/KidMixer/mazda-carplay-hud) | 马自达 CarPlay 导航信息捕获与导航状态处理的对照思路 | 原车 OEM HUD / JCI 通道与信界 HUD 蓝牙不同；本仓库未收录它的 OEM D-Bus 发送模块。原项目的依赖与许可仍归其各自作者。 |
| [wireless-carplay-dongle-reverse-engineering / ludwig-v](https://github.com/ludwig-v/wireless-carplay-dongle-reverse-engineering) | 无线盒子的型号差异、固件与文件系统、Flash 提取研究方法 | 原研究中的 CPlay2Air/Carlinkit 型号不自动等于 U5/U6A；不能把其 SSH、拆机或跨刷步骤当作本项目通用教程。研究快照为 b437135316275a5d3c375bdd1ceac2a6bcf1315b。 |
| [node-CarPlay / rhysmorgan134](https://github.com/rhysmorgan134/node-CarPlay) | USB 盒子的主机侧消息与驱动架构对照 | AutoKit/CCPA 类主机侧 USB 接入与原车有线转无线盒子属于不同集成场景；不是本项目的运行依赖。研究快照为 670f19eda2a0b0047a5a538b9602b263f442433a。 |
| [DiPlay 车道与导航结束讨论 #511](https://github.com/shihabal3amri/DiPlay/issues/511) | 对照车道显示、转向卡与结束清屏的用户现象 | 讨论与单个设备的日志不是协议规范，也不能据此认定某地图应用永久支持或不支持某类数据。本项目未复用该项目代码。 |

## 工具与发布支持

- [Luxianxing-Firmware-Tool / Srysfu](https://github.com/Srysfu/Luxianxing-Firmware-Tool)：盒子固件检测和本地刷写方式；请使用原项目发布的工具，保留原作者署名。
- [Quantumult X / crossutility](https://github.com/crossutility/Quantumult-X)：自建升级查询的重写格式参考；本仓库示例只用占位域名。
- [Zig](https://ziglang.org/)：C/ARM 构建；[Unicorn](https://www.unicorn-engine.org/)、[Capstone](https://www.capstone-engine.org/)、[Keystone](https://www.keystone-engine.org/)：指令执行、反汇编与汇编检查。
- [Python](https://www.python.org/)、[Pillow](https://python-pillow.org/)、[squashfs-tools](https://github.com/plougher/squashfs-tools)：工具脚本、PNG 属性检查、固件文件系统处理。

图标设计使用 GPT 图像生成辅助并经项目作者审核；真实上车观察、问题复现和日志反馈由项目作者及车友提供。公开资料不记录参与者的设备标识或行程。

参考链接不代表这些项目的作者审核、认可或支持本项目。复用部分的法律声明以 [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES.md) 和原文件为准；署名不能替代许可证义务。
