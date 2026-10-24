#ifndef NFSU_AUDIO_MIXER_H_
#define NFSU_AUDIO_MIXER_H_

#include <lib/winapp.h>
#include <x86.h>

// EA SND render loop without the execution lock (audio_mixer.cpp).
namespace nfsu { namespace mixer
{

// True while the calling guest thread runs the render loop 0x654050 without the execution lock.
bool unlocked(const x86::CPU& cpu);

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
