# NFSUG1VITA - Need for Speed Underground (static recompilation) -> PS Vita. ELF -> SELF -> VPK.
cmake_minimum_required(VERSION 3.16)

option(NFSU_RUNTIME_CHECK "Only compile the runtime sources that build without the generated code (CI)" OFF)
# gcc: VitaSDK's compiler. clang: clang and lld (cmake/vita-clang.cmake, LLVM from NFSU_LLVM), ThinLTO, PGO.
set(NFSU_COMPILER "gcc" CACHE STRING "Compiler: gcc (VitaSDK) or clang (LLVM)")
set_property(CACHE NFSU_COMPILER PROPERTY STRINGS gcc clang)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(NFSU_COMPILER STREQUAL "clang")
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/cmake/vita-clang.cmake" CACHE PATH "toolchain file")
  elseif(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Define the VITASDK environment variable (point at your VitaSDK root).")
  endif()
endif()

project(NFSUG1VITA C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# GNU mode keeps newlib's POSIX declarations (ftruncate, strcasecmp) visible.
set(CMAKE_CXX_EXTENSIONS ON)

include("${VITASDK}/share/vita.cmake" REQUIRED)

if(NFSU_COMPILER STREQUAL "clang")
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "NFSU_COMPILER=clang needs the toolchain file cmake/vita-clang.cmake")
  endif()
  set(NFSU_CLANG ON)
elseif(NFSU_COMPILER STREQUAL "gcc")
  set(NFSU_CLANG OFF)
else()
  message(FATAL_ERROR "NFSU_COMPILER must be gcc or clang")
endif()

set(VITA_APP_NAME "NFSUG1VITA")
set(VITA_TITLEID  "NFSUG1VTA")
set(VITA_VERSION  "00.01")

option(NFSU_LOG_DEBUG        "Verbose runtime logging" OFF)
option(NFSU_ASSERT_SOFT      "Log failed assertions instead of trapping" OFF)

include(cmake/shaders.cmake)
include(cmake/sources.cmake)

nfsu_add_shaders(NFSU_SHADER_SOURCES)

if(NFSU_CLANG)
  set(NFSU_ARCH_FLAGS -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard)
else()
  set(NFSU_ARCH_FLAGS -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard)
endif()
set(NFSU_COMMON_FLAGS
  ${NFSU_ARCH_FLAGS}
  -fno-exceptions -fno-asynchronous-unwind-tables -fno-unwind-tables
  -ffunction-sections -fdata-sections
  -Wall -Wextra
  # __FILE__ in assertions and logs: paths relative to the repository, nothing from the build machine.
  -ffile-prefix-map=${CMAKE_SOURCE_DIR}/=
)
# Generated code only sees the stable defines; debug switches stay in the runtime to avoid full rebuilds.
  # Math functions do not set errno: sqrt is a single vsqrt.f64.
  -fno-math-errno
set(NFSU_DEFINES VITA __vita__ WITH_MMX)
set(NFSU_RUNTIME_DEFINES ${NFSU_DEFINES})
if(NFSU_LOG_DEBUG)
  list(APPEND NFSU_RUNTIME_DEFINES NFSU_LOG_DEBUG)
endif()
if(NFSU_ASSERT_SOFT)
  list(APPEND NFSU_RUNTIME_DEFINES NFSU_ASSERT_SOFT)
endif()

set(NFSU_GENERATED_DIR "${CMAKE_SOURCE_DIR}/src/nfsu/generated" CACHE PATH "Generated game code (tools/generate.ps1)")
set(NFSU_INCLUDES
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src/nfsu
  ${NFSU_GENERATED_DIR}
)

# Compiles the runtime sources that do not need the generated game code; no VPK (CI without game binaries).
if(NFSU_RUNTIME_CHECK)
  add_library(nfsu_runtime_check OBJECT ${NFSU_STANDALONE_SOURCES} ${NFSU_SHADER_SOURCES})
  target_compile_options(nfsu_runtime_check PRIVATE ${NFSU_COMMON_FLAGS} -O3 -Werror)
  target_compile_definitions(nfsu_runtime_check PRIVATE ${NFSU_RUNTIME_DEFINES})
  target_include_directories(nfsu_runtime_check PRIVATE ${NFSU_INCLUDES})
  return()
endif()

file(GLOB NFSU_GENERATED_SOURCES CONFIGURE_DEPENDS "${NFSU_GENERATED_DIR}/*.cpp")
if(NOT NFSU_GENERATED_SOURCES)
  message(FATAL_ERROR "${NFSU_GENERATED_DIR} is empty; run tools/generate.ps1 first.")
endif()

if(NFSU_CLANG)
  include(cmake/clang.cmake)
  set(NFSU_NO_SLP -fno-slp-vectorize)
else()
  set(NFSU_NO_SLP -fno-tree-slp-vectorize)
endif()

# Generated game code: large, optimise for size and compile time.
add_library(nfsu_game OBJECT ${NFSU_GENERATED_SOURCES})
# No SLP vectorisation: it packs guest stores into NEON stores through the stack (store forwarding stalls).
target_compile_options(nfsu_game PRIVATE ${NFSU_COMMON_FLAGS} -O2 ${NFSU_NO_SLP} -Wno-unused-label -Wno-unused-variable -Wno-unused-parameter ${NFSU_CLANG_GAME_FLAGS})
# Start-up method registration (nfsu.cpp, runs once) gains nothing from -O2 and compiles much faster at -Og.
set(NFSU_LIGHT_SOURCES ${NFSU_GENERATED_SOURCES})
list(FILTER NFSU_LIGHT_SOURCES INCLUDE REGEX "/nfsu\\.cpp$")
set_source_files_properties(${NFSU_LIGHT_SOURCES} PROPERTIES COMPILE_OPTIONS -Og)
if(NFSU_CLANG)
  set_property(SOURCE ${NFSU_LIGHT_SOURCES} APPEND PROPERTY COMPILE_OPTIONS ${NFSU_CLANG_LIGHT_FLAGS})
endif()
target_compile_definitions(nfsu_game PRIVATE ${NFSU_DEFINES})
target_include_directories(nfsu_game PRIVATE ${NFSU_INCLUDES})

# Runtime and Vita backend.
add_library(nfsu_runtime OBJECT ${NFSU_RUNTIME_SOURCES} ${NFSU_SHADER_SOURCES})
target_compile_options(nfsu_runtime PRIVATE ${NFSU_COMMON_FLAGS} -O3 ${NFSU_CLANG_FLAGS})
target_compile_definitions(nfsu_runtime PRIVATE ${NFSU_RUNTIME_DEFINES})
target_include_directories(nfsu_runtime PRIVATE ${NFSU_INCLUDES})

add_executable(NFSUG1VITA.elf src/nfsu/main.cpp ${NFSU_CLANG_SOURCES} $<TARGET_OBJECTS:nfsu_game> $<TARGET_OBJECTS:nfsu_runtime>)
target_compile_options(NFSUG1VITA.elf PRIVATE ${NFSU_COMMON_FLAGS} -O2 ${NFSU_CLANG_FLAGS})
target_compile_definitions(NFSUG1VITA.elf PRIVATE ${NFSU_RUNTIME_DEFINES})
target_include_directories(NFSUG1VITA.elf PRIVATE ${NFSU_INCLUDES})
# Nothing references the main thread's stack size; keep it from being collected so the loader sees it.
target_link_options(NFSUG1VITA.elf PRIVATE -Wl,--gc-sections -Wl,--undefined=sceUserMainThreadStackSize)
if(NFSU_CLANG)
  target_link_options(NFSUG1VITA.elf PRIVATE ${NFSU_CLANG_LINK_FLAGS})
  target_link_libraries(NFSUG1VITA.elf ${NFSU_CLANG_LIBRARIES})
  set_target_properties(NFSUG1VITA.elf PROPERTIES LINK_DEPENDS "${NFSU_LLD_SCRIPT}")
endif()
target_link_libraries(NFSUG1VITA.elf
  stdc++
  SceGxm_stub
  SceDisplay_stub
  SceCtrl_stub
  SceTouch_stub
  SceCommonDialog_stub
  SceAppUtil_stub
  SceAudio_stub
  ScePower_stub
  SceRtc_stub
  SceLibKernel_stub
  SceKernelThreadMgr_stub
  SceSysmem_stub
  SceProcessmgr_stub
  c
  m
)

vita_create_self(NFSUG1VITA.self NFSUG1VITA.elf UNSAFE)

# ATTRIBUTE2=12: extended memory budget for the guest arena.
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")
configure_file(
  ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/template.xml.in
  ${CMAKE_BINARY_DIR}/generated/livearea/template.xml
  @ONLY)

vita_create_vpk(NFSUG1VITA.vpk ${VITA_TITLEID} NFSUG1VITA.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE ${CMAKE_BINARY_DIR}/generated/livearea/template.xml sce_sys/livearea/contents/template.xml
)
