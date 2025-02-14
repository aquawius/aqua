####################### start #######################
cmake_minimum_required(VERSION 3.30)
project(aqua LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 构建选项配置
option(BUILD_CLIENT "Build client component" ON)
option(BUILD_SERVER "Build server component" ON)

option(BUILD_QT_SERVER "Build Qt server components" ON)
option(BUILD_QT_CLIENT "Build Qt server components" ON)

####################### prepare #######################

# fxxk cmake on windows cannot find package!
if (WIN32)
    add_compile_definitions(_UNICODE UNICODE _WIN32_WINNT=0x0A00)
    set(Boost_ROOT "C:/Boost" CACHE PATH "Boost root")
    set(gRPC_ROOT "C:/Program Files (x86)/grpc" CACHE PATH "gRPC root")
    include_directories("${gRPC_ROOT}/include")
endif ()

message(STATUS "Finding libraries which required.")
find_package(spdlog REQUIRED)
find_package(cxxopts REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

## 这块写的有点丑，但是我不知道怎样改，那两个简单的函数cmake找不到
# 定义路径
set(PROTO_PATH "${CMAKE_CURRENT_SOURCE_DIR}/public/proto")
set(PROTO_FILES "${PROTO_PATH}/audio_service.proto")
set(GENERATED_PROTO_PATH "${CMAKE_BINARY_DIR}/proto_gen")

file(MAKE_DIRECTORY ${GENERATED_PROTO_PATH})

# 查找 Protobuf 和 gRPC 编译插件
find_program(PROTOC_EXECUTABLE protoc)
if (NOT PROTOC_EXECUTABLE)
    message(FATAL_ERROR "Cannot find protoc compiler!")
endif ()

find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)
if (NOT GRPC_CPP_PLUGIN)
    message(FATAL_ERROR "Cannot find grpc_cpp_plugin!")
endif ()

# 生成 Protobuf 文件
add_custom_command(
        OUTPUT ${GENERATED_PROTO_PATH}/audio_service.pb.cc ${GENERATED_PROTO_PATH}/audio_service.pb.h
        COMMAND ${PROTOC_EXECUTABLE}
        ARGS --cpp_out=${GENERATED_PROTO_PATH}
        -I ${PROTO_PATH}
        ${PROTO_FILES}
        DEPENDS ${PROTO_FILES}
        COMMENT "Generating Protobuf code"
)

# 生成 gRPC 文件
add_custom_command(
        OUTPUT ${GENERATED_PROTO_PATH}/audio_service.grpc.pb.cc ${GENERATED_PROTO_PATH}/audio_service.grpc.pb.h
        COMMAND ${PROTOC_EXECUTABLE}
        ARGS --grpc_out=${GENERATED_PROTO_PATH}
        --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
        -I ${PROTO_PATH}
        ${PROTO_FILES}
        DEPENDS ${PROTO_FILES}
        COMMENT "Generating gRPC code"
)

# 设置源文件和头文件列表
set(PROTO_SRCS
        ${GENERATED_PROTO_PATH}/audio_service.pb.cc
        ${GENERATED_PROTO_PATH}/audio_service.grpc.pb.cc
)
set(PROTO_HDRS
        ${GENERATED_PROTO_PATH}/audio_service.pb.h
        ${GENERATED_PROTO_PATH}/audio_service.grpc.pb.h
)

# 创建公共proto库
add_library(aqua_proto STATIC ${PROTO_SRCS})
target_link_libraries(aqua_proto PUBLIC
        protobuf::libprotobuf
        gRPC::grpc++
)



target_include_directories(aqua_proto PUBLIC
        ${GENERATED_PROTO_PATH}
        ${PROTO_PATH}
)

# 条件编译
if (BUILD_CLIENT)
    add_subdirectory(aqua-client)
endif ()

if (BUILD_SERVER)
    add_subdirectory(aqua-server)
endif ()

if (BUILD_QT_SERVER OR BUILD_QT_CLIENT)
    add_subdirectory(aqua-qt)
endif()
