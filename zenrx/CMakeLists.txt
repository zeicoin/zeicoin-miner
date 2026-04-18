cmake_minimum_required(VERSION 3.10)
project(zenrx VERSION 1.4.1 LANGUAGES C CXX ASM)

# ==================================================
# Language standards
# ==================================================
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==================================================
# Options
# ==================================================
option(WITH_MSR "Enable MSR support" ON)
option(WITH_ICON "Include Windows icon resource" OFF)
option(BUILD_STATIC "Build static executable" ON)
option(WITH_NATIVE_CPU "Use -march=native for CPU-specific optimizations" OFF)
option(WITH_LTO "Enable Link-Time Optimization" OFF)

# ==================================================
# OS detection
# ==================================================
if(WIN32)
    set(ZENRX_OS_WIN ON)
    add_definitions(-DZENRX_OS_WIN -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0600)
elseif(APPLE)
    set(ZENRX_OS_APPLE ON)
    add_definitions(-DZENRX_OS_APPLE)
elseif(UNIX)
    set(ZENRX_OS_LINUX ON)
    add_definitions(-DZENRX_OS_LINUX)
endif()

# ==================================================
# Feature flags (x86_64 ASM support for RandomX JIT)
# ==================================================
add_definitions(-DZENRX_FEATURE_ASM)

# ==================================================
# Compiler flags
# ==================================================
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -pipe)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O3 -DNDEBUG)
        if(WITH_NATIVE_CPU)
            add_compile_options(-march=native)
        endif()
    endif()
endif()

if(MSVC)
    add_compile_options(/W4)
endif()

