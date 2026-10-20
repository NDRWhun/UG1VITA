# NFSUG1VITA - Need for Speed Underground (static recompilation) -> PS Vita. ELF -> SELF -> VPK.
cmake_minimum_required(VERSION 3.16)

# -DNFSU_HOST=ON: the Linux (x86-64) test harness instead of the Vita build (cmake/host.cmake).
option(NFSU_HOST "Build the host test harness" OFF)
option(NFSU_RUNTIME_CHECK "Only compile the runtime sources that build without the generated code (CI)" OFF)
if(NFSU_HOST)
  project(NFSUG1VITA_HOST C CXX)
  include(cmake/host.cmake)
  return()
endif()

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
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

set(VITA_APP_NAME "NFSUG1VITA")
set(VITA_TITLEID  "NFSUG1VTA")
set(VITA_VERSION  "00.01")

option(NFSU_LOG_DEBUG        "Verbose runtime logging" OFF)
option(NFSU_ASSERT_SOFT      "Log failed assertions instead of trapping" OFF)

include(cmake/shaders.cmake)
include(cmake/sources.cmake)

nfsu_add_shaders(NFSU_SHADER_SOURCES)

set(NFSU_COMMON_FLAGS
  -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard
  -fno-exceptions -fno-asynchronous-unwind-tables -fno-unwind-tables
  -ffunction-sections -fdata-sections
  -Wall -Wextra
  # __FILE__ in assertions and logs: paths relative to the repository, nothing from the build machine.
  -ffile-prefix-map=${CMAKE_SOURCE_DIR}/=
)
# Generated code only sees the stable defines; debug switches stay in the runtime to avoid full rebuilds.
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

# Generated game code: large, optimise for size and compile time.
add_library(nfsu_game OBJECT ${NFSU_GENERATED_SOURCES})
target_compile_options(nfsu_game PRIVATE ${NFSU_COMMON_FLAGS} -O2 -Wno-unused-label -Wno-unused-variable -Wno-unused-parameter)
# Start-up method registration (nfsu.cpp, runs once) gains nothing from -O2 and compiles much faster at -Og.
set(NFSU_LIGHT_SOURCES ${NFSU_GENERATED_SOURCES})
list(FILTER NFSU_LIGHT_SOURCES INCLUDE REGEX "/nfsu\\.cpp$")
set_source_files_properties(${NFSU_LIGHT_SOURCES} PROPERTIES COMPILE_OPTIONS -Og)
target_compile_definitions(nfsu_game PRIVATE ${NFSU_DEFINES})
target_include_directories(nfsu_game PRIVATE ${NFSU_INCLUDES})

# Runtime and Vita backend.
add_library(nfsu_runtime OBJECT ${NFSU_RUNTIME_SOURCES} ${NFSU_SHADER_SOURCES})
target_compile_options(nfsu_runtime PRIVATE ${NFSU_COMMON_FLAGS} -O3)
target_compile_definitions(nfsu_runtime PRIVATE ${NFSU_RUNTIME_DEFINES})
target_include_directories(nfsu_runtime PRIVATE ${NFSU_INCLUDES})

add_executable(NFSUG1VITA.elf src/nfsu/main.cpp $<TARGET_OBJECTS:nfsu_game> $<TARGET_OBJECTS:nfsu_runtime>)
target_compile_options(NFSUG1VITA.elf PRIVATE ${NFSU_COMMON_FLAGS} -O2)
target_compile_definitions(NFSUG1VITA.elf PRIVATE ${NFSU_RUNTIME_DEFINES})
target_include_directories(NFSUG1VITA.elf PRIVATE ${NFSU_INCLUDES})
target_link_options(NFSUG1VITA.elf PRIVATE -Wl,--gc-sections)
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
