# 源码索引

为保留已验证的相对 include，源码快照保留原有目录名，而不是机械移动到新的 core/platforms。

- `ble-r18/core/`：GPL-3.0-or-later 的导航状态、车道排序、兼容/完整图标转换。
- `ble-r18/`：U5 BLE 服务和公共适配层；`test_adapter.py` 只包含合成数据。
- `iap_patch/`、`u5hud-runtime-r18/`、`u5hud-web-r18/`：U5 原厂库叠加、诊断与管理页。
- `u6a-8952-r2c/`：本次 U6A 主版本；修复晚挂载配置恢复，保留 R2a 的进程身份校验。
- `hud-bafb0-r1/`：FB0 专用补丁、构包和原厂 Thumb 路径仿真验证。
- `hud-icons-r3/icon_ids.h`：扩展蓝牙编号 40–85，区别于 HUD 内部逻辑资源编号。

原厂可执行程序、历史固件、私有测试抓包不在此目录。构建时由用户提供对应指纹的输入。

适配新盒子时从[跨品牌教程](../docs/box-porting.md)及[设备记录模板](../examples/box-porting/device-profile.md)开始。路先行 CPBox 镜像可以使用[只读检查脚本](../tools/inspect_cpbox.py)识别项目号。
