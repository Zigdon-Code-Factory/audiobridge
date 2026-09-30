# MainActivity is called from native code via JNI (onNativeEvent) and hosts the
# external native methods; R8 must not strip or rename them.
-keep class com.audiobridge.audiobridge.MainActivity {
    *;
}
-keepclasseswithmembernames class * {
    native <methods>;
}
