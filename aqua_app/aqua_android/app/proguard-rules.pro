# JNI 动态注册（aqua_jni.cpp 的 FindClass + RegisterNatives）按类全名
# com.aquawius.aqua.native.AquaNative 查找；R8 的裁剪/混淆不得动它。
# AquaNative 是 Kotlin object，{ *; } 同时保住类名、INSTANCE 字段与 native 方法名/签名。
-keep class com.aquawius.aqua.native.AquaNative { *; }

# R8 full mode 会把 AquaClient 与它引用的 AquaDiagnostics 做垂直合并/方法内联。
# AquaClient 的构造函数已有 16 个参数，其中 4 个 Double 各占 2 个寄存器槽，
# 参数寄存器共 18 个；合并后生成的方法超过 ART verifier 的 16 参数寄存器上限，
# release 构建点击连接即 VerifyError（debug 不混淆，不受影响）。
# 两个类都是纯 Kotlin 状态/数据持有者；JNI 只动态注册 AquaNative，keep 它们安全。
-keep class com.aquawius.aqua.AquaClient { *; }
-keep class com.aquawius.aqua.AquaDiagnostics { *; }
