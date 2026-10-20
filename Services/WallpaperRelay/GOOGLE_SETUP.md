# Google 登录联调

当前服务只监听服务器的 127.0.0.1:18741；先使用 SSH 隧道完成登录，不需要开放公网 HTTP 端口。

1. 在 [Google Cloud Console](https://console.cloud.google.com/) 创建或选择自己的项目，进入 Google Auth Platform，配置应用名称、联系邮箱和受众。使用测试模式时，将你和参与联调的朋友加入测试用户。
2. 创建 OAuth 客户端，类型选 **Web application**。虽然最终产品是 Windows 软件，这里的授权码由 Linux 后端交换，因此这一配置对应服务端。
3. 添加授权重定向 URI，必须完全一致：`http://localhost:18741/v1/auth/callback`。只请求 `openid`，当前服务不读取 Google 邮件或网盘。
4. 在服务器 `/home/ubuntu/wallpaper-relay/config.json` 的 `providers.google` 下填写 `client_id` 和 `client_secret`。不要把密钥放进客户端、Git 或聊天记录。将 `public_url` 设为 `http://localhost:18741`，然后执行 `sudo systemctl restart aelura-wallpaper-relay`。
5. Windows 保持 SSH 隧道运行：

```powershell
ssh -6 -L 18741:127.0.0.1:18741 ubuntu@lemoe.luotiany1.top
```

6. Windows 另开 PowerShell 请求登录并打开系统浏览器：

```powershell
$login = Invoke-RestMethod -Method Post http://localhost:18741/v1/auth/start/google
Start-Process $login.authorization_url
# 浏览器完成登录后执行；未完成可隔 3 秒重试。
$result = Invoke-RestMethod -Method Post `
  -Uri "http://localhost:18741/v1/auth/poll/$($login.ticket)" `
  -Headers @{ Authorization = "Bearer $($login.poll_secret)" }
$result | Select-Object status, identity
```

7. 第一次登录应得到 `denied` 和已验证的 `google:SUBJECT`。把这个完整身份字符串加入服务器配置的 `allowlist` 数组，重启服务后重新登录。Google 测试用户名单只控制能否登录该 OAuth 应用；本服务允许名单另行控制能否使用下载服务。
8. 允许后，`$result.access_token` 为一小时有效的会话令牌，只返回一次。调用接口时放在 Authorization 请求头中，不要打印或拼入 URL。Win32 正式客户端应存入 Windows 凭据管理器。

```powershell
$authHeaders = @{ Authorization = "Bearer $($result.access_token)" }
Invoke-RestMethod http://localhost:18741/v1/me -Headers $authHeaders
```

Google 登录成功仍不代表 Steam 下载已接通。需要在服务器专用 HOME 下手动登录 SteamCMD，完成 Steam Guard，验证物品下载，再设置 `steam_account` 和 `downloads_enabled`。当前下载开关默认关闭。

如果之后让朋友不经 SSH 直接访问，应先配置 HTTPS 域名及反向代理，再同步修改 `public_url` 和 Google 重定向 URI；每个人的正式客户端都应走同一个已部署的回调地址。当前版本不包含该公网部署。

参考：[Google OpenID Connect 与重定向 URI 配置](https://developers.google.com/identity/openid-connect/openid-connect)。
