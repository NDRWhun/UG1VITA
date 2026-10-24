# Profile-guided optimisation of the gcc build. Sets NFSU_GCC_FLAGS (runtime, main), NFSU_GCC_GAME_FLAGS (generated
# code), NFSU_GCC_SOURCES and NFSU_GCC_LINK_FLAGS.
#   NFSU_PGO_GENERATE=ON   instrumented build (edge counters): the game writes ux0:data/NFSUG1VITA/nfsu.gcov
#   NFSU_PGO_USE=<files>   optimise with one or more nfsu.gcov files; gcov-tool merges them into the .gcda files of
#                          the build folder at configure time

set(NFSU_GCC_FLAGS "")
set(NFSU_GCC_GAME_FLAGS "")
set(NFSU_GCC_SOURCES "")
set(NFSU_GCC_LINK_FLAGS "")

if(NFSU_PGO_GENERATE)
  # VitaSDK's libgcov has no value profilers and no file output: edge counters, registered in the gcov_info section
  # and written by src/platform/pgo_gcov.cpp.
  set(NFSU_PGO_FLAGS -fprofile-generate -fno-profile-values -fprofile-update=atomic -fprofile-info-section=gcov_info)
  set(NFSU_GCC_FLAGS ${NFSU_PGO_FLAGS})
  set(NFSU_GCC_GAME_FLAGS ${NFSU_PGO_FLAGS})
  set(NFSU_GCC_LINK_FLAGS -fprofile-generate)
  list(APPEND NFSU_RUNTIME_DEFINES NFSU_PGO_GENERATE NFSU_PGO_GCOV)
  set(NFSU_GCC_SOURCES src/platform/pgo.cpp src/platform/pgo_gcov.cpp)
  set_property(SOURCE src/platform/pgo_gcov.cpp APPEND PROPERTY COMPILE_DEFINITIONS
               "NFSU_PGO_BUILD_DIR=\"${CMAKE_BINARY_DIR}\"")
elseif(NFSU_PGO_USE)
  get_filename_component(NFSU_GCC_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
  find_program(NFSU_GCOV_TOOL arm-vita-eabi-gcov-tool HINTS "${NFSU_GCC_BIN}" REQUIRED)
  # Merges the profiles again when the list or a file changed.
  set(stamp "")
  foreach(profile IN LISTS NFSU_PGO_USE)
    if(NOT EXISTS "${profile}")
      message(FATAL_ERROR "NFSU_PGO_USE: ${profile} not found")
    endif()
    file(SHA256 "${profile}" hash)
    string(APPEND stamp "${profile} ${hash}\n")
  endforeach()
  set(stampFile "${CMAKE_BINARY_DIR}/pgo-profiles.txt")
  set(previous "")
  if(EXISTS "${stampFile}")
    file(READ "${stampFile}" previous)
  endif()
  if(NOT previous STREQUAL stamp)
    file(GLOB_RECURSE stale "${CMAKE_BINARY_DIR}/CMakeFiles/*.gcda")
    if(stale)
      file(REMOVE ${stale})
    endif()
    foreach(profile IN LISTS NFSU_PGO_USE)
      message(STATUS "PGO: merging ${profile}")
      execute_process(COMMAND "${NFSU_GCOV_TOOL}" merge-stream "${profile}"
                      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" RESULT_VARIABLE result)
      if(NOT result EQUAL 0)
        file(REMOVE "${stampFile}")
        message(FATAL_ERROR "gcov-tool merge-stream ${profile} failed (${result})")
      endif()
    endforeach()
    file(WRITE "${stampFile}" "${stamp}")
  endif()
  # Functions without counts (not run while profiling) are optimised as without a profile; a source changed since the
  # profile run loses the profile of the changed functions.
  set(NFSU_PGO_FLAGS -fprofile-use -fno-profile-values -fprofile-partial-training -Wno-missing-profile
      -Wno-error=coverage-mismatch)
  set(NFSU_GCC_FLAGS ${NFSU_PGO_FLAGS})
  set(NFSU_GCC_GAME_FLAGS ${NFSU_PGO_FLAGS})
  # Sources compiled with the profile are rebuilt when it changes.
  set_property(SOURCE ${NFSU_GENERATED_SOURCES} ${NFSU_RUNTIME_SOURCES} src/nfsu/main.cpp
               APPEND PROPERTY OBJECT_DEPENDS ${NFSU_PGO_USE})
endif()

message(STATUS "gcc build: PGO generate ${NFSU_PGO_GENERATE}, PGO use '${NFSU_PGO_USE}'")
