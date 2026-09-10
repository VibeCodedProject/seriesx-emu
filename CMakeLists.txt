cmake_minimum_required(VERSION 3.20)
project(seriesx-emu VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

# Core static lib: memory map + native cpu + gpu stub + vulkan heap query
# Prefer a real Vulkan SDK install; fall back to vendored headers and the
# versioned loader (libvulkan.so.1) when only the runtime lib is present.
find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
  find_path(Vulkan_INCLUDE_DIR vulkan/vulkan.h
    PATHS ${CMAKE_CURRENT_SOURCE_DIR}/vendor/vulkan-headers/include
    NO_DEFAULT_PATH)
  if(NOT Vulkan_INCLUDE_DIR)
    find_path(Vulkan_INCLUDE_DIR vulkan/vulkan.h)
  endif()
  find_library(Vulkan_LIBRARY NAMES vulkan)
  if(NOT Vulkan_LIBRARY)
    # Only the versioned runtime is installed (no -dev symlink). CMake's
    # find_library cannot match "libvulkan.so.1" name mangling, so glob it.
    file(GLOB _vk_loader
         /lib/x86_64-linux-gnu/libvulkan.so*
         /usr/lib/x86_64-linux-gnu/libvulkan.so*
         /usr/lib/libvulkan.so*
         /lib/libvulkan.so*)
    if(_vk_loader)
      list(GET _vk_loader 0 _vk0)
      set(Vulkan_LIBRARY "${_vk0}" CACHE FILEPATH "Vulkan loader (versioned)" FORCE)
    endif()
  endif()
  if(NOT Vulkan_INCLUDE_DIR OR NOT Vulkan_LIBRARY)
    message(FATAL_ERROR "Vulkan headers+loader not found (see vendor/)")
  endif()
  message(STATUS "Vulkan fallback: ${Vulkan_LIBRARY} / ${Vulkan_INCLUDE_DIR}")
  add_library(Vulkan_headers INTERFACE)
  target_include_directories(Vulkan_headers INTERFACE ${Vulkan_INCLUDE_DIR})
  add_library(Vulkan::Vulkan ALIAS Vulkan_headers)
  target_link_libraries(Vulkan_headers INTERFACE ${Vulkan_LIBRARY})
endif()
add_library(seriesx_core STATIC
  src/mem_map.cpp
  src/cpu_native.cpp
  src/cpu_sched.cpp
  src/xbe_loader.cpp
  src/fiber_tls.cpp
  src/xbox_krnl.cpp
  src/gpu_stub.cpp
  src/xbox_vs.cpp
  src/xbox_rc.cpp
  src/vulkan_heaps.cpp
  src/vulkan_alloc.cpp
  src/vulkan_buffer.cpp
  src/vulkan_image.cpp
  src/xbox_vfs.cpp
  src/io_queue.cpp
  src/gpu_batch.cpp
  src/xbox_sys.cpp
  src/xbox_input.cpp
  src/xbox_audio.cpp
  src/xbox_display.cpp
  src/vulkan_present.cpp
)
target_include_directories(seriesx_core PUBLIC include)
target_link_libraries(seriesx_core PUBLIC Vulkan::Vulkan)

# ---------------------------------------------------------------------------
# Optional display window (GLFW + Vulkan swapchain). Like Unicorn: when GLFW
# is absent the presenter compiles as a stub whose init() returns false.
# ---------------------------------------------------------------------------
find_package(glfw3 QUIET)
if(glfw3_FOUND)
  message(STATUS "Display window: glfw3 found, swapchain presenter enabled")
  target_compile_definitions(seriesx_core PUBLIC DISPLAY_GLFW_ENABLED)
  target_link_libraries(seriesx_core PUBLIC glfw)
else()
  message(STATUS "Display window: glfw3 NOT found, presenter is a stub")
endif()

# ---------------------------------------------------------------------------
# Optional Unicorn x86-32 CPU core (guest execution). Unicorn is GPL-2.0
# (QEMU-derived), so it stays an OPTIONAL dependency: when it is not found,
# everything else still builds and the xbox_cpu tests are simply absent.
# Vendored headers live in vendor/unicorn/include; the engine library is
# looked up in the system paths plus the local Unicorn wheel install.
# ---------------------------------------------------------------------------
find_path(UNICORN_INCLUDE_DIR unicorn/unicorn.h
  PATHS ${CMAKE_CURRENT_SOURCE_DIR}/vendor/unicorn/include
  NO_DEFAULT_PATH)
if(NOT UNICORN_INCLUDE_DIR)
  find_path(UNICORN_INCLUDE_DIR unicorn/unicorn.h)
endif()
# The library is searched in the system paths and in a local Unicorn
# python-wheel install (the engine ships libunicorn inside it). Override
# both with -DUNICORN_INCLUDE_DIR=... -DUNICORN_LIBRARY=... .
file(GLOB _uc_wheel_libs
  $ENV{HOME}/.local/lib/python*/site-packages/unicorn/lib/libunicorn*
  /usr/local/lib/libunicorn*
  /usr/lib/x86_64-linux-gnu/libunicorn*
  /usr/lib/libunicorn*)
if(_uc_wheel_libs)
  list(GET _uc_wheel_libs 0 _uc0)
  get_filename_component(_uc_hint "${_uc0}" DIRECTORY)
endif()
find_library(UNICORN_LIBRARY NAMES libunicorn.so.2 unicorn
  HINTS ${_uc_hint})
if(UNICORN_LIBRARY AND UNICORN_INCLUDE_DIR)
  message(STATUS "Unicorn CPU core: ${UNICORN_LIBRARY} / ${UNICORN_INCLUDE_DIR}")
  target_sources(seriesx_core PRIVATE src/xbox_cpu.cpp)
  target_include_directories(seriesx_core PUBLIC ${UNICORN_INCLUDE_DIR})
  target_link_libraries(seriesx_core PUBLIC ${UNICORN_LIBRARY})
  get_filename_component(UNICORN_LIBDIR "${UNICORN_LIBRARY}" DIRECTORY)
  set_property(TARGET seriesx_core PROPERTY BUILD_RPATH "${UNICORN_LIBDIR}")
  set(UNICORN_ENABLED ON)
else()
  message(STATUS "Unicorn not found: guest execution (xbox_cpu) disabled")
  set(UNICORN_ENABLED OFF)
endif()

add_executable(seriesx-emu src/main.cpp)
target_link_libraries(seriesx-emu PRIVATE seriesx_core)
if(UNICORN_ENABLED)
  target_compile_definitions(seriesx-emu PRIVATE UNICORN_ENABLED)
endif()

# Minimal test without external deps
add_executable(test_mem tests/test_mem.cpp)
target_link_libraries(test_mem PRIVATE seriesx_core)

add_executable(test_cpu tests/test_cpu.cpp)
target_link_libraries(test_cpu PRIVATE seriesx_core)

add_executable(test_cpu_sched tests/test_cpu_sched.cpp)
target_link_libraries(test_cpu_sched PRIVATE seriesx_core)

add_executable(test_xbe_loader tests/test_xbe_loader.cpp)
target_link_libraries(test_xbe_loader PRIVATE seriesx_core)
target_compile_definitions(test_xbe_loader PRIVATE
  XBE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/lithiumx.xbe")

add_executable(test_fiber_tls tests/test_fiber_tls.cpp)
target_link_libraries(test_fiber_tls PRIVATE seriesx_core)

add_executable(test_xbox_krnl tests/test_xbox_krnl.cpp)
target_link_libraries(test_xbox_krnl PRIVATE seriesx_core)
target_compile_definitions(test_xbox_krnl PRIVATE
  XBE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/lithiumx.xbe")

if(UNICORN_ENABLED)
  add_executable(test_xbox_cpu tests/test_xbox_cpu.cpp)
  target_link_libraries(test_xbox_cpu PRIVATE seriesx_core)
  target_compile_definitions(test_xbox_cpu PRIVATE
    XBE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/lithiumx.xbe")
endif()

add_executable(test_vulkan tests/test_vulkan_heaps.cpp)
target_link_libraries(test_vulkan PRIVATE seriesx_core)

add_executable(test_vulkan_alloc tests/test_vulkan_alloc.cpp)
target_link_libraries(test_vulkan_alloc PRIVATE seriesx_core)

add_executable(test_vulkan_buffer tests/test_vulkan_buffer.cpp)
target_link_libraries(test_vulkan_buffer PRIVATE seriesx_core)

add_executable(test_vulkan_image tests/test_vulkan_image.cpp)
target_link_libraries(test_vulkan_image PRIVATE seriesx_core)

add_executable(test_vfs_io tests/test_vfs_io.cpp)
target_link_libraries(test_vfs_io PRIVATE seriesx_core)

add_executable(test_gpu_batch tests/test_gpu_batch.cpp)
target_link_libraries(test_gpu_batch PRIVATE seriesx_core)

add_executable(test_xbox_gpu tests/test_xbox_gpu.cpp)
target_link_libraries(test_xbox_gpu PRIVATE seriesx_core)

add_executable(test_xbox_sys tests/test_xbox_sys.cpp)
target_link_libraries(test_xbox_sys PRIVATE seriesx_core)

add_executable(test_xbox_shaders tests/test_xbox_shaders.cpp)
target_link_libraries(test_xbox_shaders PRIVATE seriesx_core)

add_executable(test_xbox_display tests/test_xbox_display.cpp)
target_link_libraries(test_xbox_display PRIVATE seriesx_core)

add_executable(test_vulkan_present tests/test_vulkan_present.cpp)
target_link_libraries(test_vulkan_present PRIVATE seriesx_core)

enable_testing()
add_test(NAME mem_map COMMAND test_mem)
add_test(NAME cpu_hle COMMAND test_cpu)
add_test(NAME cpu_sched COMMAND test_cpu_sched)
add_test(NAME xbe_loader COMMAND test_xbe_loader)
add_test(NAME fiber_tls COMMAND test_fiber_tls)
add_test(NAME xbox_krnl COMMAND test_xbox_krnl)
if(UNICORN_ENABLED)
  add_test(NAME xbox_cpu COMMAND test_xbox_cpu)
endif()
add_test(NAME vulkan_heaps COMMAND test_vulkan)
add_test(NAME vulkan_alloc COMMAND test_vulkan_alloc)
add_test(NAME vulkan_buffer COMMAND test_vulkan_buffer)
add_test(NAME vulkan_image COMMAND test_vulkan_image)
add_test(NAME vfs_io COMMAND test_vfs_io)
add_test(NAME gpu_batch COMMAND test_gpu_batch)
add_test(NAME xbox_gpu COMMAND test_xbox_gpu)
add_test(NAME xbox_sys COMMAND test_xbox_sys)
add_test(NAME xbox_shaders COMMAND test_xbox_shaders)
add_test(NAME xbox_display COMMAND test_xbox_display)
add_test(NAME vulkan_present COMMAND test_vulkan_present)

# Every test must finish in 60s (catches condvar/scheduler deadlocks).
# GPU-dependent tests exit 77 when no Vulkan device exists -> "Skipped".
# ASan/UBSan/LSan slow Unicorn TCG execution down ~10x (xbox_cpu runs ~70s
# sanitized vs ~7s native), so sanitizer builds get a roomier deadline.
set(_all_tests mem_map cpu_hle cpu_sched xbe_loader fiber_tls xbox_krnl
               vulkan_heaps vulkan_alloc vulkan_buffer vulkan_image vfs_io
               gpu_batch xbox_gpu xbox_sys xbox_display vulkan_present)
if(UNICORN_ENABLED)
  list(APPEND _all_tests xbox_cpu)
endif()
set(_test_timeout 60)
if(CMAKE_CXX_FLAGS MATCHES "fsanitize")
  set(_test_timeout 300)
endif()
foreach(t ${_all_tests})
  set_tests_properties(${t} PROPERTIES TIMEOUT ${_test_timeout}
                                       SKIP_RETURN_CODE 77)
endforeach()
