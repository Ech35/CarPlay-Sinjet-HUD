# 版本与下载选择

下载前在设备上核对项目号或 HUD 模块号。

[路先行盒子 IMG](#路先行盒子-img) · [信界 HUD PAK](#信界-hud-pak)

## 路先行盒子 IMG

打开盒子管理页，在设备名称下方查看完整版本。中间一段是项目号。例如下图中的 **8952** 对应 U6A，完整底包版本是 **26052617.8952.1**。

![盒子管理页版本局部，框出 26052617.8952.1 中的 8952](../assets/docs/box-project-number.webp)

| 盒子与底包 | 刷写文件 | 版本与构建 |
| :--- | :--- | :--- |
| **路先行 U6A** · `26052617.8952.1` | [R2c IMG](https://github.com/Ech35/CarPlay-Sinjet-HUD/releases/download/v0.1.1/8952_26052617.8952.1_HUD_R2c.img) | [版本说明](../releases/u6a-8952-r2c.md) · [从底包构建](build.md#u6a-r2c) |
| **路先行 U5** · `26051317.2734.2` | [R18 IMG](https://github.com/Ech35/CarPlay-Sinjet-HUD/releases/download/v0.1.1/2734_26051317.2734.2_HUD_R18.img) | [版本说明](../releases/u5-2734-r18.md) · [组件构建](build.md#u5) |

路先行原厂盒子固件、Android 刷写工具和操作步骤见[盒子刷写教程](luxianxing-flashing.md)。使用其他品牌或项目号，先看[跨品牌适配](box-porting.md)。

U6A R2c 修复了开机读取配置过早的问题。它会等待 CarPlay 就绪后恢复设备和开关，配置晚到则继续重试，并尊重用户手动关闭。

R2c 的 HUD 自动连接已据实机反馈验证通过。离线检查覆盖后台启动、ARM 接口、蓝牙和导航转发。R2b 已停用。

## 信界 HUD PAK

打开车载助手的「关于 → 模块」，核对弹窗中 **Module** 下方的完整型号。图中的 **HUDBAFB0** 对应 FB0。SA0 设备应显示 **HUDBASA0**。

![车载助手模块弹窗局部，框出 HUDBAFB0](../assets/docs/hud-module-number.webp)

| HUD 模块号 | 刷写文件 | 版本与构建 |
| :--- | :--- | :--- |
| `HUDBAFB0` | [201013 R1 PAK](https://github.com/Ech35/CarPlay-Sinjet-HUD/releases/download/v0.1.1/mcu_HUDBAFB0_201013.pak) | [版本说明](../releases/hud-bafb0-201013-r1.md) · [从底包构建](build.md#fb0-专用固件) |
| `HUDBASA0` | [230928 R3 PAK](https://github.com/Ech35/CarPlay-Sinjet-HUD/releases/download/v0.1.1/mcu_HUDBASA0.pak) | [版本说明](../releases/hud-basa0-230928-r3.md) · [图标适配](hud-upgrade.md) |

上表是改装成品。用于资源适配的原厂底包见[信界 HUD 原厂固件存档](https://github.com/Ech35/CarPlay-Sinjet-HUD/releases/tag/sinjet-hud-stock-20261127)。

FB0 补图基于同型号 201012，内部版本为 20201013，增加 40–85 共 46 个槽位。四个仪表主题共用一套 68×68 导航图。该版本已据实机反馈刷写成功。

BASA0 230928 R3 为三主题补图版本，标准 68×68、BM 80×80、MI 82×82。用户曾反馈补图显示效果正常；不表示这份程序适用于 FB0。

成品的字节数、MD5 和 SHA-256 见[清单](../releases/manifest.json)。在 Windows PowerShell 运行 `Get-FileHash -LiteralPath '你的固件文件.img' -Algorithm SHA256` 可核对下载文件。自行构建的 U6A 镜像可能因工具链产生不同哈希，按[构建与验证说明](build.md#u6a-r2c)核对。
