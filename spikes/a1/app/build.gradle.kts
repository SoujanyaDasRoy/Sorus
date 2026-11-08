plugins {
    id("com.android.application")
}

android {
    namespace = "dev.sorus.spike.a1"
    compileSdk = 36
    defaultConfig {
        applicationId = "dev.sorus.spike.a1"
        minSdk = 30  // wireless debugging (Shizuku without a PC) needs Android 11
        targetSdk = 36
        versionCode = 1
        versionName = "0.1-spike"
    }
}

dependencies {
    implementation("dev.rikka.shizuku:api:13.1.5")
    implementation("dev.rikka.shizuku:provider:13.1.5")
}
