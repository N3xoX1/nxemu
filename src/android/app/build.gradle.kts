plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// The command-line script keeps Gradle outputs and native caches under build/.
// Android Studio keeps its usual directories unless this property is supplied.
val nxemuBuildDirectory = providers.gradleProperty("nxemuBuildDirectory").orNull?.let { file(it) }
if (nxemuBuildDirectory != null) {
    layout.buildDirectory.set(nxemuBuildDirectory.resolve("app"))
}

abstract class CopyCatalogues : DefaultTask() {
    @get:InputFiles
    abstract val catalogues: ConfigurableFileCollection

    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @TaskAction
    fun copy() {
        val target = outputDir.get().dir("ui/lang").asFile
        target.deleteRecursively()
        target.mkdirs()
        catalogues.forEach { it.copyTo(target.resolve(it.name)) }
    }
}

val copyCatalogues = tasks.register<CopyCatalogues>("copyCatalogues") {
    catalogues.from(fileTree(file("../../../lang")) { include("*.po") })
}

// The mods that ship with NxEmu: the game folders of external/nxemu-switch-mods ("<Game name> [<title id>]"), with
// their licenses. The app copies them out of its assets (org.nxemu.utils.BundledMods), as the loader reads files.
abstract class CopyBundledMods : DefaultTask() {
    @get:InputFiles
    abstract val mods: ConfigurableFileCollection

    @get:Internal
    abstract val modsRoot: DirectoryProperty

    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @TaskAction
    fun copy() {
        val target = outputDir.get().dir("mods").asFile
        target.deleteRecursively()
        target.mkdirs()
        val root = modsRoot.get().asFile
        mods.forEach { it.copyTo(target.resolve(it.relativeTo(root).path)) }
    }
}

val copyBundledMods = tasks.register<CopyBundledMods>("copyBundledMods") {
    val root = file("../../../external/nxemu-switch-mods")
    modsRoot.set(root)
    mods.from(fileTree(root) { include("*]/**") })
}

android {
    namespace = "org.nxemu"
    compileSdk = 36
    ndkVersion = "30.0.16248370"

    defaultConfig {
        applicationId = "org.nxemu"
        minSdk = 28
        targetSdk = 36
        versionCode = (project.findProperty("nxemuVersionCode") as String?)?.toInt() ?: 1
        versionName = project.findProperty("nxemuVersionName") as String? ?: "0.1"

        ndk {
            abiFilters += "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                arguments("-DANDROID_ARM_NEON=true")
                arguments("-DNXEMU_ENABLE_ANDROID_ADPF=${providers.gradleProperty("nxemuEnableAndroidAdpf").getOrElse("ON")}")
                // Gradle's worker limit does not limit Ninja's compilation jobs.
                arguments("-DNXEMU_COMPILE_JOBS=${providers.environmentVariable("NXEMU_BUILD_JOBS").getOrElse("2")}")
                providers.environmentVariable("NXEMU_THINLTO_CACHE_DIR").orNull?.let {
                    arguments("-DNXEMU_THINLTO_CACHE_DIR=$it")
                }
                abiFilters("arm64-v8a")
            }
        }
    }

    val releaseKeystore = System.getenv("NXEMU_KEYSTORE_FILE")
    signingConfigs {
        if (releaseKeystore != null) {
            create("release") {
                storeFile = file(releaseKeystore)
                storePassword = System.getenv("NXEMU_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("NXEMU_KEY_ALIAS")
                keyPassword = System.getenv("NXEMU_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName(if (releaseKeystore != null) "release" else "debug")
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
            if (nxemuBuildDirectory != null) {
                buildStagingDirectory = nxemuBuildDirectory.resolve("native")
            }
        }
    }
}

androidComponents {
    onVariants { variant ->
        variant.sources.assets?.addGeneratedSourceDirectory(copyCatalogues, CopyCatalogues::outputDir)
        variant.sources.assets?.addGeneratedSourceDirectory(copyBundledMods, CopyBundledMods::outputDir)
    }
}

dependencies {
    implementation("androidx.activity:activity-ktx:1.9.3")
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.drawerlayout:drawerlayout:1.2.0")
    implementation("androidx.core:core-splashscreen:1.0.1")
    implementation("androidx.documentfile:documentfile:1.0.1")
    implementation("androidx.window:window:1.3.0")
}
