# Shaders: Cg sources in shaders/, prebuilt .gxp binaries in shaders/gxp/, embedded as C arrays.
# With SCE_PSP2_SDK_DIR (official PSP2 SDK) the sources are compiled with psp2cgc and the result also refreshes
# shaders/gxp/, so the committed binaries follow the sources. Without it the committed binaries are used.

if(NOT DEFINED SCE_PSP2_SDK_DIR AND DEFINED ENV{SCE_PSP2_SDK_DIR})
  set(SCE_PSP2_SDK_DIR "$ENV{SCE_PSP2_SDK_DIR}" CACHE PATH "Official PSP2 SDK root (optional, provides psp2cgc)")
endif()

if(SCE_PSP2_SDK_DIR)
  find_program(PSP2CGC psp2cgc
    PATHS "${SCE_PSP2_SDK_DIR}/host_tools/bin"
    NO_DEFAULT_PATH)
  if(NOT PSP2CGC)
    message(FATAL_ERROR "psp2cgc not found under ${SCE_PSP2_SDK_DIR}/host_tools/bin")
  endif()
  message(STATUS "Shaders: compiled with ${PSP2CGC}")
else()
  message(STATUS "Shaders: prebuilt binaries from shaders/gxp")
endif()

set(NFSU_SHADER_BINARY_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${NFSU_SHADER_BINARY_DIR}")

# nfsu_add_shader(<list-var> <name> <sce_vp_psp2|sce_fp_psp2> [SOURCE <file.cg>] [DEFINES <NAME=value>...])
# Adds <name>.gxp -> <name>_gxp.c (symbols shader_<name>, shader_<name>_size). SOURCE defaults to <name>.cg;
# DEFINES build a variant of a shared source.
function(nfsu_add_shader out_var name profile)
  cmake_parse_arguments(SHADER "" "SOURCE" "DEFINES" ${ARGN})
  if(NOT SHADER_SOURCE)
    set(SHADER_SOURCE "${name}.cg")
  endif()
  set(defines "")
  foreach(define ${SHADER_DEFINES})
    list(APPEND defines "-D${define}")
  endforeach()
  set(src "${CMAKE_SOURCE_DIR}/shaders/${SHADER_SOURCE}")
  set(prebuilt "${CMAKE_SOURCE_DIR}/shaders/gxp/${name}.gxp")
  set(csrc "${NFSU_SHADER_BINARY_DIR}/${name}_gxp.c")
  if(SCE_PSP2_SDK_DIR)
    set(gxp "${NFSU_SHADER_BINARY_DIR}/${name}.gxp")
    add_custom_command(
      OUTPUT "${gxp}"
      # Relative source name: psp2cgc hashes the source path string into the header (bytes 16-19), so the
      # output is then the same in every checkout.
      COMMAND "${PSP2CGC}" -profile ${profile} -O3 -Wperf ${defines} -o "${gxp}" "${SHADER_SOURCE}"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${gxp}" "${prebuilt}"
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/shaders"
      DEPENDS "${src}"
      COMMENT "Compiling shader ${name}.cg (${profile})"
      VERBATIM)
  else()
    set(gxp "${prebuilt}")
    if(NOT EXISTS "${gxp}")
      message(FATAL_ERROR "Missing prebuilt shader ${gxp}; set SCE_PSP2_SDK_DIR to compile it.")
    endif()
  endif()
  add_custom_command(
    OUTPUT "${csrc}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${gxp}" "-DOUTPUT=${csrc}" "-DSYMBOL=shader_${name}"
            -P "${CMAKE_SOURCE_DIR}/cmake/bin2c.cmake"
    DEPENDS "${gxp}" "${CMAKE_SOURCE_DIR}/cmake/bin2c.cmake"
    COMMENT "Embedding shader ${name}.gxp"
    VERBATIM)
  list(APPEND ${out_var} "${csrc}")
  set(${out_var} "${${out_var}}" PARENT_SCOPE)
endfunction()

# Every program of the port: the present and clear programs, the Direct3D effect and fixed-function programs
# (include/platform/render.h); fragment programs also with -DATEST (alpha test, suffix _atest), filter programs also
# with -DFACTOR (blend factor outputs, suffixes _bfcolor, _bfalpha, _bfscale).
function(nfsu_add_shaders out_var)
  set(list "")
  nfsu_add_shader(list blit_v  sce_vp_psp2)
  nfsu_add_shader(list blit_f  sce_fp_psp2)
  nfsu_add_shader(list clear_v sce_vp_psp2)
  nfsu_add_shader(list clear_f sce_fp_psp2)
  foreach(name world_v worldheadlight_v worldreflect_v worldheadlightreflect_v worldbone_v car_v filter_v clearz_v)
    nfsu_add_shader(list ${name} sce_vp_psp2)
  endforeach()
  nfsu_add_shader(list ff_v sce_vp_psp2)
  nfsu_add_shader(list ff_screen_v sce_vp_psp2 SOURCE ff_v.cg DEFINES SCREEN=1)
  foreach(name world_f worldheadlight_f worldreflect_f worldheadlightreflect_f car_f)
    nfsu_add_shader(list ${name} sce_fp_psp2)
    nfsu_add_shader(list ${name}_atest sce_fp_psp2 SOURCE ${name}.cg DEFINES ATEST=1)
  endforeach()
  foreach(taps 1 2 3 4)
    nfsu_add_shader(list filter${taps}_f sce_fp_psp2 SOURCE filter_f.cg DEFINES TAPS=${taps})
    nfsu_add_shader(list filter${taps}_f_atest sce_fp_psp2 SOURCE filter_f.cg DEFINES TAPS=${taps} ATEST=1)
    # Blend factor variants (render::FactorOutput 1..3).
    set(factor 1)
    foreach(suffix bfcolor bfalpha bfscale)
      nfsu_add_shader(list filter${taps}_f_${suffix} sce_fp_psp2 SOURCE filter_f.cg DEFINES TAPS=${taps} FACTOR=${factor})
      math(EXPR factor "${factor} + 1")
    endforeach()
  endforeach()
  foreach(variant color tex proj)
    string(TOUPPER ${variant} define)
    set(defines "")
    if(NOT variant STREQUAL "color")
      set(defines ${define}=1)
    endif()
    nfsu_add_shader(list ff_${variant}_f sce_fp_psp2 SOURCE ff_f.cg DEFINES ${defines})
    nfsu_add_shader(list ff_${variant}_f_atest sce_fp_psp2 SOURCE ff_f.cg DEFINES ${defines} ATEST=1)
  endforeach()
  set(${out_var} "${list}" PARENT_SCOPE)
endfunction()
