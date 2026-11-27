# GitHub 发布步骤

仓库名建议 CarPlay-Sinjet-HUD。这个目录就是仓库根目录，旁边 release-assets 是成品候选附件，不能把整个父目录上传为源码。

1. 本地运行 `python tools/audit_repo.py`，确认没有私人路径、站点、真实抓包或密钥。
2. 核对 LICENSE 与 THIRD_PARTY_NOTICES；自编代码与新图的许可不改变原厂程序或第三方图像的权利。
3. 把这个目录创建为 Git 仓库，使用自己的 GitHub 账号发布。
4. 依 releases/manifest.json 为每个成品注明项目号或 HUD 模块号。U6A 当前发布 R2c。
5. 确认固件的再分发范围后，把可刷写成品与 SHA256SUMS 放入 Release。
6. 单独发布原厂 HUD 存档，按车型和模块号归档。校验异常的样本单独打包，并标记为仅归档。
7. 公开后点击 README 中的教程和下载，完整下载成品并核对 SHA-256。

建议开启 Issues 收集反馈。问题模板要求型号、版本、复现步骤及已脱敏证据；不要把用户提交的行驶日志直接复制进测试目录。

需要随推送运行公开内容检查时，可将 [GitHub Actions 示例](../examples/github-actions/source-check.yml)复制到仓库的 `.github/workflows/source-check.yml`。通过 GitHub CLI 提交工作流文件时，授权需要包含 `workflow` 权限。

## 上传脚本

也可以在本机使用 GitHub CLI：先从 https://cli.github.com 安装，执行 `gh auth login` 登录自己的账号，然后运行 `pwsh -File tools/publish.ps1`。脚本会先检查公开文件，再读取实际登录账号，创建全新的 Public 仓库并推送源码。遇到同名仓库或已有远端时停止，不会覆盖它们。

此脚本不会自动上传旁边的完整固件候选附件，也不会读取应用内部的授权凭据。应用内 GitHub 连接与本机 CLI 登录是两个独立入口。
