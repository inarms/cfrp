cmake_minimum_required(VERSION 3.10)

set(CMAKE_TOOLCHAIN_FILE $ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake)

project(cfrp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find dependencies
find_package(CLI11 CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(zstd CONFIG REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(tomlplusplus REQUIRED IMPORTED_TARGET tomlplusplus)

add_executable(cfrp 
    main.cpp
    src/server/server.cpp
    src/client/client.cpp
)

target_include_directories(cfrp PRIVATE src)
target_link_libraries(cfrp PRIVATE CLI11::CLI11 PkgConfig::tomlplusplus asio::asio OpenSSL::SSL OpenSSL::Crypto zstd::libzstd)

# Copy configuration files to the build directory
file(COPY 
    ${CMAKE_CURRENT_SOURCE_DIR}/config_server.toml 
    ${CMAKE_CURRENT_SOURCE_DIR}/config_client.toml 
    DESTINATION ${CMAKE_CURRENT_BINARY_DIR}
)
