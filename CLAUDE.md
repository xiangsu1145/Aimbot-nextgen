# AGENTS.md

## Project

AimbotNextgen — Android aimbot using YOLO AI recognition + uinput touch injection. System-level ImGui overlay via shell permissions (Shizuku-like model, not root). Successor to Aimbot-ai (`G:\ai\Aimbot-ai\android-client`).

**Status: Empty template.** Fresh Android Studio Compose project. Everything needs to be built from scratch.

## Stack

- AGP 8.10.0, Kotlin 2.0.21, Compose BOM 2024.09.00
- compileSdk/targetSdk 35, minSdk 31 (Android 12+)
- Single module: `app/`
- Version catalog: `gradle/libs.versions.toml`
- **No NDK/CMake yet** — must be added for ImGui (C++)

## Architecture (planned)

- **Shell Server**: Runs via `adb shell app_process` with shell UID (2000). Grants `SYSTEM_ALERT_WINDOW` and other permissions to the app via `appops set`/`pm grant`. Similar to Shizuku.
- **Overlay Service**: Foreground service, `WindowManager.addView(SurfaceView)` with `TYPE_APPLICATION_OVERLAY`.
- **Native Rendering (JNI/NDK)**: EGL + OpenGL ES 3.0. ImGui with `imgui_impl_android` + `imgui_impl_opengl3`.
- **Input**: Touch from `/dev/input/eventX` (real physical via uinput injection later).

## Build Commands

```bash
# Build debug APK
./gradlew assembleDebug

# Install on device
./gradlew installDebug

# Clean build
./gradlew clean assembleDebug
```

## Build workflow convention

After any code change that should land on the test device, the agent should
run `./gradlew :app:installDebug` rather than just `assembleDebug` — `installDebug`
builds **and** pushes the APK to the connected device (`OPD2404`) in one step,
so the user can immediately exercise the new build without manually invoking
`adb install`. Only fall back to `assembleDebug` when the user explicitly wants
just the APK artifact (e.g. to copy it elsewhere) or when no device is wired up.

No NDK build yet. When CMake is added: `./gradlew assembleDebug` handles native builds automatically.

## Key Paths

```
app/src/main/java/io/github/xiangsu1145/aimbotnextgen/
├── MainActivity.kt          # Entry point (currently just "Hello Android!")
└── ui/theme/                 # Compose theme (default template)

app/src/main/cpp/             # (to create) ImGui native code
app/src/main/AndroidManifest.xml  # Minimal, no permissions declared yet
```

## Conventions

- Kotlin, Java 11 target
- Package: `io.github.xiangsu1145.aimbotnextgen`
- No existing code style beyond Kotlin official (`kotlin.code.style=official` in gradle.properties)
- No tests beyond template boilerplate
