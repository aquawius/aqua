plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

// App 版本号单一来源：仓库根 CMakeLists.txt 的 AQUA_ANDROID_CLI_VERSION
//（native 侧 version.h 中的 AQUA_ANDROID_CLI_VERSION 宏同源生成）。
val aquaAndroidVersion: String =
    Regex("""set\(AQUA_ANDROID_CLI_VERSION\s+"([^"]+)"\)""")
        .find(rootProject.projectDir.parentFile.resolve("CMakeLists.txt").readText())
        ?.groupValues?.get(1)
        ?: error("AQUA_ANDROID_CLI_VERSION not found in root CMakeLists.txt")

android {
    namespace = "com.aquawius.aqua"
    compileSdk {
        version = release(37)
    }

    defaultConfig {
        applicationId = "com.aquawius.aqua"
        minSdk = 26
        targetSdk = 37
        versionCode = 1
        versionName = aquaAndroidVersion

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    buildTypes {
        release {
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
    implementation("androidx.compose.material:material-icons-core")
    implementation("androidx.compose.material:material-icons-extended")
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