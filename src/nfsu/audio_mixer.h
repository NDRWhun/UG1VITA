#ifndef NFSU_AUDIO_MIXER_H_
#define NFSU_AUDIO_MIXER_H_

#include <lib/winapp.h>
#include <x86.h>

// EA SND render loop without the execution lock (audio_mixer.cpp).
namespace nfsu { namespace mixer
{

// True while the calling guest thread runs the render loop 0x654050 without the execution lock.
bool unlocked(const x86::CPU& cpu);

// When the calling thread runs the render loop without the execution lock, takes the lock for the rest of that run
// of the loop; does nothing otherwise.
void lockRest(win32::WinApplication* app, const x86::CPU& cpu);

// In the render loop without the execution lock: true when a bank routine's call of the library function target
// (0x64D860 sound state, 0x64C690 instance teardown) reaches only audited code in the current SND state; else false
// with the failed check (a string literal) and the guest value it failed on. The call's arguments are on cpu's
// stack above the return address.
bool libraryAudited(x86::reg32 target, const x86::CPU& cpu, const char*& reason, x86::reg32& value);

// Holds the execution lock while it lives when the calling thread runs the render loop without it; does nothing
// otherwise.
class Locked
{
public:
    Locked(win32::WinApplication* app, const x86::CPU& cpu);
    ~Locked();

private:
    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;
    win32::WinApplication*  m_app;
    const x86::CPU&         m_cpu;
    bool                    m_taken;
};

}}

#endif
