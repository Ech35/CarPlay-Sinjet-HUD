# 构建与本机验证

Python 3.11+；安装 `pip install -r requirements.txt`。原厂输入全部放在被 Git 忽略的 inputs/，仓库不自动下载原厂固件。

## 公共核心

Windows 下准备 Zig 0.13.0，执行：

```powershell
pwsh -File src/ble-r18/test.ps1 -Zig C:/tools/zig/zig.exe -Python python
```

覆盖当前动作与未来动作、文字优先级、距离有效性、推荐车道、图标编号、兼容模式及清屏；不连接无线设备。

## FB0 专用固件

放入 `inputs/mcu_HUDBAFB0_201012.pak`，SHA-256 必须为 `eb6b541a6a6050349f64d1cdb7b52459bf5d810c4e214ccc1b7777f63788f9dc`。执行：

```powershell
python -B src/hud-bafb0-r1/build.py
```

输出 `src/hud-bafb0-r1/candidate.pak` 和验证记录，这些构建产物不会进入 Git。新图来自 assets，复用图从原厂输入读取；无需作者桌面目录、审核表或 BASA0 完整固件作图像供体。

## U6A R2c

放入 `inputs/u6a-stock.img`，底包必须为 8952 / 26052617.8952.1。先运行 `python tools/extract_stock.py u6a` 提取本机输入；使用 Zig 0.13.0，运行 `pwsh -File src/u6a-8952-r2c/build.ps1 -Zig C:/tools/zig/zig.exe -Python python`。公开构建入口使用 Zig 的标准 musl 链接方式，不需要作者本机缓存路径；生成文件可能与原发布版的链接布局不同，应独立验证后使用。

构建脚本输出自己的 ELF 载荷、诊断 CGI 和 HUD 后台；pack.py 只在本机叠加到指纹一致的原厂程序。完整镜像执行 `python tools/assemble_u6a.py`，需另安装 sqfstar，并将完整路径设置到环境变量 SQFSTAR。构包保留项目描述、内核、原始客户分区长度与元数据，并核对 CPIO 和外层 MD5。

构包时会从同一个原厂底包读取管理页，应用 `src/u6a-8952-r2c/management-page.patch.json` 并核对重建页面的 SHA-256。

重新编译后可执行 `python src/u6a-8952-r2c/test_api.py` 和 `python src/u6a-8952-r2c/test_worker_boot.py`，检查 30 项 ARM 接口和七项当前后台启动场景。它们使用合成文件系统与蓝牙尝试，不需要历史版本或真实抓包。

## U5

已收录 R18 核心、BLE、原厂库钩子、诊断与管理页。iap_patch 需要从用户提供的对应原厂镜像提取 customer/app/libiap_new.so，执行其 build.ps1；runtime 与 BLE 各有构建入口。完整镜像集成需按原厂文件树执行 repack_u5.py 并核对本机输出，不宣称只凭本文已经跨工具链复现交付镜像。该部分的公开复现说明仍需完善，初次使用优先选择对应成品。

不同版本的原厂 ELF/PAK 不可通过跳过断言强行构建。构建成功仍需要实机检查。
