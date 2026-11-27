param([string]$RepositoryName='CarPlay-Sinjet-HUD')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
if(-not(Get-Command gh -ErrorAction SilentlyContinue)){
  throw '需要 GitHub CLI：从 https://cli.github.com 安装，然后使用 gh auth login 登录。应用内的 GitHub 授权不等于本机 CLI 已登录。'
}
& gh auth status
if($LASTEXITCODE){throw '本机 GitHub CLI 尚未登录，请先执行 gh auth login。无需提供令牌给项目作者。'}
$identity=(& gh api user | ConvertFrom-Json)
if($LASTEXITCODE -or -not $identity.login){throw '无法读取当前 GitHub 账号'}
$destination="$($identity.login)/$RepositoryName"
Write-Host "发布目标：$destination（Public）"
$python=Get-Command python -ErrorAction Stop
& $python.Source -B (Join-Path $PSScriptRoot 'audit_repo.py')
if($LASTEXITCODE){throw '公开内容检查未通过，未上传'}
if(-not(Test-Path -LiteralPath (Join-Path $root '.git'))){
  & git -C $root init -b main
  if($LASTEXITCODE){throw '初始化本地仓库失败'}
}
$existing=& git -C $root remote get-url origin 2>$null
if($LASTEXITCODE -eq 0){throw '此目录已有远端，请先核对目标；脚本不会替换已有远端。'}
& gh repo view $destination --json name 2>$null | Out-Null
if($LASTEXITCODE -eq 0){throw '同名远端已存在，脚本不会覆盖它；请用 -RepositoryName 指定新名称。'}
& git -C $root config user.name $identity.login
& git -C $root config user.email "$($identity.id)+$($identity.login)@users.noreply.github.com"
& git -C $root config core.autocrlf false
& git -C $root add --all
if($LASTEXITCODE){throw '暂存源码失败'}
& git -C $root diff --cached --quiet
if($LASTEXITCODE -eq 1){
  & git -C $root commit -m 'Prepare public CarPlay to Sinjet HUD source and guides'
  if($LASTEXITCODE){throw '本地提交失败'}
}
& gh auth setup-git
if($LASTEXITCODE){throw '配置 GitHub CLI 的 Git 授权失败'}
Push-Location -LiteralPath $root
try{
  & gh repo create $destination --public --description 'CarPlay navigation to Sinjet HUD: U5/U6A bridge, diagnostics, icon resources and Chinese guides' --source . --remote origin --push
  if($LASTEXITCODE){throw 'GitHub 发布失败，请保留返回信息'}
  & gh repo view $destination --json url --jq .url
}finally{Pop-Location}
# 仅发布公开源码；完整厂商固件候选附件不会被本脚本自动上传。
