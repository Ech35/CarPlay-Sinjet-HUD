# 自建下载站与 QX

升级查询返回 JSON，JSON 的 `url` 再指向固件。两者是两次不同请求。示例见 `examples/upgrade.json`，实际发布前替换为你自己控制的域名、文件和校验值。

配置至少包含 vcode、vname、date、changed、url、md5、invalid_code、force。FB0 本次 vcode 为 20201013；MD5 必须计算实际下载的整份 PAK，不能拿某个内部记录的 MD5 代替。

配置页返回 HTTP 200 和 application/json；固件返回可下载的二进制内容。发布后完整下载一次，核对大小与 SHA-256。优先采用包含型号与版本的固定文件路径，避免同名缓存指向旧包。

QX 示例仅使用 example.invalid。按自己设备实际请求填写来源地址。正则中的 `(?:_[^&]+)?` 接受密码和 Bootloader 等下划线后缀，`(?:&.*)?` 接受后续参数；是否应将两种查询导向同一配置，需结合客户端流程验证，不能仅凭名称认定。

用 307 重定向时，规则命中不等于客户端已取得 JSON。核对目标路径完整，例如 `/firmware/upgrade.json` 与 `/firmware/upgrade` 可能是完全不同的路由。清掉同条件、不同目标的重复规则。

以往 iOS 案例停在 Request To Server!，截图发现错误路径与重复规则，修正后的实际下载结果未确认。Android 下载代码的行为只能作为参考，不能当作 iOS 内部实现的证据。
