#ifndef WINAPI_D3D9_NATIVE_H_
#define WINAPI_D3D9_NATIVE_H_

// Direct calls into the Direct3D 9 device and the D3DX effects for native game code: the same methods the guest
// reaches through its vtables (d3d9.cpp, d3dx9.cpp), taking the guest COM pointer of the object and host pointers
// for the data. The guest's own calls to these methods run through the same functions.
//
// A capture records the calls in order (method, object, arguments, pointed-to data, result). In replay mode each
// call is compared with the next recorded one instead of running, and returns the recorded result; guest calls
// made through callGuest are replayed the same way, with the CPU state the recorded call returned.
//
// A deferral window (deferBegin) queues the calls one guest CPU makes, with their pointed-to data copied, and returns
// D3D_OK at once (Begin stores its pass count at once); the queue runs in order on the queue thread, or on the
// calling thread at the next drain. Every other access of the Direct3D and effect objects drains the queue first
// (drain(): the D3D9 and effect methods the guest calls, the D3DX effect creation, native callers of the device), so
// the calls run in the order they were made and the frame records them at the same place.

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
// Not recorded by a capture.
HRESULT setTextureStageState(WinApplication* app, x86::CPU& cpu, reg32 device, DWORD stage, DWORD type, DWORD value);
HRESULT setSamplerState(WinApplication* app, x86::CPU& cpu, reg32 device, DWORD sampler, DWORD type, DWORD value);
HRESULT setTexture(WinApplication* app, x86::CPU& cpu, reg32 device, DWORD stage, reg32 texture);
HRESULT setVertexDeclaration(WinApplication* app, x86::CPU& cpu, reg32 device, reg32 declaration);
HRESULT setStreamSource(WinApplication* app, x86::CPU& cpu, reg32 device, UINT stream, reg32 buffer, UINT offset,
                        UINT stride);
HRESULT setIndices(WinApplication* app, x86::CPU& cpu, reg32 device, reg32 buffer);
HRESULT drawIndexedPrimitive(WinApplication* app, x86::CPU& cpu, reg32 device, DWORD type, LONG baseVertex,
                             UINT minIndex, UINT vertices, UINT startIndex, UINT count);
// data: the vertices in host memory (a capture records them by value).
HRESULT drawPrimitiveUP(WinApplication* app, x86::CPU& cpu, reg32 device, DWORD type, UINT count, const void* data,
                        UINT stride);

// ---- ID3DXEffect ----

HRESULT effectSetInt(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, x86::sreg32 value);
HRESULT effectSetFloat(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, float value);
HRESULT effectSetVector(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* vector);
HRESULT effectSetVectorArray(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* vectors,
                             UINT count);
HRESULT effectSetMatrix(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* matrix);
HRESULT effectSetMatrixArray(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* matrices,
                             UINT count);
// SetMatrix of a * b (* c when c is not null), multiplied as matrixMultiply does. Outside a capture the product is
// recorded unmultiplied and the render thread multiplies it with the caller's FPSCR (the same floats); under a
// capture it is multiplied here and recorded as a SetMatrix.
HRESULT effectSetMatrixProduct(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* a,
                               const float* b, const float* c);
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
    MethodSetTexture, MethodSetVertexDeclaration, MethodSetStreamSource, MethodSetIndices,
    MethodDrawIndexedPrimitive, MethodDrawPrimitiveUP, MethodEffectSetInt, MethodEffectSetFloat, MethodEffectSetVector,
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
enum { CalloutCount = 16 };
reg32 calloutAddress(uint32_t index);
void setCalloutTarget(uint32_t index, reg32 target);

// Registers the callout methods; called with the Direct3D interfaces (d3d9.cpp).
void init(WinApplication* app);

// ---- Deferred calls ----

enum DeferMode
{
    DeferOff,       // calls run at once
    DeferThread,    // the queue thread runs the queue
    DeferInline     // the next drain runs the queue on the draining thread
};

// Once, before the first window: the mode, and the core (1-3) and priority of the queue thread.
void configureDeferral(DeferMode mode, int core, int priority);
// Opens a window for the calls made with cpu (game thread, execution lock held); false when the mode is off, a
// capture runs or the device has no frame open (no window then). Windows do not nest.
bool deferBegin(WinApplication* app, x86::CPU& cpu, reg32 device);
// Closes the window; the queued calls keep running.
void deferEnd();

extern bool g_deferPending;
void drainSlow();
// Waits until every queued call has run (runs them with DeferInline). Callers hold the execution lock, or no guest
// code runs any more.
inline void drain()
{
    if (__builtin_expect(g_deferPending, 0))
        drainSlow();
}

enum { DeferOpCount = 32 };

// Queue statistics since the last call, for perf logs. A queued call returns the result its arguments give (handle,
// index and object kind checks); draws return D3D_OK.
struct DeferStats
{
    uint32_t windows, calls, words, fallbacks;  // fallbacks: calls of a window that ran at once
    uint32_t drains, windowDrains;              // drains that waited; of them, inside a window
    uint64_t waitUs, runUs;                     // drain waits of the caller; queue run time
    uint32_t mismatches, passes;                // queued calls whose result differs from the one returned; Begin pass
                                                // counts that differ
    uint32_t deletes;                           // objects released by queued calls, deleted by a drain
    uint32_t errors[DeferOpCount];              // per operation: queued calls whose result was not D3D_OK
    HRESULT errorCodes[DeferOpCount];           // the last such result
};
DeferStats takeDeferStats();
// Name of a queue operation (DeferStats::errors index).
const char* deferOpName(uint32_t op);

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
// fpscr: the FPSCR the product is multiplied with.
HRESULT setProduct(reg32 effect, reg32 handle, const float* const* matrices, UINT count, uint32_t fpscr);
// The pass count Begin stores.
UINT passCount(reg32 effect);
// The results the setters return for these arguments, without setting: float setters (data: not null), SetInt,
// SetFloat, SetTexture, Pass.
HRESULT checkFloats(reg32 effect, reg32 handle, bool data);
HRESULT checkInt(reg32 effect, reg32 handle);
HRESULT checkFloat(reg32 effect, reg32 handle);
HRESULT checkTexture(reg32 effect, reg32 handle, reg32 texture);
HRESULT checkPass(reg32 effect, UINT pass);
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
