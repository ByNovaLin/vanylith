cmake_minimum_required(VERSION 3.24)

project(Vanylith VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Vanylith v0.1 supports Windows 10/11 only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# CUDA remains optional so a clean CPU-only build is still possible on hosts
# without a toolkit. The Windows CUDA package carries native code for every
# NVIDIA architecture in the documented v0.1 compatibility set, plus PTX for
# the newest architecture as a forward-JIT fallback.
option(VANITYFORGE_ENABLE_CUDA "Build the CUDA vanity-search backend" ON)
set(VANITYFORGE_CUDA_ARCHITECTURES "80-real;86-real;89-real;120" CACHE STRING
    "Semicolon-separated CUDA SM architectures for the vanity-search backend")
set(VANITYFORGE_CUDA_GROUP_THREADS "32" CACHE STRING
    "CUDA point-progression and inversion threads per block (A/B: 32 or 64)")
set_property(CACHE VANITYFORGE_CUDA_GROUP_THREADS PROPERTY STRINGS 32 64)
set(VANITYFORGE_CUDA_CANDIDATES_PER_THREAD "64" CACHE STRING
    "CUDA inversion-group width (A/B: 32, 64, or 128)")
set_property(CACHE VANITYFORGE_CUDA_CANDIDATES_PER_THREAD PROPERTY STRINGS 32 64 128)
if(NOT VANITYFORGE_CUDA_GROUP_THREADS MATCHES "^(32|64)$")
  message(FATAL_ERROR "VANITYFORGE_CUDA_GROUP_THREADS must be 32 or 64")
endif()
if(NOT VANITYFORGE_CUDA_CANDIDATES_PER_THREAD MATCHES "^(32|64|128)$")
  message(FATAL_ERROR "VANITYFORGE_CUDA_CANDIDATES_PER_THREAD must be 32, 64, or 128")
endif()
set(VANITYFORGE_HAS_CUDA OFF)
if(VANITYFORGE_ENABLE_CUDA)
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    find_package(CUDAToolkit 13.3 REQUIRED)
    if(CUDAToolkit_VERSION VERSION_LESS 13.3 OR
       NOT CUDAToolkit_VERSION VERSION_LESS 13.4)
      message(FATAL_ERROR
        "The CUDA RC build is pinned to CUDA Toolkit 13.3.x; found ${CUDAToolkit_VERSION}")
    endif()
    set(VANITYFORGE_HAS_CUDA ON)
    message(STATUS "Vanylith CUDA backend enabled with ${CMAKE_CUDA_COMPILER}")
  else()
    message(STATUS "CUDA compiler unavailable; building Vanylith without CUDA backend")
  endif()
endif()

include(FetchContent)
set(SECP256K1_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXHAUSTIVE_TESTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_CTIME_TESTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_ECDH OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_RECOVERY OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_EXTRAKEYS OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_SCHNORRSIG OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_MUSIG OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_ELLSWIFT OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_SILENTPAYMENTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  secp256k1
  GIT_REPOSITORY https://github.com/bitcoin-core/secp256k1.git
  GIT_TAG 18f07c42218765cd46148d74d9fe575795f56dce
  GIT_SHALLOW FALSE
  GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(secp256k1)

# The native WebView2 SDK provides headers and the static loader. The loader is
# linked into Vanylith.exe; the browser runtime itself remains the Microsoft
# Evergreen WebView2 Runtime installed on Windows.
set(VANITYFORGE_WEBVIEW2_VERSION "1.0.4191.47")
set(VANITYFORGE_WEBVIEW2_SDK_DIR "" CACHE PATH "Optional pre-extracted WebView2 SDK directory")
if(VANITYFORGE_WEBVIEW2_SDK_DIR)
  set(webview2_sdk_SOURCE_DIR "${VANITYFORGE_WEBVIEW2_SDK_DIR}")
else()
  FetchContent_Declare(
    webview2_sdk
    URL "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/${VANITYFORGE_WEBVIEW2_VERSION}/microsoft.web.webview2.${VANITYFORGE_WEBVIEW2_VERSION}.nupkg"
    URL_HASH SHA256=f492bbf547d0da329553b6727435b677579b1e9f91cc9e4a1ad029366d5f23d0
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(webview2_sdk)
endif()

if(CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64")
  set(VANITYFORGE_WEBVIEW2_ARCH arm64)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(VANITYFORGE_WEBVIEW2_ARCH x64)
else()
  set(VANITYFORGE_WEBVIEW2_ARCH x86)
endif()
add_library(webview2_loader STATIC IMPORTED GLOBAL)
set_target_properties(webview2_loader PROPERTIES
  IMPORTED_LOCATION "${webview2_sdk_SOURCE_DIR}/build/native/${VANITYFORGE_WEBVIEW2_ARCH}/WebView2LoaderStatic.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${webview2_sdk_SOURCE_DIR}/build/native/include"
)

add_library(vanityforge_core STATIC
  src/backend/backend_registry.cpp
  src/backend/cpu/cpu_backend.cpp
  src/controller/result_manager.cpp
  src/controller/task_manager.cpp
  src/core/base58.cpp
  src/core/keccak256.cpp
  src/core/pattern.cpp
  src/core/probability.cpp
  src/core/search_space.cpp
  src/core/secure_memory.cpp
  src/core/secure_random.cpp
  src/core/sha256.cpp
  src/core/tron_address.cpp
  src/gpu/nvml_monitor.cpp
  src/web/api.cpp
)

target_include_directories(vanityforge_core PUBLIC src)
target_compile_definitions(vanityforge_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX SECP256K1_STATIC)
target_link_libraries(vanityforge_core PUBLIC secp256k1 bcrypt advapi32)

if(MSVC)
  # Do not pass raw MSVC warning/security options to nvcc. CUDA sources carry
  # their host compiler options explicitly below.
  target_compile_options(vanityforge_core PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:/W4>
    $<$<COMPILE_LANGUAGE:CXX>:/WX>
    $<$<COMPILE_LANGUAGE:CXX>:/permissive->
    $<$<COMPILE_LANGUAGE:CXX>:/sdl>
    $<$<COMPILE_LANGUAGE:CXX>:/guard:cf>
    $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
  target_link_options(vanityforge_core PUBLIC /guard:cf)
endif()

if(VANITYFORGE_HAS_CUDA)
  target_sources(vanityforge_core PRIVATE
    src/backend/cuda/cuda_backend.cu
  )
  target_compile_definitions(vanityforge_core PUBLIC VANITYFORGE_ENABLE_CUDA=1)
  target_compile_definitions(vanityforge_core PRIVATE
    VANITYFORGE_MIN_CUDA_DRIVER_API=13000
    $<$<COMPILE_LANGUAGE:CUDA>:VANITYFORGE_CUDA_GROUP_THREADS=${VANITYFORGE_CUDA_GROUP_THREADS}>
    $<$<COMPILE_LANGUAGE:CUDA>:VANITYFORGE_CUDA_CANDIDATES_PER_THREAD=${VANITYFORGE_CUDA_CANDIDATES_PER_THREAD}>)
  target_link_libraries(vanityforge_core PUBLIC CUDA::cudart_static)
  set_target_properties(vanityforge_core PROPERTIES
    CUDA_STANDARD 20
    CUDA_STANDARD_REQUIRED ON
    CUDA_EXTENSIONS OFF
    CUDA_ARCHITECTURES "${VANITYFORGE_CUDA_ARCHITECTURES}"
    CUDA_RUNTIME_LIBRARY Static
    CUDA_SEPARABLE_COMPILATION OFF)
  target_compile_options(vanityforge_core PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=/utf-8>
    $<$<COMPILE_LANGUAGE:CUDA>:--threads=0>)
endif()

add_executable(Vanylith WIN32
  src/main.cpp
  src/desktop/desktop_app.cpp
  src/desktop/single_instance.cpp
  src/desktop/webview_host.cpp
  src/web/assets.rc
  src/web/local_server.cpp
)
target_link_libraries(Vanylith PRIVATE
  vanityforge_core
  webview2_loader
  ws2_32
  shell32
  ole32
  version
)
set_target_properties(Vanylith PROPERTIES OUTPUT_NAME "Vanylith")
if(MSVC)
  target_compile_options(Vanylith PRIVATE /W4 /WX /permissive- /sdl /guard:cf /utf-8)
  target_link_options(Vanylith PRIVATE /guard:cf)
endif()

option(VANITYFORGE_BUILD_TESTS "Build Vanylith tests" ON)
if(VANITYFORGE_BUILD_TESTS)
  enable_testing()
  add_executable(vanityforge_tests
    tests/test_main.cpp
    tests/test_base58.cpp
    tests/test_cpu_search.cpp
    tests/test_keccak.cpp
    tests/test_nvml.cpp
    tests/test_pattern.cpp
    tests/test_probability.cpp
    tests/test_search_ranges.cpp
    tests/test_device_policy.cpp
    tests/test_sha256.cpp
    tests/test_tron_address.cpp
    tests/test_web_api.cpp
  )
  if(VANITYFORGE_HAS_CUDA)
    target_sources(vanityforge_tests PRIVATE tests/test_cuda.cpp)
  endif()
  target_link_libraries(vanityforge_tests PRIVATE vanityforge_core)
  target_compile_definitions(vanityforge_tests PRIVATE
    VANITYFORGE_TEST_DATA_DIR="${CMAKE_BINARY_DIR}/test-data")
  if(MSVC)
    target_compile_options(vanityforge_tests PRIVATE /W4 /WX /permissive- /sdl /guard:cf /utf-8)
  endif()
  add_test(NAME vanityforge_core_tests COMMAND vanityforge_tests)
endif()
