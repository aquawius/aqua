import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

// App 版本号单一来源：仓库根 CMakeLists.txt 的 AQUA_VERSION 字面量
//（AQUA_ANDROID_VERSION/AQUA_ANDROID_VERSION_CODE 均由其派生；Android 版本不生成
// C++ 头，Gradle 直接读版本字面量并按同一算法派生 versionCode）。
// rootProject = aqua_android；其上两级（aqua_android/../..）是仓库根。
val rootCmakeText = rootProject.projectDir.parentFile.parentFile
    .resolve("CMakeLists.txt").readText()
val aquaAndroidVersion: String =
    Regex("""set\(AQUA_VERSION\s+"(\d+\.\d+\.\d+)"\)""")
        .find(rootCmakeText)
        ?.groupValues?.get(1)
        ?: error("AQUA_VERSION literal not found in root CMakeLists.txt")
// versionCode：major*1_000_000 + minor*1_000 + patch（与根 CMake 派生算法一致；
// CMake 侧是 math(EXPR) 非字面量，正则不可读，故此处同规则重算，单一源仍是版本号）。
val aquaAndroidVersionCode: Int =
    aquaAndroidVersion.split('.').let { parts ->
        require(parts.size == 3) {
            "AQUA_ANDROID_VERSION '$aquaAndroidVersion' is not major.minor.patch"
        }
        val (maj, min, pat) = parts.map { it.toIntOrNull() ?: error("non-numeric '$it'") }
        require(min < 1000 && pat < 1000) { "minor/patch must be < 1000" }
        maj * 1_000_000 + min * 1_000 + pat
    }

// Release 签名：从 aqua_android/keystore.properties 读取（该文件不入 git）。
// 无该文件时回退到 debug 签名，保证 assembleRelease 在开发机上仍可用。
val keystoreProperties = Properties().apply {
    val f = rootProject.file("keystore.properties")
    if (f.exists()) f.inputStream().use { load(it) }
}
val hasReleaseKeystore = keystoreProperties.isNotEmpty()

android {
    namespace = "com.aquawius.aqua"

    compileSdk {
        version = release(37)
    }

    defaultConfig {
        applicationId = "com.aquawius.aqua"
        // AAudio 在 API 26/27 有已知断流问题，业界事实标准 28 起可用。
        minSdk = 28
        targetSdk = 37
        versionCode = aquaAndroidVersionCode
        versionName = aquaAndroidVersion

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    signingConfigs {
        if (hasReleaseKeystore) {
            create("release") {
                storeFile = rootProject.file(keystoreProperties["storeFile"] as String)
                storePassword = keystoreProperties["storePassword"] as String
                keyAlias = keystoreProperties["keyAlias"] as String
                keyPassword = keystoreProperties["keyPassword"] as String
            }
        }
    }

    buildTypes {
        release {
            // R8 混淆会破坏 JNI 动态注册（FindClass 按 AquaNative 全名查找），
            // 优化保持关闭。
            signingConfig = if (hasReleaseKeystore) {
                signingConfigs.getByName("release")
            } else {
                signingConfigs.getByName("debug") // 无正式签名时回退 debug，产物仍可安装
            }
            optimization {
                enable = false
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    buildFeatures {
        compose = true
    }
}

dependencies {
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.compose.material.icons.core)
    implementation(libs.androidx.compose.material.icons.extended)
    implementation(libs.androidx.media)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.runtime.compose)
    testImplementation(libs.junit)
    androidTestImplementation(platform(libs.androidx.compose.bom))
    androidTestImplementation(libs.androidx.compose.ui.test.junit4)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.androidx.junit)
    debugImplementation(libs.androidx.compose.ui.test.manifest)
    debugImplementation(libs.androidx.compose.ui.tooling)
}
