# Hand-written runtime sources. The generated game code is globbed separately.

# Direct3D frame model and texture layouts (platform neutral).
set(NFSU_RENDER_SOURCES
  src/platform/render.cpp
  src/platform/texture.cpp
)
# Direct3D 9 and D3DX layer.
set(NFSU_D3D9_SOURCES
  src/lib/winapi/d3d9.cpp
  src/lib/winapi/d3d9_device.cpp
  src/lib/winapi/d3d9_record.cpp
  src/lib/winapi/d3d9_resources.cpp
  src/lib/winapi/d3d9_state.cpp
  src/lib/winapi/d3dx9.cpp
)

set(NFSU_PLATFORM_SOURCES
  ${NFSU_RENDER_SOURCES}
  src/platform/config.cpp
  src/platform/cores.cpp
  src/platform/filesystem.cpp
  src/platform/jobs.cpp
  src/platform/ime.cpp
  src/platform/log.cpp
  src/platform/perf.cpp
  src/platform/time.cpp
  src/platform/sha256.cpp
  src/platform/sync.cpp
  src/platform/msgqueue.cpp
  src/platform/mmx_selftest.cpp
  src/platform/fpu_selftest.cpp
  src/platform/gxm/gxm_device.cpp
  src/platform/gxm/gxm_render.cpp
)

set(NFSU_LIB_SOURCES
  src/lib/audio.cpp
  src/lib/controls.cpp
  src/lib/cpu.cpp
  src/lib/event.cpp
  src/lib/file.cpp
  src/lib/fpucontrol.cpp
  src/lib/gamepad.cpp
  src/lib/gamethreads.cpp
  src/lib/heap.cpp
  src/lib/jump.cpp
  src/lib/library.cpp
  src/lib/mutex.cpp
  src/lib/palette.cpp
  src/lib/registry.cpp
  src/lib/renderer.cpp
  src/lib/surface.cpp
  src/lib/thread.cpp
  src/lib/timer.cpp
  src/lib/winapp.cpp
  src/lib/window.cpp
)

set(NFSU_WINAPI_SOURCES
  ${NFSU_D3D9_SOURCES}
  src/lib/winapi/advapi32.cpp
  src/lib/winapi/com.cpp
  src/lib/winapi/ddraw.cpp
  src/lib/winapi/dinput.cpp
  src/lib/winapi/dinput8.cpp
  src/lib/winapi/dsound.cpp
  src/lib/winapi/gdi32.cpp
  src/lib/winapi/iforce2.cpp
  src/lib/winapi/imports.cpp
  src/lib/winapi/kernel32.cpp
  src/lib/winapi/netapi32.cpp
  src/lib/winapi/ole32.cpp
  src/lib/winapi/shell32.cpp
  src/lib/winapi/shfolder.cpp
  src/lib/winapi/support.cpp
  src/lib/winapi/tapi32.cpp
  src/lib/winapi/user32.cpp
  src/lib/winapi/winmm.cpp
  src/lib/winapi/ws2_32.cpp
  src/lib/winapi/ddraw/idirectdraw2.cpp
  src/lib/winapi/ddraw/idirectdrawpalette.cpp
  src/lib/winapi/ddraw/idirectdrawsurface.cpp
  src/lib/winapi/dinput/idirectinput2.cpp
  src/lib/winapi/dinput/idirectinput8.cpp
  src/lib/winapi/dinput/idirectinputdevice.cpp
  src/lib/winapi/dsound/idirectsound.cpp
  src/lib/winapi/dsound/idirectsound3dlistener.cpp
  src/lib/winapi/dsound/idirectsoundbuffer.cpp
)

# Port code that works with guest addresses of Speed.exe.
set(NFSU_GAME_PORT_SOURCES
  src/nfsu/audio_mixer.cpp
  src/nfsu/bankcode.cpp
  src/nfsu/bench.cpp
  src/nfsu/coll.cpp
  src/nfsu/cull.cpp
  src/nfsu/mathfn.cpp
  src/nfsu/movie_yuv.cpp
  src/nfsu/prep.cpp
  src/nfsu/native.cpp
)

# Runtime sources that include headers generated from the game binary (nfsu_layout.h).
set(NFSU_GENERATED_HEADER_SOURCES
  src/lib/memmap.cpp
)

# Runtime sources that build without the generated code (the CI runtime check).
set(NFSU_STANDALONE_SOURCES
  ${NFSU_PLATFORM_SOURCES}
  ${NFSU_LIB_SOURCES}
  ${NFSU_WINAPI_SOURCES}
  ${NFSU_GAME_PORT_SOURCES}
)

set(NFSU_RUNTIME_SOURCES
  ${NFSU_STANDALONE_SOURCES}
  ${NFSU_GENERATED_HEADER_SOURCES}
)
