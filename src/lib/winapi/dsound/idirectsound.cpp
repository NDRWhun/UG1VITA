#include <winapi/dsound/idirectsound.h>
#include <winapi/dsound/idirectsoundbuffer.h>
#include <winapi/support.h>
#include <winapi/wrapper.h>
#include <lib/audio.h>
#include <platform/log.h>


namespace win32 { namespace dsound
{

x86::reg32 IDirectSound::s_iDirectSoundVtable = 0;

struct DSCAPS
{
    DWORD  dwSize;
    DWORD  dwFlags;
    DWORD  dwMinSecondarySampleRate;
    DWORD  dwMaxSecondarySampleRate;
    DWORD  dwPrimaryBuffers;
    DWORD  dwMaxHwMixingAllBuffers;
    DWORD  dwMaxHwMixingStaticBuffers;
    DWORD  dwMaxHwMixingStreamingBuffers;
    DWORD  dwFreeHwMixingAllBuffers;
    DWORD  dwFreeHwMixingStaticBuffers;
    DWORD  dwFreeHwMixingStreamingBuffers;
    DWORD  dwMaxHw3DAllBuffers;
    DWORD  dwMaxHw3DStaticBuffers;
    DWORD  dwMaxHw3DStreamingBuffers;
    DWORD  dwFreeHw3DAllBuffers;
    DWORD  dwFreeHw3DStaticBuffers;
    DWORD  dwFreeHw3DStreamingBuffers;
    DWORD  dwTotalHwMemBytes;
    DWORD  dwFreeHwMemBytes;
    DWORD  dwMaxContigFreeHwMemBytes;
    DWORD  dwUnlockTransferRateHwBuffers;
    DWORD  dwPlayCpuOverheadSwBuffers;
    DWORD  dwReserved1;
    DWORD  dwReserved2;
};

enum DirectSoundCaps
{
    DSCAPS_PRIMARYMONO        = 0x00000001,
    DSCAPS_PRIMARYSTEREO      = 0x00000002,
    DSCAPS_PRIMARY8BIT        = 0x00000004,
    DSCAPS_PRIMARY16BIT       = 0x00000008,
    DSCAPS_CONTINUOUSRATE     = 0x00000010,
    DSCAPS_EMULDRIVER         = 0x00000020,
    DSCAPS_CERTIFIED          = 0x00000040,
    DSCAPS_SECONDARYMONO      = 0x00000100,
    DSCAPS_SECONDARYSTEREO    = 0x00000200,
    DSCAPS_SECONDARY8BIT      = 0x00000400,
    DSCAPS_SECONDARY16BIT     = 0x00000800
};

static x86::reg32 s_primaryBuffer = 0;
static AudioDevice* s_device = nullptr;

DEFINE_GUID(IID_IDirectSound,           0x279AFA83,0x4981,0x11CE,0xA5,0x21,0x00,0x20,0xAF,0x0B,0xE5,0x60);

// One output port and mixer shared by every IDirectSound object.
static AudioDevice* sharedDevice(WinApplication* app)
{
    if (!s_device)
    {
        s_device = new AudioDevice();
        app->allocateResource(s_device);
    }
    return s_device;
}

IDirectSound::IDirectSound()
    :   IUnknown(IID_IDirectSound)
{
    m_vtable = s_iDirectSoundVtable;
    m_resource = nullptr;
}

HRESULT IDirectSound::CreateSoundBuffer(WinApplication* app, x86::CPU& cpu,
                                       LPCDSBUFFERDESC lpcDSBufferDesc, LPLPDIRECTSOUNDBUFFER lplpDirectSoundBuffer, IUnknown *pUnkOuter)
{
    if (!lplpDirectSoundBuffer)
        return DSERR_INVALIDPARAM;
    if (pUnkOuter)
        return DSERR_NOAGGREGATION;
    // Mixing is software only and buffers have no 3D control (no DS3D buffer object): the game's hardware 3D and
    // EAX probe asks for LOCHARDWARE | CTRL3D and gives up quietly.
    if (lpcDSBufferDesc && lpcDSBufferDesc->validSize()
        && !(lpcDSBufferDesc->dwFlags & DSBCAPS_PRIMARYBUFFER)
        && (lpcDSBufferDesc->dwFlags & (DSBCAPS_LOCHARDWARE | DSBCAPS_CTRL3D)))
    {
        pf::logInfo("CreateSoundBuffer: flags 0x%x need hardware or 3D buffers, not available",
                    lpcDSBufferDesc->dwFlags);
        *lplpDirectSoundBuffer = Packed<IDirectSoundBuffer>();
        return (lpcDSBufferDesc->dwFlags & DSBCAPS_LOCHARDWARE) ? DSERR_INVALIDCALL : DSERR_CONTROLUNAVAIL;
    }
    AudioFormat format;
    DWORD bufferBytes;
    const HRESULT result = IDirectSoundBuffer::readDescription(app, lpcDSBufferDesc, &format, &bufferBytes);
    if (result != DS_OK)
    {
        *lplpDirectSoundBuffer = Packed<IDirectSoundBuffer>();
        return result;
    }
    if (lpcDSBufferDesc->dwFlags & DSBCAPS_PRIMARYBUFFER)
    {
        if (!s_primaryBuffer)
        {
            s_primaryBuffer = Packed<IDirectSoundBuffer>(com::ComAlloc(app));
            IDirectSoundBuffer* buffer = &app->getMemory<IDirectSoundBuffer>(s_primaryBuffer);
            new (buffer) IDirectSoundBuffer(app, sharedDevice(app), bufferBytes, format, true);
            // The cached primary buffer keeps one reference of its own: the caps probe releases its device and
            // primary buffer before the game creates them again.
            buffer->AddRef(app, cpu);
        }
        else
        {
            (&app->getMemory<IDirectSoundBuffer>(s_primaryBuffer))->AddRef(app, cpu);
        }
        *lplpDirectSoundBuffer = Packed<IDirectSoundBuffer>(s_primaryBuffer);
    }
    else
    {
        *lplpDirectSoundBuffer = Packed<IDirectSoundBuffer>(com::ComAlloc(app));
        IDirectSoundBuffer* buffer = &app->getMemory<IDirectSoundBuffer>(*lplpDirectSoundBuffer);
        new (buffer) IDirectSoundBuffer(app, sharedDevice(app), bufferBytes, format, false);
    }
    return DS_OK;
}

HRESULT IDirectSound::GetCaps(WinApplication* app, x86::CPU& cpu,
                              LPDSCAPS lpDSCaps)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!lpDSCaps || lpDSCaps->dwSize != sizeof(DSCAPS))
        return DSERR_INVALIDPARAM;
    lpDSCaps->dwFlags = DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY8BIT | DSCAPS_PRIMARY16BIT
                      | DSCAPS_SECONDARYMONO | DSCAPS_SECONDARYSTEREO | DSCAPS_SECONDARY8BIT | DSCAPS_SECONDARY16BIT
                      | DSCAPS_CONTINUOUSRATE | DSCAPS_CERTIFIED;
    // A software-mixing device (not emulated: DSCAPS_EMULDRIVER clear) without hardware buffers. The game clamps
    // the maximum rate to 44100 for its mixer.
    lpDSCaps->dwMinSecondarySampleRate = 100;
    lpDSCaps->dwMaxSecondarySampleRate = 48000;
    lpDSCaps->dwPrimaryBuffers = 1;
    lpDSCaps->dwMaxHwMixingAllBuffers = 0;
    lpDSCaps->dwMaxHwMixingStaticBuffers = 0;
    lpDSCaps->dwMaxHwMixingStreamingBuffers = 0;
    lpDSCaps->dwFreeHwMixingAllBuffers = 0;
    lpDSCaps->dwFreeHwMixingStaticBuffers = 0;
    lpDSCaps->dwFreeHwMixingStreamingBuffers = 0;
    lpDSCaps->dwMaxHw3DAllBuffers = 0;
    lpDSCaps->dwMaxHw3DStaticBuffers = 0;
    lpDSCaps->dwMaxHw3DStreamingBuffers = 0;
    lpDSCaps->dwFreeHw3DAllBuffers = 0;
    lpDSCaps->dwFreeHw3DStaticBuffers = 0;
    lpDSCaps->dwFreeHw3DStreamingBuffers = 0;
    lpDSCaps->dwTotalHwMemBytes = 0;
    lpDSCaps->dwFreeHwMemBytes = 0;
    lpDSCaps->dwMaxContigFreeHwMemBytes = 0;
    lpDSCaps->dwUnlockTransferRateHwBuffers = 0;
    lpDSCaps->dwPlayCpuOverheadSwBuffers = 0;
    lpDSCaps->dwReserved1 = 0;
    lpDSCaps->dwReserved2 = 0;
    return DS_OK;
}

