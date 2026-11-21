# MainActivity and NativeMic host JNI native methods (and MainActivity is called
# from native code via onNativeEvent); R8 must not strip or rename them.
-keep class com.audiobridge.audiobridge.MainActivity {
    *;
}
-keep class com.audiobridge.audiobridge.NativeMic {
    *;
}
-keepclasseswithmembernames class * {
    native <methods>;
}
