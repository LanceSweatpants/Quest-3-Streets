import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    kotlin("plugin.serialization") version "2.2.21"
    id("androidx.baselineprofile")
}

// StreetMR public build: gradlew :app:assembleQuestDebug -PstreetPublic=true
//   [-PstreetAppName="Quest 3 Street"] [-PstreetVersion=0.1.0-alpha1] [-PstreetVersionCode=100]
// gives the app id io.github.lancesweatpants.street (no .quest/.street suffixes), its own launcher
// name and version, and BuildConfig.STREET_PUBLIC. The native side is the same build either way
// (it checks the app id). Without the property the build is the private one
// (org.dolphinemu.dolphinemu.quest.street, "Street MR", -street-<tag>).
val streetPublic = project.findProperty("streetPublic")?.toString()?.trim()?.toBoolean() ?: false
val streetAppName = project.findProperty("streetAppName")?.toString() ?: "Quest 3 Street"
val streetVersion = project.findProperty("streetVersion")?.toString() ?: "0.1.0-alpha1"
val streetVersionCode = project.findProperty("streetVersionCode")?.toString()?.toInt() ?: 100

@Suppress("UnstableApiUsage")
android {
    compileSdkVersion = "android-36"
    ndkVersion = "29.0.14206865"

    buildFeatures {
        viewBinding = true
        buildConfig = true
    }

    flavorDimensions += "device"

    compileOptions {
        // Flag to enable support for the new language APIs
        isCoreLibraryDesugaringEnabled = true

        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlin {
        compilerOptions {
            jvmTarget = JvmTarget.fromTarget("17")
        }
    }

    lint {
        // This is important as it will run lint but not abort on error
        // Lint has some overly obnoxious "errors" that should really be warnings
        abortOnError = false

        //Uncomment disable lines for test builds...
        //disable "MissingTranslation"
        //disable "ExtraTranslation"
    }

    defaultConfig {
        applicationId = if (streetPublic) "io.github.lancesweatpants.street" else "org.dolphinemu.dolphinemu"
        minSdk = 21
        targetSdk = 36

        versionCode = if (streetPublic) streetVersionCode else getBuildVersionCode()

        versionName = if (streetPublic) streetVersion else getGitVersion()

        buildConfigField("String", "GIT_HASH", "\"${getGitHash()}\"")
        buildConfigField("String", "BRANCH", "\"${getBranch()}\"")
        buildConfigField("boolean", "STREET_PUBLIC", streetPublic.toString())

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    signingConfigs {
        create("release") {
            if (project.hasProperty("keystore")) {
                storeFile = file(project.property("keystore")!!)
                storePassword = project.property("storepass").toString()
                keyAlias = project.property("keyalias").toString()
                keyPassword = project.property("keypass").toString()
            }
        }
    }

    // Define build types, which are orthogonal to product flavors.
    buildTypes {
        // Signed by release key, allowing for upload to Play Store.
        release {
            if (project.hasProperty("keystore")) {
                signingConfig = signingConfigs.getByName("release")
            }

            resValue("string", "app_name_suffixed", "Dolphin Emulator")
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android.txt"),
                "proguard-rules.pro"
            )
        }

        // Signed by debug key disallowing distribution on Play Store.
        // Attaches "debug" suffix to version and package name, allowing installation alongside the release build.
        debug {
            // StreetMR: own launcher name and app id (installs next to Hitz MR and Baseball MR), and
            // a build tag we can check on the headset. The public build has a clean id and version.
            if (streetPublic) {
                resValue("string", "app_name_suffixed", streetAppName)
                applicationIdSuffix = null
                versionNameSuffix = null
            } else {
                resValue("string", "app_name_suffixed", "Street MR")
                applicationIdSuffix = ".street"
                versionNameSuffix = "-street-" + (project.findProperty("streetTag") ?: "dev")
            }
            isJniDebuggable = true
        }

        // Diagnostic release variants. These keep the release application ID so they can be
        // tested against the same Quest package data/runtime profile as the real release build.
        // They require the same release signing properties as questRelease.
        create("releaseNoMinify") {
            initWith(getByName("release"))
            matchingFallbacks += listOf("release")
            signingConfig = signingConfigs.getByName("debug")
            isMinifyEnabled = false
            isShrinkResources = false
        }

        create("releaseDebuggable") {
            initWith(getByName("releaseNoMinify"))
            matchingFallbacks += listOf("release")
            isDebuggable = true
            isJniDebuggable = true
        }
    }

    productFlavors {
        create("standard") {
            dimension = "device"
            buildConfigField("boolean", "IS_QUEST", "false")
            ndk {
                abiFilters += listOf("arm64-v8a", "x86_64")
            }
            externalNativeBuild {
                cmake {
                    arguments += "-DENABLE_VR=OFF"
                }
            }
        }

        create("quest") {
            dimension = "device"
            applicationIdSuffix = if (streetPublic) null else ".quest"
            versionNameSuffix = if (streetPublic) null else "-quest"
            buildConfigField("boolean", "IS_QUEST", "true")
            manifestPlaceholders["questSupportedDevices"] = "quest2|quest3|quest3s|questpro"
            ndk {
                abiFilters += listOf("arm64-v8a")
            }
            externalNativeBuild {
                cmake {
                    arguments += "-DENABLE_VR=ON"
                }
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../../../CMakeLists.txt")
            version = "3.22.1+"
        }
    }
    namespace = "org.dolphinemu.dolphinemu"

    defaultConfig {
        externalNativeBuild {
            cmake {
                arguments(
                    "-DANDROID_STL=c++_static",
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
                    "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
                    // , "-DENABLE_GENERIC=ON"
                )
                // StreetMR: no public/private CMake argument on purpose. A different argument list
                // means a separate .cxx folder (full native rebuild); the native side tells the
                // public build apart at run time from the app id.

                // Uncomment the line below if you don't want to build the C++ unit tests
                // HitzMR: skip the unit tests (saves disk and link time).
                targets("main", "hook_impl", "main_hook", "gsl_alloc_hook", "file_redirect_hook")
            }
        }
    }

    packaging {
        jniLibs.useLegacyPackaging = true
    }
}

dependencies {
    baselineProfile(project(":benchmark"))
    coreLibraryDesugaring("com.android.tools:desugar_jdk_libs:2.1.5")

    implementation("androidx.core:core-ktx:1.17.0")
    implementation("androidx.appcompat:appcompat:1.7.1")
    implementation("androidx.cardview:cardview:1.0.0")
    implementation("androidx.recyclerview:recyclerview:1.4.0")
    implementation("androidx.constraintlayout:constraintlayout:2.2.1")
    implementation("androidx.fragment:fragment-ktx:1.8.9")
    implementation("androidx.slidingpanelayout:slidingpanelayout:1.2.0")
    implementation("com.google.android.material:material:1.13.0")
    implementation("androidx.core:core-splashscreen:1.2.0")
    implementation("androidx.preference:preference-ktx:1.2.1")
    implementation("androidx.profileinstaller:profileinstaller:1.4.1")

    // Kotlin extensions for lifecycle components
    implementation("androidx.lifecycle:lifecycle-viewmodel-ktx:2.9.4")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.9.4")

    // Android TV UI libraries.
    implementation("androidx.leanback:leanback:1.2.0")
    implementation("androidx.tvprovider:tvprovider:1.1.0")
    implementation("androidx.swiperefreshlayout:swiperefreshlayout:1.1.0")

    // For loading game covers from disk and GameTDB
    implementation("io.coil-kt:coil:2.7.0")

    // For loading custom GPU drivers
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.9.0")

    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.10.2")

    implementation("com.nononsenseapps:filepicker:4.2.1")
}

fun getGitVersion(): String {
    try {
        return ProcessBuilder("git", "describe", "--always", "--long")
            .directory(project.rootDir)
            .redirectOutput(ProcessBuilder.Redirect.PIPE)
            .redirectError(ProcessBuilder.Redirect.PIPE)
            .start().inputStream.bufferedReader().use { it.readText() }
            .trim()
            .replace(Regex("(-0)?-[^-]+$"), "")
    } catch (e: Exception) {
        logger.error("Cannot find git, defaulting to dummy version number")
    }

    return "0.0"
}

fun getBuildVersionCode(): Int {
    try {
        val commitCount = Integer.valueOf(
            ProcessBuilder("git", "rev-list", "--first-parent", "--count", "HEAD")
                .directory(project.rootDir)
                .redirectOutput(ProcessBuilder.Redirect.PIPE)
                .redirectError(ProcessBuilder.Redirect.PIPE)
                .start().inputStream.bufferedReader().use { it.readText() }
                .trim()
        )

        val isRelease = ProcessBuilder("git", "describe", "--exact-match", "HEAD")
            .directory(project.rootDir)
            .redirectOutput(ProcessBuilder.Redirect.PIPE)
            .redirectError(ProcessBuilder.Redirect.PIPE)
            .start()
            .waitFor() == 0

        return commitCount * 2 + (if (isRelease) 0 else 1)
    } catch (e: Exception) {
        logger.error("Cannot find git, defaulting to dummy version code")
    }

    return 1
}

fun getGitHash(): String {
    try {
        return ProcessBuilder("git", "rev-parse", "HEAD")
            .directory(project.rootDir)
            .redirectOutput(ProcessBuilder.Redirect.PIPE)
            .redirectError(ProcessBuilder.Redirect.PIPE)
            .start().inputStream.bufferedReader().use { it.readText() }
            .trim()
    } catch (e: Exception) {
        logger.error("Cannot find git, defaulting to dummy git hash")
    }

    return "0"
}

fun getBranch(): String {
    try {
        return ProcessBuilder("git", "rev-parse", "--abbrev-ref", "HEAD")
            .directory(project.rootDir)
            .redirectOutput(ProcessBuilder.Redirect.PIPE)
            .redirectError(ProcessBuilder.Redirect.PIPE)
            .start().inputStream.bufferedReader().use { it.readText() }
            .trim()
    } catch (e: Exception) {
        logger.error("Cannot find git, defaulting to dummy git hash")
    }

    return "master"
}
