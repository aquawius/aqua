####################### start #######################
cmake_minimum_required(VERSION 3.27)
project(aqua LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)


set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")

# 构建选项配置
option(BUILD_CLIENT "Build client component" ON)
option(BUILD_SERVER "Build server component" ON)

option(BUILD_QT_SERVER "Build Qt server components" ON)
option(BUILD_QT_CLIENT "Build Qt client components" ON)

option(BUILD_SHARED_LIBS "Build using shared libraries" OFF)

####################### prepare #######################

# for boost.asio need this value in windows compile.
if (WIN32)
    add_compile_definitions(_UNICODE UNICODE _WIN32_WINNT=0x0A00)
    # set(Boost_ROOT "C:/Boost" CACHE PATH "Boost root")
    # set(gRPC_ROOT "C:/Program Files (x86)/grpc" CACHE PATH "gRPC root")
    # include_directories("${gRPC_ROOT}/include")
endif ()

message(STATUS "Finding libraries which required.")
find_package(spdlog CONFIG REQUIRED)
find_package(cxxopts CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

# https://github.com/faaxm/exmpl-cmake-grpc
# 设置 proto 文件路径
set(PROTO_PATH "${CMAKE_CURRENT_SOURCE_DIR}/public/proto")
set(PROTO_FILES
        "${PROTO_PATH}/audio_service.proto"
)

# 创建公共 proto 库
add_library(aqua_proto STATIC ${PROTO_FILES})
target_link_libraries(aqua_proto PUBLIC
        protobuf::libprotobuf
        gRPC::grpc
        gRPC::grpc++
)

# 我也不知道为什么， 生成的位置在/public/proto
target_include_directories(aqua_proto PUBLIC ${CMAKE_BINARY_DIR})
target_include_directories(aqua_proto PUBLIC
    ${CMAKE_BINARY_DIR}/public/proto
)

get_target_property(grpc_cpp_plugin_location gRPC::grpc_cpp_plugin LOCATION)
protobuf_generate(TARGET aqua_proto LANGUAGE cpp)
protobuf_generate(TARGET aqua_proto LANGUAGE grpc GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc PLUGIN "protoc-gen-grpc=${grpc_cpp_plugin_location}")

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

# 安装aqua_proto库
install(TARGETS aqua_proto
        EXPORT aquaTargets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
        INCLUDES DESTINATION include
)
