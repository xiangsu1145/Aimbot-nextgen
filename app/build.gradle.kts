import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
}

// ── Signing: credentials are NEVER hardcoded ──────────────────────────
// The repo is public (AGPLv3). The real keystore (sign/*.jks) and the real
// passwords are gitignored and live only on your machine / CI.
// Priority: local.properties (local dev) > env vars (CI) > gradle properties.
//   local.properties keys: signing.storeFile, signing.storePassword,
//                          signing.keyAlias, signing.keyPassword
//   env vars: AIMBOT_SIGNING_STORE_FILE, AIMBOT_SIGNING_STORE_PASSWORD,
//             AIMBOT_SIGNING_KEY_ALIAS, AIMBOT_SIGNING_KEY_PASSWORD
val localSigningProps = Properties().apply {
    val f = rootProject.file("local.properties")
    if (f.exists()) f.inputStream().use { load(it) }
}
fun signingProp(key: String, envName: String): String? =
    (localSigningProps.getProperty(key) as? String)?.takeIf { it.isNotBlank() }
        ?: System.getenv(envName)?.takeIf { it.isNotBlank() }
        ?: (project.findProperty(key) as? String)?.takeIf { it.isNotBlank() }

val signingStoreFilePath =
    signingProp("signing.storeFile", "AIMBOT_SIGNING_STORE_FILE") ?: "sign/aimbot.jks"
val signingStoreFile = rootProject.file(signingStoreFilePath)
val signingStorePassword = signingProp("signing.storePassword", "AIMBOT_SIGNING_STORE_PASSWORD")
val signingKeyAlias = signingProp("signing.keyAlias", "AIMBOT_SIGNING_KEY_ALIAS")
val signingKeyPassword = signingProp("signing.keyPassword", "AIMBOT_SIGNING_KEY_PASSWORD")
// Only wire the custom key when everything is present. A fresh clone
// (no keystore, no passwords) still builds: debug falls back to the
// default debug key, release stays unsigned with a warning.
val hasCustomSigning = signingStoreFile.exists()
    && !signingStorePassword.isNullOrBlank()
    && !signingKeyAlias.isNullOrBlank()
    && !signingKeyPassword.isNullOrBlank()
if (!hasCustomSigning) {
    logger.warn(
        "[signing] custom keystore not configured (missing $signingStoreFilePath " +
            "or signing.* passwords). Using default debug key; " +
            "see sign/README.md to set it up."
    )
}

android {
    namespace = "io.github.xiangsu1145.aimbotnextgen"
    compileSdk = 36

    defaultConfig {
        applicationId = "io.github.xiangsu1145.aimbotnextgen"
        minSdk = 31
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"

        ndk {
            // arm64-v8a only — the current target device (OnePlus) is
            // arm64-only, and the TFLite runtime .so shipped under
            // app/src/main/jniLibs/ is also arm64-only. armv7 is kept off
            // so a missing .so does not break the build.
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++17", "-fexceptions")
                val haveNeuropilot = (project.findProperty("aimbot.haveNeuropilot") as String?)?.toBoolean() ?: true
                // Private Neuron APU backend gate. See gradle.properties.
                val haveNeuron = (project.findProperty("aimbot.haveNeuron") as String?)?.toBoolean() ?: true
                arguments += listOf(
                    // Static C++ runtime: the privileged shell daemon dlopen()s
                    // libaimbotng.so by absolute path, where the linker would
                    // otherwise resolve DT_NEEDED "libc++_shared.so" against the
                    // platform one (a different, possibly older ABI). Linking it
                    // in removes that dependency entirely.
                    "-DANDROID_STL=c++_static",
                    "-DANDROID_TOOLCHAIN=clang",
                    // Closed-source backend gate. See gradle.properties.
                    "-DAIMBOTNG_HAVE_NEUROPILOT=${if (haveNeuropilot) "ON" else "OFF"}",
                    // Private Neuron APU delegate gate. See gradle.properties.
                    "-DAIMBOTNG_HAVE_NEURON=${if (haveNeuron) "ON" else "OFF"}"
                )
            }
        }

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    signingConfigs {
        if (hasCustomSigning) {
            create("aimbot") {
                storeFile = signingStoreFile
                storePassword = signingStorePassword
                keyAlias = signingKeyAlias
                keyPassword = signingKeyPassword
            }
        }
    }
    buildTypes {
        debug {
            if (hasCustomSigning) signingConfig = signingConfigs.getByName("aimbot")
        }
        release {
            if (hasCustomSigning) signingConfig = signingConfigs.getByName("aimbot")
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    buildFeatures {
        prefab = true
    }
    // Extract the .so files into the app's nativeLibraryDir on install.
    // The privileged shell daemon (ShellServerEntry, started via app_process)
    // has no APK context, so it can only load libaimbotng.so by absolute path —
    // which requires the library to exist on disk rather than only inside the APK.
    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }
    kotlinOptions {
        jvmTarget = "11"
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
    ndkVersion = "29.0.14206865"
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.activity.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.material)
    implementation(libs.androidx.fragment.ktx)
    implementation(libs.androidx.recyclerview)
    implementation(libs.androidx.constraintlayout)
    implementation("org.bouncycastle:bcpkix-jdk15on:1.70")
    implementation("io.github.vvb2060.ndk:boringssl:20250114")
    implementation("org.conscrypt:conscrypt-android:2.5.2")
    // libonnxruntime.so for every ABI, extracted into the APK's lib dir.
    // The shell daemon dlopen()s it by absolute path from there — see
    // cpp/inference/libpath.h for why a bare name would never resolve.
    implementation(libs.onnxruntime.android)
    // libtensorflowlite_jni.so is shipped under app/src/main/jniLibs/<abi>/,
    // NOT as a Maven AAR. AGP picks it up automatically (alongside any other
    // .so placed there) and packages it into the APK's nativeLibraryDir.
    // CMake links it directly into libaimbotng.so — see CMakeLists.txt.
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
}