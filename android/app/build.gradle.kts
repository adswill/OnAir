// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

// ---- Version: from the repository's CMakeLists.txt, project(OnAir VERSION x.y.z) ----------------------------------------------
// -PonairVersionName=0.3.0-beta.2 (or env ONAIR_VERSION, as the CI sets from a tag) overrides the name, e.g. for a beta.
// versionCode = (major*10000 + minor*100 + patch) * 100 + (beta number, or 99 for a stable release): 0.2.9 -> 20999,
// 0.3.0-beta.1 -> 30001 < 0.3.0 -> 30099. Always above the old hand-set codes (<= 4). -PonairVersionCode=N overrides it.
val repoCmake = rootProject.file("../CMakeLists.txt")
val cmakeVersion: String = repoCmake.takeIf { it.exists() }?.readText()
    ?.let { Regex("""project\(\s*OnAir\s+VERSION\s+(\d+\.\d+\.\d+)""").find(it)?.groupValues?.get(1) }
    ?: "0.0.0"
val onairVersionName: String = (findProperty("onairVersionName") as String?)
    ?: System.getenv("ONAIR_VERSION")?.removePrefix("v")?.takeIf { it.isNotBlank() }
    ?: cmakeVersion
val onairVersionCode: Int = (findProperty("onairVersionCode") as String?)?.toInt() ?: run {
    val m = Regex("""^(\d+)\.(\d+)\.(\d+)(?:-beta\.(\d+))?""").find(onairVersionName)
    if (m == null) 1 else {
        val (a, b, c) = m.destructured
        val beta = m.groupValues[4].toIntOrNull()?.coerceIn(0, 98) ?: 99
        (a.toInt() * 10000 + b.toInt() * 100 + c.toInt()) * 100 + beta
    }
}

// ---- Signing: never in git. android/README.md "Signing" ---------------------------------------------------------------------
// 1. A properties file outside git: -PonairKeystoreProperties=/path/keystore.properties, env ONAIR_KEYSTORE_PROPERTIES, or
//    android/keystore.properties (gitignored). Keys: storeFile (relative paths resolve against the file's folder), storePassword,
//    keyAlias, keyPassword.
// 2. Or environment variables (the CI, from repository secrets): ONAIR_STORE_FILE, ONAIR_STORE_PASSWORD, ONAIR_KEY_ALIAS, ONAIR_KEY_PASSWORD.
// Without either, the release APK is built unsigned (fine for CI checks; it cannot be installed until signed).
val ksFile: File? = ((findProperty("onairKeystoreProperties") as String?) ?: System.getenv("ONAIR_KEYSTORE_PROPERTIES"))
    ?.let { file(it) } ?: rootProject.file("keystore.properties").takeIf { it.exists() }
val ksProps = Properties().apply { ksFile?.takeIf { it.exists() }?.inputStream()?.use { load(it) } }
fun signingValue(prop: String, env: String): String? = ksProps.getProperty(prop) ?: System.getenv(env)
val storePath: String? = signingValue("storeFile", "ONAIR_STORE_FILE")
val storeFileResolved: File? = storePath?.let { p -> File(p).let { if (it.isAbsolute) it else File(ksFile?.parentFile ?: rootProject.projectDir, p) } }
val canSign = storeFileResolved?.exists() == true && signingValue("storePassword", "ONAIR_STORE_PASSWORD") != null &&
    signingValue("keyAlias", "ONAIR_KEY_ALIAS") != null

android {
    namespace = "io.github.adswill.onair"
    compileSdk = 35

    defaultConfig {
        applicationId = "io.github.adswill.onair"
        minSdk = 28
        targetSdk = 35
        versionCode = onairVersionCode
        versionName = onairVersionName
        ndk { abiFilters += "arm64-v8a" }
    }

    signingConfigs {
        if (canSign) create("release") {
            storeFile = storeFileResolved
            storePassword = signingValue("storePassword", "ONAIR_STORE_PASSWORD")
            keyAlias = signingValue("keyAlias", "ONAIR_KEY_ALIAS")
            keyPassword = signingValue("keyPassword", "ONAIR_KEY_PASSWORD") ?: signingValue("storePassword", "ONAIR_STORE_PASSWORD")
        }
    }

    buildTypes {
        debug {
            // installs next to the release app, so testing never touches the signed install
            applicationIdSuffix = ".debug"
            versionNameSuffix = "-debug"
            isMinifyEnabled = false
        }
        release {
            if (canSign) signingConfig = signingConfigs.getByName("release")
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    lint { checkReleaseBuilds = false }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    // Shared data from the repository (one source for desktop and phone), copied by syncOnAirAssets below
    sourceSets["main"].assets.srcDir(project.layout.buildDirectory.dir("generated/onairAssets").get().asFile)

    testOptions { unitTests.isReturnDefaultValues = true }

    packaging {
        jniLibs {
            // libonair.so is built by tools/build_native.sh (native/CMakeLists.txt: the desktop core + the JNI bridge) into
            // src/main/jniLibs/arm64-v8a/ (gitignored); Gradle only packages it.
            useLegacyPackaging = true
        }
    }
}

// ---- Assets from the repository ------------------------------------------------------------------------------------------
// The first-run lessons (data/lessons/*.json, shared with the desktop's setup guide) and the credits (tools/credits_data.tsv,
// tools/contributors.tsv, app/panels/supporters_data.h, LICENSES/OFL-1.1.txt for the bundled fonts) go into the APK's assets as they
// are, so the phone shows the same lessons and credits as the desktop and nothing is kept twice. Missing files are simply skipped.
val repoRoot = rootProject.file("..")
val syncOnAirAssets by tasks.registering(Sync::class) {
    into(layout.buildDirectory.dir("generated/onairAssets"))
    from(File(repoRoot, "data/lessons")) { include("*.json"); into("lessons") }
    from(File(repoRoot, "tools")) { include("credits_data.tsv", "contributors.tsv"); into("credits") }
    from(File(repoRoot, "app/panels")) { include("supporters_data.h"); into("credits") }
    from(File(repoRoot, "LICENSES")) { include("OFL-1.1.txt"); into("credits") }
}
tasks.matching { it.name.startsWith("merge") && it.name.endsWith("Assets") }.configureEach { dependsOn(syncOnAirAssets) }
tasks.matching { it.name == "preBuild" || it.name.contains("lint", ignoreCase = true) }.configureEach { dependsOn(syncOnAirAssets) }

// A release without the native library would install but receive nothing: stop the build instead. Debug builds only warn.
val nativeLib = file("src/main/jniLibs/arm64-v8a/libonair.so")
tasks.matching { it.name == "preReleaseBuild" }.configureEach {
    doFirst {
        if (!nativeLib.exists()) throw GradleException("Missing $nativeLib: run android/tools/build_native.sh first (android/README.md)")
    }
}
tasks.matching { it.name == "preDebugBuild" }.configureEach {
    doFirst { if (!nativeLib.exists()) logger.warn("warning: $nativeLib is missing; the app will start without the receiver") }
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2024.12.01")
    implementation(composeBom)
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")
    // @Preview of every screen (ui/Previews.kt); the tooling itself only in debug builds
    implementation("androidx.compose.ui:ui-tooling-preview")
    debugImplementation("androidx.compose.ui:ui-tooling")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.7")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.7")
    implementation("androidx.datastore:datastore-preferences:1.1.1")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.9.0")

    // JVM unit tests (src/test: UI logic, navigation, deep links, USB device filter, permission/hot-plug flow with fakes, network radio forms): ./gradlew testDebugUnitTest
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.9.0")
}
