# logger 模块测试
add_subdirectory(core/logger)

# net 模块测试（address_utils / UDP transport / gRPC），独立 target：aqua_net_tests。
add_subdirectory(core/net)
