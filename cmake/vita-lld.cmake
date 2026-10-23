# nfsu_lld_script(<out-var>): writes VitaSDK's default ld script (armelf.xc), edited for lld, to the build directory
# and returns its path.
function(nfsu_lld_script out_var)
  set(stock "${VITASDK}/arm-vita-eabi/lib/ldscripts/armelf.xc")
  if(NOT EXISTS "${stock}")
    message(FATAL_ERROR "VitaSDK linker script not found: ${stock}")
  endif()
  file(READ "${stock}" script)
  set(original "${script}")
  # ALIGN(ALIGNOF(NEXT_SECTION)) becomes ALIGN(8).
  string(REPLACE "ALIGN(ALIGNOF(NEXT_SECTION))" "ALIGN(8)" script "${script}")
  # Removes the .stack section.
  string(REGEX REPLACE "\n  \\.stack[ ]+0x80000 :[^}]*}" "" script "${script}")
  # Moves .preinit_array to the start of .init_array.
  string(REGEX REPLACE "\n  \\.preinit_array[ ]+:[^}]*}" "" script "${script}")
  string(REPLACE "    PROVIDE_HIDDEN (__init_array_start = .);"
    "    PROVIDE_HIDDEN (__preinit_array_start = .);\n    KEEP (*(.preinit_array))\n    PROVIDE_HIDDEN (__preinit_array_end = .);\n    PROVIDE_HIDDEN (__init_array_start = .);"
    script "${script}")
  if(script MATCHES "NEXT_SECTION|\\.stack[ ]+0x80000|\\.preinit_array[ ]+:" OR NOT script MATCHES "__preinit_array_end"
     OR script STREQUAL original)
    message(FATAL_ERROR "${stock} changed: update cmake/vita-lld.cmake")
  endif()
  set(out "${CMAKE_BINARY_DIR}/vita-lld.ld")
  # Written only when the content changes.
  set(current "")
  if(EXISTS "${out}")
    file(READ "${out}" current)
  endif()
  if(NOT current STREQUAL script)
    file(WRITE "${out}" "${script}")
  endif()
  set(${out_var} "${out}" PARENT_SCOPE)
endfunction()
