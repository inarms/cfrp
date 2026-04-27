cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

project(cfrp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find dependencies
find_package(CLI11 CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(wolfssl CONFIG REQUIRED)

# tomlplusplus and ngtcp2 in vcpkg are easier to find via pkg-config
# On Windows, we search for pkgconf or pkg-config provided by vcpkg or system
if(NOT PKG_CONFIG_EXECUTABLE)
    find_program(PKG_CONFIG_EXECUTABLE NAMES pkgconf pkg-config)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(tomlplusplus REQUIRED IMPORTED_TARGET tomlplusplus)
pkg_check_modules(ngtcp2 REQUIRED IMPORTED_TARGET libngtcp2)
pkg_check_modules(ngtcp2_crypto REQUIRED IMPORTED_TARGET libngtcp2_crypto_wolfssl)

# Configure Asio to use wolfSSL as OpenSSL replacement
add_definitions(-DASIO_USE_WOLFSSL)

add_executable(cfrp 
    main.cpp
    src/server/server.cpp
    src/client/client.cpp
    src/common/mux.cpp
    src/common/quic_ngtcp2.cpp
    src/common/ssl_utils.cpp
    src/common/websocket_stream.cpp
)

target_include_directories(cfrp PRIVATE 
    src
)

# For ASIO to find openssl/*.h as wolfSSL headers
if(DEFINED ENV{VCPKG_ROOT})
    target_include_directories(cfrp SYSTEM PRIVATE 
        ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include/wolfssl
    )
endif()

target_link_libraries(cfrp PRIVATE 
    CLI11::CLI11 
    PkgConfig::tomlplusplus
    asio::asio 
    zstd::libzstd
    wolfssl::wolfssl
    PkgConfig::ngtcp2
    PkgConfig::ngtcp2_crypto
)

if(WIN32)
    target_compile_definitions(cfrp PRIVATE 
        _WIN32_WINNT=0x0A00
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(cfrp PRIVATE ws2_32)
endif()

# Copy configuration files to the build directory
file(COPY 
    ${CMAKE_CURRENT_SOURCE_DIR}/server.toml 
    ${CMAKE_CURRENT_SOURCE_DIR}/client.toml 
    DESTINATION ${CMAKE_CURRENT_BINARY_DIR}
)
