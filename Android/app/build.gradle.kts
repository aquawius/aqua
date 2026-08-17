plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

// App 版本号单一来源：仓库根 CMakeLists.txt 的 AQUA_ANDROID_VERSION /
// AQUA_ANDROID_VERSION_CODE（Android 版本不生成 C++ 头，Gradle 直接读本文件）。
val rootCmakeText = rootProject.projectDir.parentFile.resolve("CMakeLists.txt").readText()
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

android {
    namespace = "com.aquawius.aqua"

    compileSdk {
        version = release(37)
    }

    defaultConfig {
        applicationId = "com.aquawius.aqua"
        minSdk = 26
        targetSdk = 37
        versionCode = aquaAndroidVersionCode
        versionName = aquaAndroidVersion

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    buildTypes {
        release {
            // R8 混淆会破坏 JNI 动态注册（FindClass 按 AquaNative 全名查找），
            // 优化保持关闭；用 debug 签名使 assembleRelease 产物可直接安装。
            signingConfig = signingConfigs.getByName("debug")
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