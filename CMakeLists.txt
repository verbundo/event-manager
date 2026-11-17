cmake_minimum_required(VERSION 3.28.0)
project(rooster VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

#Bring in the nlohmann/json library
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)

#Bring in the libuv library
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    libuv
    GIT_REPOSITORY https://github.com/libuv/libuv.git
    GIT_TAG v1.48.0
)

FetchContent_MakeAvailable(json libuv)

#Bring in the h2o library (libh2o, libuv backend).
#h2o has no recent release tag, so it is pinned to a master commit.
#Requires OpenSSL 3.x and zlib. h2o does not build against OpenSSL 4 yet, so on macOS prefer Homebrew's openssl@3
#(`brew install openssl@3`) over the default `brew --prefix openssl`. Override with -DOPENSSL_ROOT_DIR=...
if(APPLE AND NOT DEFINED OPENSSL_ROOT_DIR AND NOT DEFINED ENV{OPENSSL_ROOT_DIR})
    execute_process(COMMAND brew --prefix openssl@3
        OUTPUT_VARIABLE BREW_OPENSSL3_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(EXISTS "${BREW_OPENSSL3_PREFIX}/include/openssl/ssl.h")
        set(OPENSSL_ROOT_DIR ${BREW_OPENSSL3_PREFIX} CACHE PATH "OpenSSL installation used by h2o")
    endif()
endif()
set(WITH_MRUBY OFF CACHE BOOL "" FORCE)
set(WITH_DTRACE OFF CACHE BOOL "" FORCE)
set(WITH_CCACHE OFF CACHE BOOL "" FORCE)
set(WITH_BROTLI OFF CACHE BOOL "" FORCE)
set(WITH_ZSTD OFF CACHE BOOL "" FORCE)
#Make libh2o link against the libuv fetched above instead of whatever pkg-config finds on the system,
#so there is exactly one libuv in the process. Disabling PkgConfig for h2o forces its FindLIBUV fallback,
#which honours these pre-set cache variables.
set(LIBUV_INCLUDE_DIR ${libuv_SOURCE_DIR}/include CACHE PATH "" FORCE)
set(LIBUV_LIBRARIES uv_a CACHE STRING "" FORCE)
set(LIBUV_VERSION 1.48.0) # h2o rejects libuv when this is unset
set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig TRUE)
FetchContent_Declare(
    h2o
    GIT_REPOSITORY https://github.com/h2o/h2o.git
    GIT_TAG d3e43fcf00c3e6b17826b95d2d362d646185918a
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(h2o)
unset(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig)

#libh2o does not export its include directories, so attach them here
target_include_directories(libh2o SYSTEM INTERFACE
    ${h2o_SOURCE_DIR}/include
    ${h2o_SOURCE_DIR}/deps/picotls/include
    ${h2o_SOURCE_DIR}/deps/quicly/include
    ${h2o_SOURCE_DIR}/deps/klib
)
target_compile_definitions(libh2o INTERFACE H2O_USE_LIBUV=1)

# Copy config.json next to the executable (re-copied whenever it changes)
configure_file(${CMAKE_SOURCE_DIR}/config.json ${CMAKE_BINARY_DIR}/config.json COPYONLY)

# Development TLS certificates: generate them if missing, then copy next to the executable
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/ssl/server.crt OR NOT EXISTS ${CMAKE_SOURCE_DIR}/ssl/server.key)
    message(STATUS "Generating development TLS certificates in ${CMAKE_SOURCE_DIR}/ssl")
    execute_process(
        COMMAND ${CMAKE_SOURCE_DIR}/scripts/gen-ssl-certs.sh ${CMAKE_SOURCE_DIR}/ssl
        RESULT_VARIABLE GEN_SSL_RESULT
    )
    if(NOT GEN_SSL_RESULT EQUAL 0)
        message(WARNING "Could not generate TLS certificates; run scripts/gen-ssl-certs.sh manually")
    endif()
endif()
foreach(SSL_FILE ca.crt server.crt server.key)
    if(EXISTS ${CMAKE_SOURCE_DIR}/ssl/${SSL_FILE})
        configure_file(${CMAKE_SOURCE_DIR}/ssl/${SSL_FILE} ${CMAKE_BINARY_DIR}/ssl/${SSL_FILE} COPYONLY)
    endif()
endforeach()

add_executable(${PROJECT_NAME}
    src/main.cpp
    src/http/http_config.cpp
    src/http/http_server.cpp
)
target_include_directories(${PROJECT_NAME} PRIVATE src)
target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json uv_a libh2o)
