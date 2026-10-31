# The clang build (NFSU_COMPILER=clang): lld linker script, ThinLTO and profile-guided optimisation.
# Sets NFSU_CLANG_FLAGS (runtime, main), NFSU_CLANG_GAME_FLAGS (generated code), NFSU_CLANG_LIGHT_FLAGS (nfsu.cpp),
# NFSU_CLANG_SOURCES, NFSU_CLANG_LINK_FLAGS and NFSU_LLD_SCRIPT.
#   NFSU_LTO=ON            ThinLTO over the generated code, the runtime and main.cpp (default)
#   NFSU_PGO_GENERATE=ON   instrumented build: the game writes ux0:data/NFSUG1VITA/nfsu.profraw
#   NFSU_PGO_LIST=<file>   with NFSU_PGO_GENERATE: instrument only the functions and sources the file selects
#                          (clang's -fprofile-list format); empty: everything
#   NFSU_PGO_USE=<file>    optimise with an indexed profile (llvm-profdata merge)

option(NFSU_LTO "ThinLTO (clang)" ON)
set(NFSU_PGO_LIST "" CACHE FILEPATH "Functions and sources to instrument (-fprofile-list); empty: everything")

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
  if(NFSU_PGO_LIST)
    if(NOT EXISTS "${NFSU_PGO_LIST}")
      message(FATAL_ERROR "NFSU_PGO_LIST: ${NFSU_PGO_LIST} not found")
    endif()
    list(APPEND NFSU_PGO_FLAGS "-fprofile-list=${NFSU_PGO_LIST}")
    # Sources are compiled again when the selection changes.
    set_property(SOURCE ${NFSU_GENERATED_SOURCES} ${NFSU_RUNTIME_SOURCES} src/nfsu/main.cpp
                 APPEND PROPERTY OBJECT_DEPENDS "${NFSU_PGO_LIST}")
  endif()
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

message(STATUS "clang build: ThinLTO ${NFSU_LTO}, PGO generate ${NFSU_PGO_GENERATE} (list '${NFSU_PGO_LIST}'), "
               "PGO use '${NFSU_PGO_USE}'")
