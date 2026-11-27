# 来源与许可

本仓库自编的适配、构建与诊断代码，以及项目生成的新图资源，按 GPL-3.0-or-later 提供；具体文件已有其他声明时以其声明为准。原厂程序、原厂图像、Apple 文档和第三方工具不因本仓库的 LICENSE 改变许可。

## 导航解析器

`src/ble-r18/core/vendor/rgd` 来自 [luka-dev/mib2q-carplay-rgi](https://github.com/luka-dev/mib2q-carplay-rgi/tree/a63d54741839c7d30d32dd4e149f693856bcb0db)，固定提交 a63d54741839c7d30d32dd4e149f693856bcb0db，原路径 hook/routeguidance，Copyright (c) 2026 LuKa (@LuKa_dev)。原版权头和 GPL 许可证保留；本地改动包括车道完整性、可选角度 presence、增量处理与显示适配。vendor/framework 为本地兼容层。

## 刷写工具

完整的研究参考、行为对照和工具致谢见 [参考项目与致谢](docs/references.md)，其中仅“实际复用”表中的项目表示本仓库收录了对应代码；其余条目仅说明研究来源。

[Srysfu/Luxianxing-Firmware-Tool](https://github.com/Srysfu/Luxianxing-Firmware-Tool) 提供原有刷写方式参考。本仓库不把该工具源码当作自己编写，也不重新打包其 APK；请从原项目取得并遵守其许可。

## HUD 资源

assets/hud-icons 的 131 张新图来自项目经审核后的图像设计与导出，使用 GPT 图像生成参与设计。清单中七个 copied_stock 槽位不携带原厂 PNG，而在本机从对应固件提取。FB0 原厂导航图片、原厂全套图标和其他厂商图库不纳入开源许可。

## 外部依赖与原厂输入

U6A 管理页在本地构建时由用户提供的原厂页面与本项目差分合成。

Zig、Python、Pillow、Capstone、Unicorn、Keystone、pyelftools、PySquashfsImage 和 squashfs-tools 均由各自项目提供，未将其完整运行时打入仓库。

完整盒子镜像包含厂商 CarPlay/iAP2 组件和其他程序；完整 HUD PAK 也包含原厂程序与图像。公开附件前应核对再分发许可，不能把它们整体标作本项目 GPL 原创。MFi 技术规范原文、IPA/APK、私有服务器资料、原始抓包和密钥均不在公开源码目录。
