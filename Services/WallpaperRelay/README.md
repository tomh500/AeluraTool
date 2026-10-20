# Wallpaper Relay

Private Linux backend for a Windows wallpaper client. This initial version provides an OIDC browser login flow, an explicit identity allowlist, opaque one-hour sessions, a durable SQLite job queue, one SteamCMD worker, cancellation, and owner-checked ZIP downloads with SHA-256 and HTTP Range support. It only accepts Wallpaper Engine app 431960. API responses use stable `relay.*` error keys; clients translate them using the application's string resources.

## Local development

Use Python 3.11 or newer. From this directory:

```powershell
python -m venv .venv
.venv/Scripts/python -m pip install -r requirements-test.txt
.venv/Scripts/python -m pytest -q
Copy-Item config.example.json config.json
.venv/Scripts/python -m uvicorn relay.app:create_app --factory --host 127.0.0.1 --port 18741 --workers 1 --no-access-log
```

On Linux use `.venv/bin/python`. Keep exactly one worker process: the queue and Steam account session are intentionally serialized. `config.json`, identity sessions, Steam credentials, content and logs are private runtime state and must never enter Git. Set directory permissions to 0700 and use umask 0077. Server configuration is read at startup; restart after permission changes.

## Identity setup

For the first Google integration over an SSH tunnel, follow [Google 登录联调](GOOGLE_SETUP.md).

1. Register a Google OAuth application and set its client ID and secret in the `google` provider. Register exactly `PUBLIC_URL/v1/auth/callback` as a redirect URI. The public URL must be HTTPS before using a public reverse proxy. For an SSH-only experiment Google may support a registered localhost callback; do not expose plain HTTP on a public interface.
2. Leave `allowlist` empty initially. A successful login returns `denied` together with the verified `google:SUBJECT` identity to the client that owns the login ticket. Add that identity to the allowlist and restart; login again to obtain a session. Do not authorize by a user-supplied email address or QQ number.
3. For Microsoft, register a separate `microsoft` provider using an appropriate tenant-specific discovery URL, client ID and secret. Discovery must return a concrete fixed issuer, not the `{tenantid}` template used by `common`/`organizations`. The generic adapter validates RS256 signature, issuer, audience, expiry, issue time, nonce and authorized party. Microsoft real-account integration has not been verified. QQ is not implemented and cannot be authenticated by merely providing a QQ number.

The Windows client uses this flow:

```text
POST /v1/auth/start/google
  -> authorization_url, ticket, poll_secret, expires_in
Open authorization_url in the system browser.
Poll POST /v1/auth/poll/{ticket}
  Authorization: Bearer {poll_secret}
  -> pending / processing / denied / failed / approved
approved -> access_token (returned once; store in Windows Credential Manager)
GET /v1/me
  Authorization: Bearer {access_token}
```

Polling is limited to 60 requests per minute per direct source IP across auth and job-creation endpoints; poll every 3 seconds. Login tickets expire in ten minutes, sessions one hour; login again after expiry. The service checks both the session and current allowlist on each protected request. The callback state is single-use and the authorization code is exchanged using PKCE S256. The browser never receives the relay access token. Do not put bearer tokens or poll secrets in URLs. Access logging is disabled to avoid recording OAuth callback codes.

## Steam setup

Install Valve's unmodified SteamCMD into the dedicated configured `steam_root`. It may need Ubuntu's 32-bit runtime libraries. Run it interactively with the same OS account, working directory and HOME as the service:

```bash
cd /home/ubuntu/wallpaper-relay/steam
HOME=/home/ubuntu/wallpaper-relay/steam-home ./steamcmd.sh
# At the Steam prompt, enter login ACCOUNT, then password and Steam Guard interactively.
# Then test: workshop_download_item 431960 ITEM_ID validate
# Then: quit
```

Use an account with the required application entitlement. Never put its password or Steam Guard secret into this repository or pass it through client APIs. SteamCMD's cached login must permit later noninteractive `+login ACCOUNT`; otherwise jobs fail or time out and manual login is needed again. Set `steam_account` and enable `downloads_enabled` only after the real download works. The relay checks SteamCMD's success marker as well as its exit code and resulting files. SteamCMD behavior and Wallpaper Engine download permission are not proven by mocked tests.

## Download API

All endpoints below require `Authorization: Bearer ACCESS_TOKEN`:

| Endpoint | Meaning |
| --- | --- |
| `POST /v1/jobs` with `{"item_id":"123456789"}` | Queue a download; returns job ID and status |
| `GET /v1/jobs/{id}` | Read this user's task; ready tasks include archive size and SHA-256 |
| `DELETE /v1/jobs/{id}` | Request cancellation; Steam content is preserved |
| `GET /v1/jobs/{id}/content` | Download a ready ZIP; supports Range and X-Content-SHA256 |
| `POST /v1/auth/logout` | Revoke this session |

The client must verify SHA-256 before extracting and enforce its own safe extraction rules. The server rejects symlinks, special files, Windows reserved names, case-insensitive name collisions and excessive paths/file counts. It never executes downloaded wallpapers. No download URLs bypass identity checks. IDs belong to individual users even when the workshop item ID is identical.

## Deployment

Copy the source to `/home/ubuntu/wallpaper-relay`, create its virtualenv and private `config.json`, and install `deploy/aelura-wallpaper-relay.service` with administrator privileges. The unit binds **127.0.0.1:18741** only. Keep existing server ports/services unchanged. Use an SSH tunnel for initial Windows integration:

```powershell
ssh -6 -L 18741:127.0.0.1:18741 ubuntu@lemoe.luotiany1.top
curl.exe http://127.0.0.1:18741/healthz
```

Do not configure proxy headers or public ingress until a TLS reverse proxy and real identity application are configured. The service has a 512 MiB memory ceiling. Job creation allows two active jobs per identity and sixteen globally. Content is limited to 1 GiB per item, 6 GiB combined dedicated Steam/data directories and a 2 GiB free-space reserve by default. Storage monitoring is periodic, not a filesystem hard quota; large Steam downloads can temporarily exceed limits between checks. Set a dedicated filesystem quota for strict enforcement.

## Initial-version limitations

- No real Google/Microsoft application registration is bundled, no QQ adapter, no Windows client/player, no public TLS ingress.
- Steam login, actual Workshop downloads and Wallpaper Engine playback require separate real-account verification.
- No cache eviction, automatic update synchronization or cleanup. Existing item directories are deliberately refused to avoid serving stale/partial Steam content. Review retained content manually before retrying; do not delete unique data automatically.
- Job phases are available; Steam byte-level progress is not implemented. Steam interruption retains content and `.partial` ZIPs for recovery. Restart marks outstanding jobs `interrupted` rather than silently retrying.
- The service assumes its private directories cannot be modified by untrusted local users. The preflight archive scan is not a sandbox for hostile concurrent local filesystem writers.
- OIDC tests validate signed synthetic tokens and mocked token exchange; they do not prove real provider registration or end-to-end browser login.

Protocol references: [Google OIDC](https://developers.google.com/identity/openid-connect/openid-connect), [Microsoft OIDC](https://learn.microsoft.com/en-us/entra/identity-platform/v2-protocols-oidc), [Valve Steam Workshop](https://partner.steamgames.com/doc/features/workshop/implementation), [SteamCMD](https://developer.valvesoftware.com/wiki/SteamCMD).
