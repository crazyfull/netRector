cmake_minimum_required(VERSION 3.16)

project(MuxTunnelServer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Location of the netRector source tree. It is built from source as a static library and
# linked into the executable. Point it somewhere else with: -DNETRECTOR_DIR=/path/to/netRector
set(NETRECTOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "netRector source directory")
option(MUXTUNNEL_STATIC "Link the executable fully static" ON)

# TLS (BoringSSL) is optional and off by default. To enable it later:
#   -DNETRECTOR_ENABLE_TLS=ON -DBORINGSSL_DIR=/path/to/boringssl
set(NETRECTOR_ENABLE_TLS OFF CACHE BOOL "Enable TLS support in netRector")
set(NETRECTOR_BUILD_EXAMPLE OFF CACHE BOOL "Build the netRector example application" FORCE)

if(NOT EXISTS "${NETRECTOR_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "netRector not found in '${NETRECTOR_DIR}' (use -DNETRECTOR_DIR=...)")
endif()
add_subdirectory("${NETRECTOR_DIR}" "${CMAKE_BINARY_DIR}/netRector")

add_executable(muxTunnelServer
    src/main.cpp
    src/ServerApplication.cpp
    src/ServerCommandLine.cpp
    src/ServerTunnel.cpp
    src/TargetConnection.cpp
    src/WebSocketServerHandshake.cpp
    src/Socks5RequestParser.cpp
    src/Socks5Reply.cpp
    src/findHeaderValue.cpp
    src/parseUnsigned.cpp
    src/splitHostPort.cpp
    src/raiseFileDescriptorLimit.cpp
    src/blockTerminationSignals.cpp
)

target_include_directories(muxTunnelServer PRIVATE src)
target_link_libraries(muxTunnelServer PRIVATE netRector)
target_compile_options(muxTunnelServer PRIVATE -Wall -Wextra)

set_target_properties(muxTunnelServer PROPERTIES OUTPUT_NAME "mux-tunnelServer")

if(MUXTUNNEL_STATIC)
    target_link_options(muxTunnelServer PRIVATE -static -static-libgcc -static-libstdc++)
endif()
