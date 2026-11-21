#ifndef NFSU_SCENETHREAD_H_
#define NFSU_SCENETHREAD_H_

#include <lib/winapp.h>
#include <recomp.h>

namespace nfsu { namespace scenethread
{

// The scene build's worker thread (docs PARALLEL_SCENE.md phase 3): the world views run on a host thread of their
// own on another core while the game thread waits for them, holding the execution lock.
//
// The worker runs as the game thread's guest thread: the same CPU (registers, x87 stack, fs and its thread block),
// the same guest stack and the game thread's hold of the execution lock, so the Direct3D deferral (owned by fs) and
// the guest code the views still call see no other thread. Its own: the host thread and stack, the FPSCR (copied
// from the game thread at every handoff) and its CPU time. Its lock operations (a blocking shim releasing the lock,
// a preemption point) run on the game thread, which waits for them (WinApplication::setLockForward).
//
// The game thread spins while it waits (its core has nothing else to run until the views return); the worker sleeps
// between frames and spins briefly for a forwarded operation before it sleeps.
//
// Settings: scene_thread=on runs the views there (it implies scene_snapshot=serial; off by default);
// scene_thread_core=1, 2 (default) or 3 (only with core 3 available) is its core, scene_thread_priority its
// priority (64-127, default 70).

// True with scene_thread=on.
bool on();

// Runs run(app, cpu) on the worker with scene_thread=on and returns when it has returned; else on the calling
// thread. The game thread only.
void run(win32::WinApplication* app, x86::CPU& cpu, void (*run)(win32::WinApplication*, x86::CPU&));

// At the benchmark's end: the handoffs, their wake-up and join times and the forwarded lock operations.
void logTotal();

}}

#endif
