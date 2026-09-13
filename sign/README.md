# 签名配置说明（可提交，无秘密）

本仓库是公开仓库（AGPLv3），**签名文件和密码一律不进仓库**。

被 `.gitignore` 排除的内容：

- `sign/*.jks` / `*.keystore` / `*.p12` / `*.pfx` —— 私钥文件
- `sign/签名说明.txt` —— 含真实密码的本地记录
- `local.properties` —— 含真实密码的本地配置

## 本地开发（Windows 示例）

1. 把你的 keystore 放到 `sign/` 下（例如 `sign/aimbot.jks`）。
2. 在项目根目录的 `local.properties`（已在 `.gitignore` 中，不会提交）里加：
   ```properties
   signing.storeFile=sign/aimbot.jks
   signing.storePassword=<keystore 密码>
   signing.keyAlias=<别名>
   signing.keyPassword=<别名密码>
   ```
3. 直接构建即可，debug / release 都会用该签名：
   ```bash
   ./gradlew assembleDebug
   ```

## CI / 换机器

用环境变量代替 `local.properties`（两者任选其一，`local.properties` 优先）：

- `AIMBOT_SIGNING_STORE_FILE`
- `AIMBOT_SIGNING_STORE_PASSWORD`
- `AIMBOT_SIGNING_KEY_ALIAS`
- `AIMBOT_SIGNING_KEY_PASSWORD`

## 没有配置签名时会怎样

- 不报错：debug 自动回退到默认 debug key，release 为未签名包并打印 warning。
- 所以新 clone 下来不配签名也能编译跑起来，只是签名指纹不同。

## 生成新签名（仅在需要换 key 时）

```bash
keytool -genkeypair -v -keystore sign/aimbot.jks -storetype JKS \
  -alias <别名> -keyalg RSA -keysize 2048 -validity 36500 \
  -storepass <密码> -keypass <别名密码> \
  -dname "CN=Aimbot, OU=AimbotNextgen, O=Aimbot, L=Beijing, ST=Beijing, C=CN"
```

密码只记在你本机的 `local.properties` 和你自己的密码管理器里，不要写进任何会被提交的文件。
