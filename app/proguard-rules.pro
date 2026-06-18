# Keep JNI entry points referenced from native code by name.
-keepclasseswithmembernames class com.spa.scream.ScreamBridge {
    native <methods>;
}
