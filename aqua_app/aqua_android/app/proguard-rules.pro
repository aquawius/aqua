# JNI 动态注册（aqua_jni.cpp 的 FindClass + RegisterNatives）按类全名
# com.aquawius.aqua.native.AquaNative 查找；R8 的裁剪/混淆不得动它。
# AquaNative 是 Kotlin object，{ *; } 同时保住类名、INSTANCE 字段与 native 方法名/签名。
-keep class com.aquawius.aqua.native.AquaNative { *; }

# AquaClient：R8 full mode 曾把它和诊断解码路径做垂直合并，生成 ART verifier 拒绝的
# 调用 —— release 下点"连接"直接 VerifyError（debug 不混淆，不受影响）。类很小，
# keep 的代价可忽略；这是对"宽签名 + 合并"这类边界的保险，不是 JNI 的需要
# （JNI 只动态注册 AquaNative，按类全名查找）。
-keep class com.aquawius.aqua.AquaClient { *; }

# AquaDiagnostics **不 keep**：它已改成 LongArray 存储 + 单参数构造（见类注释），
# 不再有 166 参数的构造函数与 copy$default，R8 怎么合并都造不出宽签名方法。
# keep 它只会让 166 个 getter 无法裁剪/混淆。