HRESULT IDirectSound::DuplicateSoundBuffer(WinApplication* app, x86::CPU& cpu,
                                           LPDIRECTSOUNDBUFFER lpDsbOriginal, LPLPDIRECTSOUNDBUFFER lplpDsbDuplicate)
{
    RC_USE(lpDsbOriginal);
    RC_USE(lplpDsbDuplicate);
    unsupported(app, cpu, "IDirectSound::DuplicateSoundBuffer", "buffer 0x%08x", ComGetIndex(app, this));
}

HRESULT IDirectSound::SetCooperativeLevel(WinApplication* app, x86::CPU& cpu,
                                          HWND hwnd, DWORD dwLevel)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hwnd);
    RC_USE(dwLevel);
    m_resource = sharedDevice(app);
    return DS_OK;
}

// There is no sound card memory to compact.
HRESULT IDirectSound::Compact(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return DS_OK;
}

// The Vita speakers or headphones: DSSPEAKER_STEREO with a wide (20 degree) geometry.
HRESULT IDirectSound::GetSpeakerConfig(WinApplication* app, x86::CPU& cpu,
                                       LPDWORD lpdwSpeakerConfig)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!lpdwSpeakerConfig)
        return DSERR_INVALIDPARAM;
    *lpdwSpeakerConfig = 0x00140004;
    return DS_OK;
}

