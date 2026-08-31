# JNI 动态注册（aqua_jni.cpp 的 FindClass + RegisterNatives）按类全名
# com.aquawius.aqua.native.AquaNative 查找；R8 的裁剪/混淆不得动它。
# AquaNative 是 Kotlin object，{ *; } 同时保住类名、INSTANCE 字段与 native 方法名/签名。
-keep class com.aquawius.aqua.native.AquaNative { *; }
