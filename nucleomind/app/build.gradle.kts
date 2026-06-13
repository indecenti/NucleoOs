plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.nucleoos.mind"
    compileSdk = 34
    buildToolsVersion = "34.0.0"

    defaultConfig {
        applicationId = "com.nucleoos.mind"
        minSdk = 26
        targetSdk = 34
        versionCode = 2
        versionName = "2.0.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    kotlinOptions {
        jvmTarget = "11"
    }
    buildFeatures {
        compose = true
    }
    composeOptions {
        kotlinCompilerExtensionVersion = "1.5.14"
    }
    packagingOptions {
        resources.excludes += "/META-INF/{AL2.0,LGPL2.1}"
    }
}

dependencies {
    // Stack 2023 (tutto bytecode Java 11) così il dexer D8 di AGP 7.4 lo digerisce.
    // BOM 2023.10.01 → Compose 1.5.4 / Material3 1.1.2, compatibili con compileSdk 34.
    val composeBom = platform("androidx.compose:compose-bom:2023.10.01")
    implementation(composeBom)

    implementation("androidx.core:core-ktx:1.12.0")
    implementation("androidx.activity:activity-compose:1.8.2")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.6.2")
    implementation("androidx.lifecycle:lifecycle-service:2.6.2")

    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.material3:material3")

    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.7.3")

    // Embedded HTTP server (OpenAI-like endpoints).
    implementation("org.nanohttpd:nanohttpd:2.3.1")

    // On-device LLM inference (MediaPipe). NOTA: escluso da questa build locale
    // AGP 7.4 perché il dexer D8 non converte il suo bytecode recente. Ripristina
    // `implementation("com.google.mediapipe:tasks-genai:0.10.27")` nella build moderna.

    // QR code per il pairing istantaneo (mostra l'URL del server).
    implementation("com.google.zxing:core:3.5.3")
}
