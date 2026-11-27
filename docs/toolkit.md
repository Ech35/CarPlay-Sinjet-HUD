# 构建工具容器

工具容器提供 Linux x86‑64 上的导航核心回放测试和 FB0 补图构建环境。日常使用请先看[设备版本和教程](versions.md)。

## 运行导航核心测试

```sh
docker run --rm ghcr.io/ech35/carplay-sinjet-hud-toolkit:v0.1.0
```

程序使用合成导航消息检查动作、距离、车道和图标转换。

## 构建 FB0 补图包

在已克隆的仓库根目录操作。先按[构建说明](build.md#fb0-专用固件)放入匹配的 FB0 原厂底包，再运行以下 PowerShell 命令。

```powershell
docker run --rm --mount "type=bind,src=$((Get-Location).Path),dst=/opt/carplay-hud" -w /opt/carplay-hud ghcr.io/ech35/carplay-sinjet-hud-toolkit:v0.1.0 python -B src/hud-bafb0-r1/build.py
```

构建结果写入 `src/hud-bafb0-r1/candidate.pak`。U5 和 U6A 盒子镜像使用各自的[构建步骤](build.md)。
