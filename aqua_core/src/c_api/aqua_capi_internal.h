#ifndef AQUA_CAPI_INTERNAL_H
#define AQUA_CAPI_INTERNAL_H

// C API 内部共享头：aqua_capi.cpp 与 Android JNI 桥（同 aqua_capi 目标）之间
// 的私有契约，**不进安装头、不是 ABI**。

#include "aqua/c_api/aqua_capi.h"

// 句柄浅校验：magic 匹配才认为是本库创建的 client。用于 JNI 边界——jlong
// 可被 Java 侧伪造，或引用已 destroy 的实例。注意：这**不能**把
// use-after-free 变成良定义（已释放内存上的 magic 读取依赖实现行为），
// 只是尽早暴露误用的防御层；destroy 会在释放前清零 magic。
[[nodiscard]] bool aqua_client_handle_valid(const aqua_client_t* client) noexcept;

#endif // AQUA_CAPI_INTERNAL_H