// The output stays stereo whatever the game asks for.
HRESULT IDirectSound::SetSpeakerConfig(WinApplication* app, x86::CPU& cpu,
                                       DWORD dwSpeakerConfig)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(dwSpeakerConfig);
    return DS_OK;
}

// DirectSoundCreate initialised the object already.
HRESULT IDirectSound::Initialize(WinApplication* app, x86::CPU& cpu,
                                 const GUID* lpcGuid)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpcGuid);
    return DSERR_ALREADYINITIALIZED;
}

x86::reg32 IDirectSound::init(WinApplication *app, x86::reg32 memblockIndex, x86::reg8* memBlock)
{
    x86::reg32* vtable = reinterpret_cast<x86::reg32*>(memBlock);
    app->registerMethod(0x400060, {"CreateSoundBuffer", &Wrapper<decltype(&IDirectSound::CreateSoundBuffer), &IDirectSound::CreateSoundBuffer>::stdcall});
    app->registerMethod(0x400061, {"GetCaps", &Wrapper<decltype(&IDirectSound::GetCaps), &IDirectSound::GetCaps>::stdcall});
    app->registerMethod(0x400062, {"DuplicateSoundBuffer", &Wrapper<decltype(&IDirectSound::DuplicateSoundBuffer), &IDirectSound::DuplicateSoundBuffer>::stdcall});
    app->registerMethod(0x400063, {"SetCooperativeLevel", &Wrapper<decltype(&IDirectSound::SetCooperativeLevel), &IDirectSound::SetCooperativeLevel>::stdcall});
    app->registerMethod(0x400064, {"Compact", &Wrapper<decltype(&IDirectSound::Compact), &IDirectSound::Compact>::stdcall});
    app->registerMethod(0x400065, {"GetSpeakerConfig", &Wrapper<decltype(&IDirectSound::GetSpeakerConfig), &IDirectSound::GetSpeakerConfig>::stdcall});
    app->registerMethod(0x400066, {"SetSpeakerConfig", &Wrapper<decltype(&IDirectSound::SetSpeakerConfig), &IDirectSound::SetSpeakerConfig>::stdcall});
    app->registerMethod(0x400067, {"Initialize", &Wrapper<decltype(&IDirectSound::Initialize), &IDirectSound::Initialize>::stdcall});

    s_iDirectSoundVtable = memblockIndex;
    vtable[0] = 0x400000;    // IUnknown_QueryInterface
    vtable[1] = 0x400001;    // IUnknown_AddRef
    vtable[2] = 0x400002;    // IUnknwon_Release
    vtable[3] = 0x400060;    // IDirectSound_CreateSoundBuffer
    vtable[4] = 0x400061;    // IDirectSound_GetCaps
    vtable[5] = 0x400062;    // IDirectSound_DuplicateSoundBuffer
    vtable[6] = 0x400063;    // IDirectSound_SetCooperativeLevel
    vtable[7] = 0x400064;    // IDirectSound_Compact
    vtable[8] = 0x400065;    // IDirectSound_GetSpeakerConfig
    vtable[9] = 0x400066;    // IDirectSound_SetSpeakerConfig
    vtable[10] = 0x400067;   // IDirectSound_FindDevice
    return 11 * sizeof(x86::reg32);
}

}}
