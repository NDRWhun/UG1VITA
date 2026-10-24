#ifndef WINAPI_D3D9_NATIVE_H_
#define WINAPI_D3D9_NATIVE_H_

// Direct calls into the Direct3D 9 device and the D3DX effects for native game code: the same methods the guest
// reaches through its vtables (d3d9.cpp, d3dx9.cpp), taking the guest COM pointer of the object and host pointers
// for the data. The guest's own calls to these methods run through the same functions.
//
// A capture records the calls in order (method, object, arguments, pointed-to data, result). In replay mode each
// call is compared with the next recorded one instead of running, and returns the recorded result; guest calls
// made through callGuest are replayed the same way, with the CPU state the recorded call returned.

#include <x86.h>
#include <winapi/types.h>

#include <vector>

namespace win32
{

class WinApplication;

}

namespace win32 { namespace d3d9 { namespace bridge
{

using x86::reg32;

// ---- IDirect3DDevice9 ----

HRESULT setTransform(WinApplication* app, x86::CPU& cpu, reg32 device, DWORD state, const float* matrix);
HRESULT setMaterial(WinApplication* app, x86::CPU& cpu, reg32 device, const DWORD* material);
HRESULT setLight(WinApplication* app, x86::CPU& cpu, reg32 device, DWORD index, const DWORD* light);
HRESULT lightEnable(WinApplication* app, x86::CPU& cpu, reg32 device, DWORD index, BOOL enable);
HRESULT setRenderState(WinApplication* app, x86::CPU& cpu, reg32 device, DWORD state, DWORD value);
HRESULT setTexture(WinApplication* app, x86::CPU& cpu, reg32 device, DWORD stage, reg32 texture);
HRESULT setVertexDeclaration(WinApplication* app, x86::CPU& cpu, reg32 device, reg32 declaration);

// ---- ID3DXEffect ----

HRESULT effectSetInt(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, x86::sreg32 value);
HRESULT effectSetFloat(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, float value);
HRESULT effectSetVector(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* vector);
HRESULT effectSetVectorArray(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* vectors,
                             UINT count);
HRESULT effectSetMatrix(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* matrix);
HRESULT effectSetMatrixArray(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* matrices,
                             UINT count);
HRESULT effectSetTexture(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, reg32 texture);
HRESULT effectBegin(WinApplication* app, x86::CPU& cpu, reg32 effect, UINT* passes, DWORD flags);
HRESULT effectPass(WinApplication* app, x86::CPU& cpu, reg32 effect, UINT pass);
HRESULT effectEnd(WinApplication* app, x86::CPU& cpu, reg32 effect);

// ---- Guest code ----

// Calls the guest function at address with cpu (esp at the return address slot), as a call instruction of the
// guest does.
void callGuest(WinApplication* app, x86::CPU& cpu, reg32 address);

// ---- Capture ----

enum Method : uint8_t
{
    MethodSetTransform, MethodSetMaterial, MethodSetLight, MethodLightEnable, MethodSetRenderState,
    MethodSetTexture, MethodSetVertexDeclaration, MethodEffectSetInt, MethodEffectSetFloat, MethodEffectSetVector,
    MethodEffectSetVectorArray, MethodEffectSetMatrix, MethodEffectSetMatrixArray, MethodEffectSetTexture,
    MethodEffectBegin, MethodEffectPass, MethodEffectEnd, MethodGuestCall, MethodCount
};

// Name of a method, for logs.
const char* methodName(uint8_t method);

struct Capture
{
    enum Mode
    {
        Record,     // the calls run and are recorded
        Replay      // the calls are compared with the recorded ones and do not run
    };

    struct Call
    {
        uint32_t first;     // index of its words
        uint32_t count;
        uint8_t method;
        HRESULT result;
        uint32_t output;    // Begin: the pass count it stored
        uint32_t cpu;       // MethodGuestCall: index in cpus of the state after the call
    };

    Mode mode = Record;
    std::vector<uint32_t> words;
    std::vector<Call> calls;
    std::vector<x86::CPU> cpus;
    // Replay: the next recorded call, the replayed calls and the first one that differed.
    uint32_t next = 0;
    uint32_t replayed = 0;
    bool differs = false;
    uint32_t differingCall = 0;
    uint8_t expected = MethodCount, got = MethodCount;
    uint32_t differingWord = 0;
    uint32_t expectedWord = 0, gotWord = 0;

    void clear();
};

// Starts recording or replaying the calling thread's calls into capture; end() stops it.
void begin(Capture& capture, Capture::Mode mode);
void end();
// True while a capture runs on the calling thread.
bool capturing();

// While the guest run of a shadow compare is recorded, a guest call through one of these addresses is recorded as a
// guest call to target and runs target with the capture paused. index < CalloutCount.
enum { CalloutCount = 2 };
reg32 calloutAddress(uint32_t index);
void setCalloutTarget(uint32_t index, reg32 target);

// Registers the callout methods; called with the Direct3D interfaces (d3d9.cpp).
void init(WinApplication* app);

// ---- Effect parts used by the bridge (d3dx9.cpp) ----

namespace effect
{

HRESULT setInt(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, x86::sreg32 value);
HRESULT setFloat(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, float value);
HRESULT setVector(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* vector);
HRESULT setVectorArray(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* vectors,
                       UINT count);
HRESULT setMatrix(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* matrix);
HRESULT setMatrixArray(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* matrices,
                       UINT count);
HRESULT setTexture(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, reg32 texture);
HRESULT begin(WinApplication* app, x86::CPU& cpu, reg32 effect, UINT* passes, DWORD flags);
HRESULT pass(WinApplication* app, x86::CPU& cpu, reg32 effect, UINT pass);
HRESULT end(WinApplication* app, x86::CPU& cpu, reg32 effect);

}

// ---- D3DX math on host data, as the D3DX overrides compute it ----

// out = a * b (D3DXMatrixMultiply); out may alias a or b.
void matrixMultiply(const float* a, const float* b, float* out);
// out = v * m (D3DXVec4Transform).
void vec4Transform(const float* v, const float* m, float* out);

}}}

#endif
