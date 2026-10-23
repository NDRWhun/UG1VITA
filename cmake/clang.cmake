# The clang build (NFSU_COMPILER=clang): lld linker script, ThinLTO and profile-guided optimisation.
# Sets NFSU_CLANG_FLAGS (runtime, main), NFSU_CLANG_GAME_FLAGS (generated code), NFSU_CLANG_LIGHT_FLAGS (nfsu.cpp),
# NFSU_CLANG_SOURCES, NFSU_CLANG_LINK_FLAGS and NFSU_LLD_SCRIPT.
#   NFSU_LTO=ON            ThinLTO over the generated code, the runtime and main.cpp (default)
#   NFSU_PGO_GENERATE=ON   instrumented build: the game writes ux0:data/NFSUG1VITA/nfsu.profraw
#   NFSU_PGO_USE=<file>    optimise with an indexed profile (llvm-profdata merge)

option(NFSU_LTO "ThinLTO (clang)" ON)
option(NFSU_PGO_GENERATE "Instrument for a PGO profile (clang)" OFF)
set(NFSU_PGO_USE "" CACHE FILEPATH "Indexed PGO profile to optimise with (clang)")
if(NFSU_PGO_GENERATE AND NFSU_PGO_USE)
  message(FATAL_ERROR "NFSU_PGO_GENERATE and NFSU_PGO_USE exclude each other")
endif()

include(cmake/vita-lld.cmake)
nfsu_lld_script(NFSU_LLD_SCRIPT)

set(NFSU_CLANG_LINK_FLAGS "-Wl,-T,${NFSU_LLD_SCRIPT}")

# Generated code: warnings off.
set(NFSU_CLANG_GAME_FLAGS -Wno-unused-but-set-variable -Wno-unused-function -Wno-parentheses-equality -Wno-self-assign)
set(NFSU_CLANG_FLAGS "")
set(NFSU_CLANG_LIGHT_FLAGS "")
set(NFSU_CLANG_SOURCES "")
set(NFSU_CLANG_LIBRARIES "")

if(NFSU_LTO)
  list(APPEND NFSU_CLANG_FLAGS -flto=thin)
  list(APPEND NFSU_CLANG_GAME_FLAGS -flto=thin)
  # nfsu.cpp is compiled to native code.
  list(APPEND NFSU_CLANG_LIGHT_FLAGS -fno-lto)
  cmake_host_system_information(RESULT NFSU_LTO_JOBS QUERY NUMBER_OF_LOGICAL_CORES)
  # Link-time backends: -O2, one job per core, a cache in the build directory, no SLP vectorisation.
  list(APPEND NFSU_CLANG_LINK_FLAGS -flto=thin -Wl,--lto-O2 -Wl,--thinlto-jobs=${NFSU_LTO_JOBS}
       "-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/thinlto-cache" -Wl,-mllvm,-vectorize-slp=false)
endif()

if(NFSU_PGO_GENERATE)
  include(cmake/pgo-runtime.cmake)
  nfsu_pgo_runtime(NFSU_PGO_RUNTIME)
  # IR-level instrumentation with value profiles, atomic counter updates.
  set(NFSU_PGO_FLAGS -fprofile-generate -fprofile-update=prefer-atomic)
  list(APPEND NFSU_CLANG_FLAGS ${NFSU_PGO_FLAGS})
  list(APPEND NFSU_CLANG_GAME_FLAGS ${NFSU_PGO_FLAGS})
  list(APPEND NFSU_RUNTIME_DEFINES NFSU_PGO_GENERATE)
  list(APPEND NFSU_CLANG_SOURCES src/platform/pgo.cpp)
  # The profile runtime library (cmake/pgo-runtime.cmake).
  set(NFSU_CLANG_LIBRARIES ${NFSU_PGO_RUNTIME})
elseif(NFSU_PGO_USE)
  if(NOT EXISTS "${NFSU_PGO_USE}")
    message(FATAL_ERROR "NFSU_PGO_USE: ${NFSU_PGO_USE} not found")
  endif()
  # Profile mismatch warnings off.
  set(NFSU_PGO_FLAGS "-fprofile-use=${NFSU_PGO_USE}" -Wno-backend-plugin)
  list(APPEND NFSU_CLANG_FLAGS ${NFSU_PGO_FLAGS})
  list(APPEND NFSU_CLANG_GAME_FLAGS ${NFSU_PGO_FLAGS})
  # Sources compiled with the profile are rebuilt when it changes.
  set_property(SOURCE ${NFSU_GENERATED_SOURCES} ${NFSU_RUNTIME_SOURCES} src/nfsu/main.cpp
               APPEND PROPERTY OBJECT_DEPENDS "${NFSU_PGO_USE}")
endif()

message(STATUS "clang build: ThinLTO ${NFSU_LTO}, PGO generate ${NFSU_PGO_GENERATE}, PGO use '${NFSU_PGO_USE}'")