# ==================================================
# Static linking
# ==================================================
if(BUILD_STATIC)
    if(ZENRX_OS_WIN)
        set(CMAKE_EXE_LINKER_FLAGS
            "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
    elseif(ZENRX_OS_LINUX)
        # musl handles full static cleanly
        set(CMAKE_EXE_LINKER_FLAGS
            "${CMAKE_EXE_LINKER_FLAGS} -static")
    endif()
endif()

# ==================================================
# Threads
# ==================================================
find_package(Threads REQUIRED)

# ==================================================
# hwloc (REQUIRED, prebuilt)
# ==================================================
if(NOT HWLOC_INCLUDE_DIR OR NOT HWLOC_LIBRARY)
    message(FATAL_ERROR
        "hwloc not configured.\n"
        "Pass -DHWLOC_INCLUDE_DIR=... and -DHWLOC_LIBRARY=...")
endif()

if(NOT EXISTS "${HWLOC_LIBRARY}")
    message(FATAL_ERROR "hwloc library not found: ${HWLOC_LIBRARY}")
endif()

message(STATUS "hwloc include: ${HWLOC_INCLUDE_DIR}")
message(STATUS "hwloc library: ${HWLOC_LIBRARY}")

# ==================================================
# OpenSSL (REQUIRED, prebuilt — for TLS stratum)
# ==================================================
if(NOT OPENSSL_INCLUDE_DIR OR NOT OPENSSL_SSL_LIBRARY OR NOT OPENSSL_CRYPTO_LIBRARY)
    message(FATAL_ERROR
        "OpenSSL not configured.\n"
        "Pass -DOPENSSL_INCLUDE_DIR=... -DOPENSSL_SSL_LIBRARY=... -DOPENSSL_CRYPTO_LIBRARY=...")
endif()

if(NOT EXISTS "${OPENSSL_SSL_LIBRARY}")
    message(FATAL_ERROR "OpenSSL SSL library not found: ${OPENSSL_SSL_LIBRARY}")
endif()

if(NOT EXISTS "${OPENSSL_CRYPTO_LIBRARY}")
    message(FATAL_ERROR "OpenSSL crypto library not found: ${OPENSSL_CRYPTO_LIBRARY}")
endif()

message(STATUS "OpenSSL include: ${OPENSSL_INCLUDE_DIR}")
message(STATUS "OpenSSL ssl lib: ${OPENSSL_SSL_LIBRARY}")
message(STATUS "OpenSSL crypto:  ${OPENSSL_CRYPTO_LIBRARY}")

# ==================================================
# libuv (REQUIRED, prebuilt — used by argon2 impl-select)
# ==================================================
if(NOT UV_INCLUDE_DIR OR NOT UV_LIBRARY)
    message(FATAL_ERROR
        "libuv not configured.\n"
        "Pass -DUV_INCLUDE_DIR=... and -DUV_LIBRARY=...")
endif()

if(NOT EXISTS "${UV_LIBRARY}")
    message(FATAL_ERROR "libuv library not found: ${UV_LIBRARY}")
endif()

message(STATUS "libuv include:   ${UV_INCLUDE_DIR}")
message(STATUS "libuv library:   ${UV_LIBRARY}")

# ==================================================
# Link-Time Optimization
# ==================================================
if(WITH_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT LTO_SUPPORTED OUTPUT LTO_ERROR)
    if(LTO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
        message(STATUS "LTO enabled")
    else()
        message(WARNING "LTO not supported: ${LTO_ERROR}")
    endif()
endif()

# ==================================================
# Argon2 (SIMD-optimized, required by RandomX)
# ==================================================
add_subdirectory(src/crypto/argon2)

# ==================================================
# RandomX (embedded implementation)
# ==================================================
set(RANDOMX_SOURCES
    src/crypto/randomx/randomx.cpp
    src/crypto/randomx/dataset.cpp
    src/crypto/randomx/virtual_machine.cpp
    src/crypto/randomx/vm_compiled.cpp
    src/crypto/randomx/vm_compiled_light.cpp
    src/crypto/randomx/vm_interpreted.cpp
    src/crypto/randomx/vm_interpreted_light.cpp
    src/crypto/randomx/bytecode_machine.cpp
    src/crypto/randomx/instructions_portable.cpp
    src/crypto/randomx/superscalar.cpp
    src/crypto/randomx/blake2_generator.cpp
    src/crypto/randomx/aes_hash.cpp
    src/crypto/randomx/soft_aes.cpp
    src/crypto/randomx/allocator.cpp
    src/crypto/randomx/virtual_memory.cpp
    src/crypto/randomx/reciprocal.c
    src/crypto/randomx/blake2/blake2b.c
    src/crypto/randomx/blake2/blake2b_sse41.c
    src/crypto/randomx/blake2/avx2/blake2b_avx2.c
    src/crypto/randomx/jit_compiler_x86.cpp
    src/crypto/randomx/jit_compiler_x86_static.S
    # Panthera (rx/xla - Scala)
    src/crypto/randomx/panthera/sha256.c
    src/crypto/randomx/panthera/yespower-opt.c
    src/crypto/randomx/panthera/KangarooTwelve.c
    src/crypto/randomx/panthera/KeccakP-1600-reference.c
    src/crypto/randomx/panthera/KeccakSpongeWidth1600.c
    src/crypto/cpu_flags.c
    src/crypto/cpuType.cpp
)

if(ZENRX_OS_WIN)
    list(APPEND RANDOMX_SOURCES src/crypto/VirtualMemory_windows.cpp)
else()
    list(APPEND RANDOMX_SOURCES src/crypto/VirtualMemory_unix.cpp)
endif()

# Enable hardware AES and SSE4.1 intrinsics for RandomX sources
set_source_files_properties(${RANDOMX_SOURCES} PROPERTIES COMPILE_FLAGS "-maes -msse4.1")

# AVX2 blake2b needs its own flags (overrides the above for this file)
set_source_files_properties(
    src/crypto/randomx/blake2/avx2/blake2b_avx2.c
    PROPERTIES COMPILE_FLAGS "-O3 -mavx2"
)

# ==================================================
# Sources
# ==================================================
set(SOURCES
    src/main.cpp
    src/App.cpp
    src/Config.cpp
    src/Miner.cpp
    src/Worker.cpp
    src/CpuInfo.cpp
    src/Msr.cpp
    src/Api.cpp
    src/MoBenchmark.cpp
    src/Client.cpp
    src/Job.cpp
    src/crypto/RandomX.cpp
    src/crypto/RxInstance.cpp
    ${RANDOMX_SOURCES}
)

set(HEADERS
    src/App.h
    src/Config.h
    src/Log.h
    src/Miner.h
    src/Worker.h
    src/CpuInfo.h
    src/Msr.h
    src/Api.h
    src/MoBenchmark.h
    src/Platform.h
    src/version.h
    src/Client.h
    src/Job.h
    src/crypto/RandomX.h
    src/crypto/RxInstance.h
)

# ==================================================
# Windows icon/version resource
# ==================================================
if(ZENRX_OS_WIN AND WITH_ICON)
    enable_language(RC)
    set(WIN_RESOURCES res/app.rc)
    set_source_files_properties(res/app.rc PROPERTIES
        COMPILE_FLAGS "-I${CMAKE_SOURCE_DIR}/res/include"
    )
endif()

# ==================================================
# Executable
# ==================================================
add_executable(zenrx ${SOURCES} ${HEADERS} ${WIN_RESOURCES})

target_include_directories(zenrx PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/crypto/argon2/include
    ${CMAKE_SOURCE_DIR}/src/crypto/argon2/lib
    ${CMAKE_SOURCE_DIR}/src/crypto/randomx/blake2/avx2
    ${HWLOC_INCLUDE_DIR}
    ${OPENSSL_INCLUDE_DIR}
    ${UV_INCLUDE_DIR}
)

target_link_libraries(zenrx PRIVATE
    argon2
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${HWLOC_LIBRARY}
    ${UV_LIBRARY}
    Threads::Threads
)

# ==================================================
# Windows extras
# ==================================================
if(ZENRX_OS_WIN)
    target_link_libraries(zenrx PRIVATE
        ws2_32 psapi iphlpapi userenv advapi32 crypt32 bcrypt dbghelp
    )
elseif(ZENRX_OS_LINUX)
    target_link_libraries(zenrx PRIVATE dl)
endif()

# ==================================================
# Output
# ==================================================
set_target_properties(zenrx PROPERTIES
    OUTPUT_NAME "zenrx"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

install(TARGETS zenrx RUNTIME DESTINATION bin)

# ==================================================
# Summary
# ==================================================
message(STATUS "")
message(STATUS "ZenRX configuration:")
message(STATUS "  OS:        ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Compiler:  ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "  Build:     ${CMAKE_BUILD_TYPE}")
message(STATUS "  Static:    ${BUILD_STATIC}")
message(STATUS "  RandomX:   Embedded")
message(STATUS "  Argon2:    Embedded")
message(STATUS "  hwloc:     PreBuilt")
message(STATUS "  OpenSSL:   PreBuilt")
message(STATUS "  libuv:     PreBuilt")
message(STATUS "  MSR:       ${WITH_MSR}")
message(STATUS "  Native CPU: ${WITH_NATIVE_CPU}")
message(STATUS "  LTO:       ${WITH_LTO}")
message(STATUS "")
