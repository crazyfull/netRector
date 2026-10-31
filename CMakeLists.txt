
cmake_minimum_required(VERSION 3.16)

project(netRector
    VERSION 1.0.0
    LANGUAGES C CXX
)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "netRector currently supports Linux only")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# help
# cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
# cmake --build build -j"$(nproc)"

# Static library output
add_library(netRector STATIC
    src/clsBufferPool.cpp
    src/clsDNSLookup.cpp
    src/clsEpollReactor.cpp
    src/clsIntrusiveList.cpp
    src/clsListener.cpp
    src/clsLog.cpp
    src/clsUDPSocket.cpp
    src/clsMultiplexedTunnel.cpp
    src/clsSendQueue.cpp
    src/clsServer.cpp
    src/clsSocketList.cpp
    src/clsTCPSocket.cpp
    src/clsTLSContext.cpp
    src/clsTimer.cpp
    src/clsTimerManager.cpp
    src/tlsf.c
)

set(NETRECTOR_PUBLIC_HEADERS
    src/clsBufferPool.h
    src/clsDNSLookup.h
    src/clsEpollReactor.h
    src/clsIntrusiveList.h
    src/clsListener.h
    src/clsLog.h
    src/clsMultiplexedTunnel.h
    src/clsSendQueue.h
    src/clsServer.h
    src/clsSocketList.h
    src/clsTCPSocket.h
    src/clsTLSContext.h
    src/clsTimer.h
    src/clsTimerManager.h
    src/clsUDPSocket.h
    src/tlsf.h
    src/clsReactorObject.h
    src/epoll.h
    src/SocketContext.h
    src/clsGCList.h
    src/constants.h
)

add_custom_command(TARGET netRector POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_BINARY_DIR}/include"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${NETRECTOR_PUBLIC_HEADERS}
            "${CMAKE_BINARY_DIR}/include"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Copying netRector public headers"
)

# Public headers
target_include_directories(netRector
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)

# BoringSSL: -DBORINGSSL_DIR=... or env BORINGSSL_DIR, default libs/boringssl
# (scripts/build_boringssl.sh creates libs/boringssl/{include,lib}).
set(BORINGSSL_DIR "" CACHE PATH "BoringSSL prefix (include/ and lib/ or libssl.a directly inside)")
if(NOT BORINGSSL_DIR AND DEFINED ENV{BORINGSSL_DIR})
    set(BORINGSSL_DIR "$ENV{BORINGSSL_DIR}")
endif()
if(NOT BORINGSSL_DIR)
    set(BORINGSSL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/boringssl")
endif()
set(BORINGSSL_ROOT "${BORINGSSL_DIR}")

find_library(BORINGSSL_SSL
    NAMES ssl
    PATHS "${BORINGSSL_ROOT}"
    PATH_SUFFIXES lib
    NO_DEFAULT_PATH
    REQUIRED
)

find_library(BORINGSSL_CRYPTO
    NAMES crypto
    PATHS "${BORINGSSL_ROOT}"
    PATH_SUFFIXES lib
    NO_DEFAULT_PATH
    REQUIRED
)

target_include_directories(netRector
    PUBLIC
        "${BORINGSSL_ROOT}/include"
)

# System dependencies
find_package(Threads REQUIRED)

target_link_libraries(netRector
    PUBLIC
        "${BORINGSSL_SSL}"
        "${BORINGSSL_CRYPTO}"
        Threads::Threads
        ${CMAKE_DL_LIBS}
)

# Warnings
target_compile_options(netRector
    PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra>
)

# Archive output name: libnetRector.a
set_target_properties(netRector PROPERTIES
    OUTPUT_NAME netRector
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# Example/test application (example/main.cpp): SOCKS5-over-Yamux tunnel, HTTP bench server, UDP echo.
# Built as build/netRector, which tests/ and bench/ expect.
# Default ON when netRector is the top-level project, OFF when used via add_subdirectory().
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(NETRECTOR_EXAMPLE_DEFAULT ON)
else()
    set(NETRECTOR_EXAMPLE_DEFAULT OFF)
endif()
option(NETRECTOR_BUILD_EXAMPLE "Build the example application (build/netRector)" ${NETRECTOR_EXAMPLE_DEFAULT})
option(NETRECTOR_STATIC "Link the example application fully static (release binaries)" OFF)

if(NETRECTOR_BUILD_EXAMPLE)
    add_executable(netRector_example example/main.cpp)

    target_include_directories(netRector_example
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/example"
    )

    target_link_libraries(netRector_example
        PRIVATE
            netRector
    )

    set_target_properties(netRector_example PROPERTIES
        OUTPUT_NAME netRector
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    if(NETRECTOR_STATIC)
        target_link_options(netRector_example PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endif()
