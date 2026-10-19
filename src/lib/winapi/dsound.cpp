#include <winapi/dsound.h>
#include <winapi/com.h>
#include <winapi/dsound/idirectsound.h>
#include <x86.h>
#include <lib/memmap.h>
#include <cstring>

namespace win32 { namespace dsound
{

HRESULT DirectSoundCreate(WinApplication* app, x86::CPU& cpu,
                          GUID* lpGuid, Packed<IDirectSound>* ppDS, LPVOID punkOuter)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpGuid);
    RC_ASSERT(punkOuter == nullptr);
    *ppDS = Packed<IDirectSound>(com::ComAlloc(app));
    IDirectSound* dsound = &app->getMemory<IDirectSound>(*ppDS);
    new (dsound) IDirectSound();
    return 0;
}

// One device, the primary sound driver (null GUID), as Windows reports it first. The callback is
// BOOL CALLBACK (LPGUID, LPCSTR description, LPCSTR module, LPVOID context).
HRESULT DirectSoundEnumerateA(WinApplication* app, x86::CPU& cpu,
                              x86::reg32 lpDSEnumCallback, x86::reg32 lpContext)
{
    static const char description[] = "Primary Sound Driver";
    MemMap strings(sizeof(description) + 1);
    const x86::reg32 text = strings.getBlockStart();
    memcpy(&app->getMemory<char>(text), description, sizeof(description));
    app->getMemory<char>(text + sizeof(description)) = 0;
    const x86::reg32 esp = cpu.esp;
    cpu.esp -= 20;
    app->getMemory<x86::reg32>(cpu.esp + 4) = 0;
    app->getMemory<x86::reg32>(cpu.esp + 8) = text;
    app->getMemory<x86::reg32>(cpu.esp + 12) = text + sizeof(description);
    app->getMemory<x86::reg32>(cpu.esp + 16) = lpContext;
    cpu.ip = lpDSEnumCallback;
    app->dynamic_call(lpDSEnumCallback, cpu);
    cpu.esp = esp;
    return 0;
}

}}
