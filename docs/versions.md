# 版本与下载选择

先在设备上核对项目号或 HUD 模块号，再选择对应版本。

| 设备标识 | 版本说明 | 刷写文件 | 公开构建入口 |
| :--- | :--- | :--- | :--- |
| `ly8952` · `26052617.8952.1` | [**U6A R2c，当前主版本**](../releases/u6a-8952-r2c.md) | [下载 IMG](https://github.com/Ech35/CarPlay-Sinjet-HUD/releases/download/v0.1.1/8952_26052617.8952.1_HUD_R2c.img) | [从同版本底包构建](build.md#u6a-r2c) |
| `ly8952` · `26052617.8952.1` | [U6A R2a，回退包](../releases/u6a-8952-r2a.md) | [下载 IMG](https://github.com/Ech35/CarPlay-Sinjet-HUD/releases/download/v0.1.1/8952_26052617.8952.1_HUD_R2a.img) | 历史版 |
| `ly2734` · `26051317.2734.2` | [U5 R18](../releases/u5-2734-r18.md) | [下载 IMG](https://github.com/Ech35/CarPlay-Sinjet-HUD/releases/download/v0.1.1/2734_26051317.2734.2_HUD_R18.img) | [组件构建](build.md#u5) |
| `HUDBAFB0` | [FB0 201013 R1](../releases/hud-bafb0-201013-r1.md) | [下载 PAK](https://github.com/Ech35/CarPlay-Sinjet-HUD/releases/download/v0.1.1/mcu_HUDBAFB0_201013.pak) | [从同型号底包构建](build.md#fb0-专用固件) |
| `HUDBASA0` | [SA0 230928 R3](../releases/hud-basa0-230928-r3.md) | [下载 PAK](https://github.com/Ech35/CarPlay-Sinjet-HUD/releases/download/v0.1.1/mcu_HUDBASA0.pak) | [图标资源与适配说明](hud-upgrade.md) |

上表是改装成品。用于资源适配的原厂 HUD 底包见[信界 HUD 原厂固件存档](https://github.com/Ech35/CarPlay-Sinjet-HUD/releases/tag/sinjet-hud-stock-20261127)。

U6A R2a 是回退与对照版本，已有短时实机导航与显示反馈。R2c 基于它修复开机读取配置过早的问题：等待 CarPlay 就绪后恢复设备和开关，配置晚到则继续重试，尊重用户手动关闭。

R2c 的 HUD 自动连接已据实机反馈验证通过。离线检查覆盖后台启动、ARM 接口、蓝牙和导航转发。R2b 已停用。

FB0 补图基于同型号 201012，内部版本为 20201013，增加 40–85 共 46 个槽位。四个仪表主题共用一套 68×68 导航图。该版本已据实机反馈刷写成功。

BASA0 230928 R3 为三主题补图版本，标准 68×68、BM 80×80、MI 82×82。用户曾反馈补图显示效果正常；不表示这份程序适用于 FB0。

成品的字节数、MD5 和 SHA-256 见[清单](../releases/manifest.json)。在 Windows PowerShell 运行 `Get-FileHash -LiteralPath '你的固件文件.img' -Algorithm SHA256` 可核对下载文件。自行构建的 U6A 镜像可能因工具链产生不同哈希，按[构建与验证说明](build.md#u6a-r2c)核对。
