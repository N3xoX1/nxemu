plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "org.nxemu"
    compileSdk = 36
    ndkVersion = "26.1.10909125"

    defaultConfig {
        applicationId = "org.nxemu"
        minSdk = 28
        targetSdk = 36
        versionCode = 1
        versionName = "0.1"

        ndk {
            abiFilters += "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                arguments("-DANDROID_ARM_NEON=true")
                abiFilters("arm64-v8a")
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    packaging {
        jniLibs.useLegacyPackaging = true
    }

    externalNativeBuild {
        cmake {
            version = "3.22.1"
            path = file("../../../CMakeLists.txt")
        }
    }
}
