plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.kotlin.serialization)
}
android {
    namespace = "io.mooncheck.app"
    compileSdk = 35
    defaultConfig {
        applicationId = "io.mooncheck.app"
        minSdk = 26
        targetSdk = 35
        versionCode = 600
        versionName = rootProject.file("../VERSION").readText().trim()
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }
    buildFeatures { compose = true; buildConfig = true }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    signingConfigs.getByName("debug") {
        storeFile = rootProject.file(".gradle/mooncheck-debug.keystore")
    }
    buildTypes {
        release { isMinifyEnabled = true; proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro") }
    }
    packaging { resources.excludes += "/META-INF/{AL2.0,LGPL2.1}" }
    testOptions { unitTests.isIncludeAndroidResources = true }
}
dependencies {
    implementation(platform(libs.compose.bom))
    implementation(libs.compose.ui)
    implementation(libs.compose.preview)
    implementation(libs.compose.material)
    implementation(libs.compose.icons)
    implementation(libs.activity)
    implementation(libs.lifecycle.compose)
    implementation(libs.lifecycle.viewmodel)
    implementation(libs.coroutines)
    implementation(libs.serialization)
    debugImplementation(libs.compose.tooling)
    testImplementation(libs.junit)
    testImplementation(libs.coroutines.test)
    testImplementation(libs.robolectric)
    testImplementation(platform(libs.compose.bom))
    testImplementation(libs.compose.test)
    testImplementation(libs.android.test.junit)
    androidTestImplementation(platform(libs.compose.bom))
    androidTestImplementation(libs.compose.test)
    androidTestImplementation(libs.android.test.runner)
    androidTestImplementation(libs.android.test.junit)
    debugImplementation(libs.compose.test.manifest)
}

// Keep developer signing entirely local to this checkout. This is never a release key.
val prepareDebugKey by tasks.registering(Exec::class) {
    val key = rootProject.file(".gradle/mooncheck-debug.keystore")
    outputs.file(key)
    onlyIf { !key.exists() }
    doFirst { key.parentFile.mkdirs() }
    val executable = if (System.getProperty("os.name").startsWith("Windows")) "keytool.exe" else "keytool"
    commandLine("${System.getProperty("java.home")}/bin/$executable", "-genkeypair", "-noprompt",
        "-keystore", key.absolutePath, "-storepass", "android", "-alias", "androiddebugkey",
        "-keypass", "android", "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
        "-dname", "CN=Android Debug,O=Android,C=US")
}
tasks.configureEach {
    if (name == "validateSigningDebug" || name == "validateSigningDebugAndroidTest") dependsOn(prepareDebugKey)
}
tasks.withType<Test>().configureEach {
    maxHeapSize = "2g"
    systemProperty("robolectric.dependency.repo.url", "https://repo.maven.apache.org/maven2")
    val testHome = rootProject.layout.buildDirectory.dir("test-home").get().asFile
    systemProperty("user.home", testHome.absolutePath)
    systemProperty("maven.repo.local", rootProject.file(".gradle-home/robolectric").absolutePath)
    doFirst { testHome.mkdirs() }
}
