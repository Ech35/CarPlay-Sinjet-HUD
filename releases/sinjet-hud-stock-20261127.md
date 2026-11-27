# 信界 HUD 原厂固件存档 · 34 份

从现有原厂固件档案整理，按车型与模块号收录。每份均保留原始文件，下载后可用包内 `SHA256SUMS.txt` 对照校验。

| 车型 | 模块号 | 通过记录校验 | 仅归档 |
| :--- | :--- | ---: | ---: |
| 昂克塞拉 | `HUDBACA0` | 6 | 1 |
| 昂克塞拉改款 HUD | `HUDBASA0` | 0 | 1 |
| CX-4 | `HUDBAEB0` | 12 | 1 |
| CX-4 | `HUDBAFB0` | 3 | 0 |
| 阿特兹 | `HUDBAEC0` | 6 | 0 |
| 阿特兹 | `HUDBAFC0` | 3 | 0 |
| 车型待确认 | `HUDB6000` | 1 | 0 |

- [原厂主档 ZIP · 31 份](https://github.com/Ech35/CarPlay-Sinjet-HUD/releases/download/sinjet-hud-stock-20261127/Sinjet-HUD-Stock-31.zip) 包含识别记录的头部和 MD5 均通过的样本。
- [仅归档 ZIP · 3 份](https://github.com/Ech35/CarPlay-Sinjet-HUD/releases/download/sinjet-hud-stock-20261127/Sinjet-HUD-Stock-Archive-Only-3.zip) 包含 `HUDBACA0 210826`、`HUDBASA0 221019`、`HUDBAEB0 210829`。这些样本的开机动画容器及一张图片存在记录 MD5 不匹配，请勿刷写，可用于解包研究。
- [车型、模块与版本索引](https://github.com/Ech35/CarPlay-Sinjet-HUD/releases/download/sinjet-hud-stock-20261127/Sinjet-HUD-Stock-Index.csv) 记录每个包的字节数、内部版本和哈希。

车型用于查找，匹配固件时以 HUD 的完整模块号为准。`HUDB6000` 的包内版本为 `20180911`，车型尚未确认。记录校验结果反映文件结构与完整性，刷写兼容性还取决于设备与升级方式。

原厂包用于存档与型号适配。[CarPlay 转发成品固件](https://github.com/Ech35/CarPlay-Sinjet-HUD/releases/tag/v0.1.1)另有发布页。
