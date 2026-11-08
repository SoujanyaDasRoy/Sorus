plugins {
    id("com.android.application") version "9.0.1" apply false
}

// Keep build output out of OneDrive.
allprojects {
    layout.buildDirectory.set(file("${System.getProperty("user.home")}/build/sorus/a1/${project.name}"))
}
