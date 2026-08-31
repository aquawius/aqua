# logger 模块测试
add_subdirectory(logger)

# diagnostics 模块测试
add_subdirectory(diagnostics)

# c_api 模块测试（AQUA_BUILD_C_API=ON 时有效）
add_subdirectory(c_api)

# net 模块测试
add_subdirectory(net)

# Header/domain tests that do not require platform audio backends.
add_subdirectory(audio)
