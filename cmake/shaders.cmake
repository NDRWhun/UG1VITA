# Shaders: Cg sources in shaders/, compiled .gxp binaries in shaders/gxp/, embedded as C arrays.

set(NFSU_SHADER_BINARY_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${NFSU_SHADER_BINARY_DIR}")

# nfsu_add_shader(<list-var> <name> <sce_vp_psp2|sce_fp_psp2> [SOURCE <file.cg>] [DEFINES <NAME=value>...])
# Adds shaders/gxp/<name>.gxp -> <name>_gxp.c (symbols shader_<name>, shader_<name>_size). SOURCE and DEFINES name
# the Cg source and defines the binary was compiled from (SOURCE defaults to <name>.cg).
function(nfsu_add_shader out_var name profile)
  set(gxp "${CMAKE_SOURCE_DIR}/shaders/gxp/${name}.gxp")
  set(csrc "${NFSU_SHADER_BINARY_DIR}/${name}_gxp.c")
  if(NOT EXISTS "${gxp}")
    message(FATAL_ERROR "Missing shader ${gxp}")
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
  nfsu_add_shader(list world_proj_v sce_vp_psp2 SOURCE world_v.cg DEFINES PROJ=1)
  nfsu_add_shader(list ff_v sce_vp_psp2)
  nfsu_add_shader(list ff_screen_v sce_vp_psp2 SOURCE ff_v.cg DEFINES SCREEN=1)
  foreach(name world_f worldheadlight_f worldreflect_f worldheadlightreflect_f car_f)
    nfsu_add_shader(list ${name} sce_fp_psp2)
    nfsu_add_shader(list ${name}_atest sce_fp_psp2 SOURCE ${name}.cg DEFINES ATEST=1)
  endforeach()
  nfsu_add_shader(list world_proj_f sce_fp_psp2 SOURCE world_f.cg DEFINES PROJ=1)
  nfsu_add_shader(list world_proj_f_atest sce_fp_psp2 SOURCE world_f.cg DEFINES PROJ=1 ATEST=1)
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
