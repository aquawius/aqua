import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

// App 版本号单一来源：仓库根 CMakeLists.txt 的 AQUA_ANDROID_VERSION /
// AQUA_ANDROID_VERSION_CODE（Android 版本不生成 C++ 头，Gradle 直接读本文件）。
// aqua_app/aqua_android 的上一级（rootProject.projectDir.parentFile）是仓库根。
val rootCmakeText = rootProject.projectDir.parentFile
    .resolve("CMakeLists.txt").readText()
val aquaAndroidVersion: String =
    Regex("""set\(AQUA_ANDROID_VERSION\s+"([^"]+)"\)""")
        .find(rootCmakeText)
        ?.groupValues?.get(1)
        ?: error("AQUA_ANDROID_VERSION not found in root CMakeLists.txt")
val aquaAndroidVersionCode: Int =
    Regex("""set\(AQUA_ANDROID_VERSION_CODE\s+(\d+)\)""")
        .find(rootCmakeText)
        ?.groupValues?.get(1)?.toInt()
        ?: error("AQUA_ANDROID_VERSION_CODE not found in root CMakeLists.txt")

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
