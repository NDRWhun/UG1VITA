// Direct calls into the Direct3D 9 device and the D3DX effects, their capture and replay, their deferral
// (d3d9_native.h), and the guest entries of the bridged device methods.

#include <winapi/d3d9_native.h>
#include <winapi/d3d9_internal.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/sync.h>
#include <psp2/kernel/processmgr.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <recomp.h>

namespace win32 { namespace d3d9 { namespace bridge
{

bool g_deferPending = false;

namespace
{

// Per thread: calls of other threads during a capture run and are not recorded.
thread_local Capture* s_capture = nullptr;
thread_local uint32_t s_paused = 0;
WinApplication* s_app = nullptr;
MemMap* s_calloutMemory = nullptr;
reg32 s_calloutTargets[CalloutCount] = {};

const char* const s_methodNames[MethodCount] = {
    "SetTransform", "SetMaterial", "SetLight", "LightEnable", "SetRenderState", "SetTexture",
    "SetVertexDeclaration", "SetStreamSource", "SetIndices", "DrawIndexedPrimitive", "DrawPrimitiveUP",
    "Effect::SetInt", "Effect::SetFloat", "Effect::SetVector", "Effect::SetVectorArray",
    "Effect::SetMatrix", "Effect::SetMatrixArray", "Effect::SetTexture", "Effect::Begin", "Effect::Pass",
    "Effect::End", "guest call",
};

inline Device* device(reg32 guest)
{
    return static_cast<Device*>(lookup(guest));
}

inline uint32_t bits(float value)
{
    uint32_t r;
    memcpy(&r, &value, 4);
    return r;
}

inline float asFloat(uint32_t value)
{
    float f;
    memcpy(&f, &value, 4);
    return f;
}

// One call under a capture: its words are appended (record) or collected and compared with the next recorded call
// (replay).
class Record
{
public:
    explicit Record(uint8_t method) : m_capture(*s_capture), m_method(method)
    {
        if (m_capture.mode == Capture::Record)
        {
            m_call.first = uint32_t(m_capture.words.size());
            m_call.count = 0;
            m_call.method = method;
            m_call.result = D3D_OK;
            m_call.output = 0;
            m_call.cpu = 0;
        }
    }

    void word(uint32_t value)
    {
        if (m_capture.mode == Capture::Record)
            m_capture.words.push_back(value);
        else
            m_words.push_back(value);
    }
    void floats(const float* values, uint32_t count)
    {
        word(values ? 1 : 0);
        if (!values)
            return;
        for (uint32_t i = 0; i < count; ++i)
            word(bits(values[i]));
    }
    void dwords(const DWORD* values, uint32_t count)
    {
        word(values ? 1 : 0);
        if (!values)
            return;
        for (uint32_t i = 0; i < count; ++i)
            word(values[i]);
    }
    // count bytes as words, the last one padded with zero bytes.
    void bytes(const void* data, uint32_t count)
    {
        word(data ? 1 : 0);
        if (!data)
            return;
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (uint32_t i = 0; i < count; i += 4)
        {
            uint32_t w = 0;
            memcpy(&w, p + i, count - i < 4 ? count - i : 4);
            word(w);
        }
    }

    bool replaying() const { return m_capture.mode == Capture::Replay; }

    // Replay: compares with the next recorded call and returns it, or null after a difference.
    const Capture::Call* replay()
    {
        Capture& c = m_capture;
        const uint32_t index = c.replayed++;
        if (c.differs)
            return nullptr;
        if (c.next >= c.calls.size())
        {
            fail(index, MethodCount, 0, 0, 0);
            return nullptr;
        }
        const Capture::Call& call = c.calls[c.next++];
        if (call.method != m_method)
        {
            fail(index, call.method, 0, 0, 0);
            return nullptr;
        }
        const uint32_t n = call.count > m_words.size() ? call.count : uint32_t(m_words.size());
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint32_t expected = i < call.count ? c.words[call.first + i] : 0;
            const uint32_t got = i < m_words.size() ? m_words[i] : 0;
            if (i >= call.count || i >= m_words.size() || expected != got)
            {
                fail(index, call.method, i, expected, got);
                return nullptr;
            }
        }
        return &call;
    }

    // Record: the call ran with this result.
    Capture::Call& done(HRESULT result)
    {
        Capture& c = m_capture;
        m_call.count = uint32_t(c.words.size()) - m_call.first;
        m_call.result = result;
        c.calls.push_back(m_call);
        return c.calls.back();
    }

private:
    void fail(uint32_t index, uint8_t expected, uint32_t wordIndex, uint32_t expectedWord, uint32_t gotWord)
    {
        Capture& c = m_capture;
        c.differs = true;
        c.differingCall = index;
        c.expected = expected;
        c.got = m_method;
        c.differingWord = wordIndex;
        c.expectedWord = expectedWord;
        c.gotWord = gotWord;
    }

    Capture& m_capture;
    uint8_t m_method;
    Capture::Call m_call;
    std::vector<uint32_t> m_words;
};

inline bool active()
{
    return s_capture && !s_paused;
}

// Guest call: the registers it starts with.
void guestCallWords(Record& r, reg32 address, const x86::CPU& cpu)
{
    r.word(address);
    r.word(cpu.eax);
    r.word(cpu.ecx);
    r.word(cpu.edx);
    r.word(cpu.ebx);
    r.word(cpu.esp);
    r.word(cpu.ebp);
    r.word(cpu.esi);
    r.word(cpu.edi);
    r.word(cpu.fpu.count);
}

void runGuest(WinApplication* app, x86::CPU& cpu, reg32 address)
{
    Record r(MethodGuestCall);
    guestCallWords(r, address, cpu);
    if (r.replaying())
    {
        const Capture::Call* call = r.replay();
        if (call)
            cpu = s_capture->cpus[call->cpu];
        else
            cpu.esp += 4;
        return;
    }
    ++s_paused;
    app->dynamic_call(address, cpu);
    --s_paused;
    Capture::Call& call = r.done(D3D_OK);
    call.cpu = uint32_t(s_capture->cpus.size());
    s_capture->cpus.push_back(cpu);
}

void callout(WinApplication* app, x86::CPU& cpu, uint32_t index)
{
    const reg32 target = s_calloutTargets[index];
    if (active())
        runGuest(app, cpu, target);
    else
        app->dynamic_call(target, cpu);
}

template< uint32_t I >
void calloutAt(WinApplication* app, x86::CPU& cpu)
{
    callout(app, cpu, I);
}

const MethodPtr s_calloutMethods[] = {
    &calloutAt<0>, &calloutAt<1>, &calloutAt<2>, &calloutAt<3>, &calloutAt<4>, &calloutAt<5>, &calloutAt<6>,
    &calloutAt<7>, &calloutAt<8>, &calloutAt<9>, &calloutAt<10>, &calloutAt<11>, &calloutAt<12>, &calloutAt<13>,
    &calloutAt<14>, &calloutAt<15>,
};
static_assert(sizeof(s_calloutMethods) / sizeof(s_calloutMethods[0]) == CalloutCount, "one method per callout");

// Vertices of a draw of count primitives.
uint32_t primitiveVertices(DWORD type, UINT count)
{
    switch (type)
    {
    case 4:  return count * 3;
    case 5:
    case 6:  return count + 2;
    default: return 0;
    }
}

// ---- Deferral ----

// Queue records: a header word (operation in bits 0-7, payload words above) and the payload, whose last word is the
// result the call returned to its caller. Operations are the capture methods and these.
enum : uint8_t
{
    OpSetTextureStageState = MethodCount,
    OpSetSamplerState,
    OpSetMatrixProduct,     // effect, handle, count, FPSCR, count matrices
    OpSetViewport,
    OpSetFVF,
    OpClear,                // device, count, flags, colour, z, stencil, rects flag, rects
    OpSetRenderTarget,
    OpSetDepthStencilSurface,
    OpDrawFrameVertices,    // device, type, minimum index, vertices, start index, count, bytes, data
    OpPresent,              // device, source and destination rectangle flags
    OpPassDraw,             // effect, pass, then the DrawIndexedPrimitive words
    OpCalls,                // CallGroup entries
    OpFpscr,                // the FPSCR the following calls run with
    OpWrap,                 // the next record is at the start of the ring
    OpCount
};
static_assert(uint32_t(OpCount) <= uint32_t(DeferOpCount),"DeferStats has a slot per operation");

const uint32_t QueueWords = 1u << 18;           // 1 MiB ring
const uint32_t QueueMask = QueueWords - 1;
const uint32_t MaxRecordWords = QueueWords / 8; // larger calls run at once
const uint32_t MaxArrayFloats = 1024;
// The queue thread makes its progress visible every this many records.
const uint32_t ReadPublishMask = 255;
// The queue thread spins this long for more calls before it sleeps; a drain spins this long before it sleeps.
const uint32_t IdleSpinUs = 10;
const uint32_t WaitSpinUs = 50;
const uint32_t SpinClockMask = 15;
const HRESULT NotReturned = HRESULT(0xffffffffu);   // result word of a call whose caller got no checked result
// Draws queued between two publications of the written records.
const uint32_t PublishDraws = 8;
const uint32_t ResultLogLimit = 10;     // mismatches logged
const uint32_t ErrorLogLimit = 3;       // calls logged per operation whose result is not D3D_OK
const uint32_t LoggedWords = 6;
const uint32_t QueueStack = 64 * 1024;

const char* opName(uint32_t op)
{
    switch (op)
    {
    case OpSetTextureStageState: return "SetTextureStageState";
    case OpSetSamplerState:      return "SetSamplerState";
    case OpSetMatrixProduct:     return "Effect::SetMatrix (product)";
    case OpSetViewport:          return "SetViewport";
    case OpSetFVF:               return "SetFVF";
    case OpClear:                return "Clear";
    case OpSetRenderTarget:      return "SetRenderTarget";
    case OpSetDepthStencilSurface: return "SetDepthStencilSurface";
    case OpDrawFrameVertices:    return "DrawIndexedPrimitive (frame vertices)";
    case OpPresent:              return "Present";
    case OpPassDraw:             return "Effect::Pass + DrawIndexedPrimitive";
    case OpCalls:                return "call group";
    default:                     return methodName(uint8_t(op));
    }
}

struct Deferral
{
    DeferMode mode = DeferOff;
    int core = 1;
    int priority = pf::Thread::PriorityHigh + 6;
    uint32_t* ring = nullptr;

    // Callers holding the execution lock.
    WinApplication* app = nullptr;
    bool owned = false;                 // the calls of the guest thread with this fs base are queued
    reg32 ownerFs = 0;
    bool inWindow = false;
    reg32 deviceGuest = 0;
    Device* device = nullptr;
    // The device's frame, stream, indices and declaration as the queued calls leave them, for the results of draws.
    bool frameOpen = false;
    bool streamBound = false, indicesBound = false, declarationBound = false;
    UINT streamStride = 0;
    DWORD fvf = 0;
    // The parameter types of the last effect looked up (effect::paramTypes), valid while the object generation is
    // the same.
    reg32 typesEffect = 0;
    uint32_t typesGeneration = 0;
    const uint8_t* types = nullptr;
    // Drains that waited, by guest method address (0: native callers; the last slot: the others).
    struct Site { reg32 address; uint32_t count; uint32_t us; };
    Site sites[DeferSiteCount] = {};
    bool frameScope = true;             // calls stay queued between windows
    uint32_t head = 0;                  // words written
    uint32_t statsHead = 0;             // head at the last statistics
    uint32_t unpublishedDraws = 0;
    uint32_t roomEnd = QueueWords;      // the head may advance to here without overwriting unrun records
    uint32_t drained = 0;               // head at the last completed drain
    uint32_t fpscr = 0;
    bool fpscrSent = false;
    uint32_t boundOnly = 0;             // objects only bindings hold (countBoundOnly)
    std::vector<Object*> deletes;       // filled by the queue run, emptied by drains

    // Shared with the queue thread.
    std::atomic<uint32_t> write{0};     // words published
    std::atomic<uint32_t> read{0};      // words run
    std::atomic<int> sleeping{0};       // the queue thread waits for wake
    std::atomic<int> waiting{0};        // a drain waits for done
    pf::Semaphore* wake = nullptr;
    pf::Semaphore* done = nullptr;
    pf::Thread* thread = nullptr;
    std::atomic<int> threadId{0};
    bool runningInline = false;
    x86::CPU runCpu;                    // passed to the queued methods, not read by them

    uint32_t* last = nullptr;           // the payload, its words and the operation of the last record put
    uint32_t lastWords = 0;
    uint32_t lastOp = 0;

    // Statistics.
    DeferStats stats = {};
    std::atomic<uint32_t> runUs{0}, mismatches{0}, passes{0};
    std::atomic<uint32_t> errors[DeferOpCount] = {};
    std::atomic<HRESULT> errorCodes[DeferOpCount] = {};
    uint32_t logged = 0;
    uint32_t errorsLogged[DeferOpCount] = {};
};

Deferral s_defer;

inline bool deferring(const x86::CPU& cpu)
{
    return s_defer.owned && s_defer.ownerFs == cpu.efs;
}

inline bool deferringDevice(const x86::CPU& cpu, reg32 guest)
{
    return deferring(cpu) && guest == s_defer.deviceGuest;
}

inline bool deferringDraw(const x86::CPU& cpu, reg32 guest)
{
    return deferringDevice(cpu, guest) && s_defer.frameOpen;
}

// Calls that change a binding: not deferred while an object is held only by bindings.
inline bool deferringBinding(const x86::CPU& cpu, reg32 guest)
{
    return deferringDevice(cpu, guest) && s_defer.boundOnly == 0;
}

// Effect calls with a parameter handle: deferred for handles, not for names.
inline bool deferringEffect(const x86::CPU& cpu, reg32 handle)
{
    return deferring(cpu) && handle < 0x10000;
}

inline uint32_t clockUs()
{
    return sceKernelGetProcessTimeLow();
}

// The queue thread, at its waiting drain.
void notifyDrain()
{
    Deferral& d = s_defer;
    if (d.waiting.load(std::memory_order_seq_cst) == 1 && d.waiting.exchange(0) == 1)
        d.done->post();
}

inline bool isKind(const Object* object, Kind kind)
{
    return object && object->kind() == kind;
}

// The type of an effect parameter handle (effect::ParamNone: not one of the effect's); game thread.
uint8_t paramType(reg32 effect, reg32 handle)
{
    Deferral& d = s_defer;
    if (effect != d.typesEffect || d.typesGeneration != g_objectGeneration || !d.types)
    {
        d.types = effect::paramTypes(effect);
        d.typesEffect = effect;
        d.typesGeneration = g_objectGeneration;
    }
    const uint32_t i = handle - effect::ParamHandleBase;
    return d.types && i < effect::ParamHandleCount ? d.types[i] : uint8_t(effect::ParamNone);
}

// Device::draw's result for a primitive type with the geometry bound.
inline HRESULT typeResult(uint32_t type)
{
    return type >= 1 && type <= 6 ? D3D_OK : D3DERR_INVALIDCALL;
}

inline bool drawBindings()
{
    const Deferral& d = s_defer;
    return d.streamBound && d.indicesBound && (d.declarationBound || d.fvf) && d.streamStride;
}

// The bindings of the device after a call that ran at once; game thread, queue empty.
void refreshBindings()
{
    Deferral& d = s_defer;
    if (!d.device)
        return;
    d.device->bindings(d.streamBound, d.streamStride, d.indicesBound, d.declarationBound, d.fvf);
    d.frameOpen = d.device->stream() != nullptr;
}

// The result of a queued call for its payload w, as the method's checks of arguments, handles, object kinds and
// bindings give it; the bindings it changes are noted. Game thread, in queue order.
HRESULT expected(uint32_t op, const uint32_t* w)
{
    Deferral& d = s_defer;
    const HRESULT invalid = D3DERR_INVALIDCALL;
    switch (op)
    {
    case MethodSetTransform:        return w[2] ? D3D_OK : invalid;
    case MethodSetMaterial:         return w[1] ? D3D_OK : invalid;
    case MethodSetLight:            return w[2] && w[1] < 8 ? D3D_OK : invalid;
    case MethodLightEnable:         return w[1] < 8 ? D3D_OK : invalid;
    case MethodSetRenderState:      return w[1] < MaxRenderStates ? D3D_OK : invalid;
    case OpSetTextureStageState:    return w[1] < MaxStages && w[2] < D3DTSS_COUNT ? D3D_OK : invalid;
    case OpSetViewport:             return w[1] ? D3D_OK : invalid;
    case MethodSetTexture:
    {
        if (w[1] >= MaxSamplerStages)
            return D3D_OK;
        const Object* t = lookup(w[2]);
        return !t || isKind(t, KindTexture) || isKind(t, KindCubeTexture) ? D3D_OK : invalid;
    }
    case MethodSetStreamSource:
    {
        if (w[1] != 0)
            return D3D_OK;
        const Object* b = lookup(w[2]);
        if (b && !isKind(b, KindVertexBuffer))
            return invalid;
        d.streamBound = b != nullptr;
        d.streamStride = w[4];
        return D3D_OK;
    }
    case MethodSetIndices:
    {
        const Object* b = lookup(w[1]);
        if (b && !isKind(b, KindIndexBuffer))
            return invalid;
        d.indicesBound = b != nullptr;
        return D3D_OK;
    }
    case MethodSetVertexDeclaration:
        d.declarationBound = lookup(w[1]) != nullptr;
        d.fvf = 0;
        return D3D_OK;
    case OpSetFVF:
        d.declarationBound = false;
        d.fvf = w[1];
        return D3D_OK;
    case OpSetRenderTarget:
    {
        if (w[1] != 0)
            return w[1] < 4 && !w[2] ? D3D_OK : invalid;
        const Surface* s = static_cast<const Surface*>(lookup(w[2]));
        if (!s || s->role() == Surface::RoleDepth
            || (s->role() == Surface::RoleLevel && (!s->container() || !s->container()->desc().target)))
            return invalid;
        return D3D_OK;
    }
    case OpSetDepthStencilSurface:
    {
        const Surface* s = static_cast<const Surface*>(lookup(w[1]));
        return s && s->role() != Surface::RoleDepth ? invalid : D3D_OK;
    }
    case MethodDrawIndexedPrimitive:
    case OpDrawFrameVertices:       return drawBindings() ? typeResult(w[1]) : invalid;
    case MethodDrawPrimitiveUP:
    {
        d.streamBound = false;
        d.streamStride = 0;
        return (d.declarationBound || d.fvf) && w[4] && w[3] ? typeResult(w[1]) : invalid;
    }
    case MethodEffectSetInt:
    case MethodEffectSetFloat:
    {
        const uint8_t t = paramType(w[0], w[1]);
        return t == effect::ParamInt || t == effect::ParamFloat ? D3D_OK : invalid;
    }
    case MethodEffectSetVector:
    case MethodEffectSetMatrix:
        return w[2] && paramType(w[0], w[1]) == effect::ParamFloat ? D3D_OK : invalid;
    case MethodEffectSetVectorArray:
    case MethodEffectSetMatrixArray:
        return w[3] && paramType(w[0], w[1]) == effect::ParamFloat ? D3D_OK : invalid;
    case OpSetMatrixProduct:
        return paramType(w[0], w[1]) == effect::ParamFloat ? D3D_OK : invalid;
    case MethodEffectSetTexture:
    {
        if (paramType(w[0], w[1]) != effect::ParamTexture)
            return invalid;
        const Object* t = lookup(w[2]);
        return !t || isKind(t, KindTexture) || isKind(t, KindCubeTexture) ? D3D_OK : invalid;
    }
    case MethodEffectPass:          return effect::checkPass(w[0], w[1]);
    default:                        return D3D_OK;
    }
}

// A drain that waited us, at a guest method address.
void noteSite(reg32 site, uint32_t us)
{
    Deferral& d = s_defer;
    const uint32_t count = sizeof(d.sites) / sizeof(d.sites[0]);
    for (uint32_t i = 0; i < count; ++i)
    {
        Deferral::Site& s = d.sites[i];
        if (s.count == 0 || s.address == site || i == count - 1)
        {
            if (s.count == 0)
                s.address = i == count - 1 ? 0xffffffffu : site;
            ++s.count;
            s.us += us;
            return;
        }
    }
}

// The record's call ran with result h and returned expected to its caller; w: its payload of length words.
void noteResult(uint32_t op, const uint32_t* w, uint32_t length, HRESULT h, HRESULT expected)
{
    Deferral& d = s_defer;
    char words[LoggedWords * 11 + 1] = {};
    const auto text = [&]
    {
        int at = 0;
        for (uint32_t i = 0; i < length && i < LoggedWords; ++i)
            at += snprintf(words + at, sizeof(words) - at, " 0x%08x", unsigned(w[i]));
        return words;
    };
    if (h != expected && expected != NotReturned)
    {
        d.mismatches.fetch_add(1, std::memory_order_relaxed);
        if (d.logged < ResultLogLimit)
        {
            ++d.logged;
            pf::logInfo("flush_async: queued %s returned 0x%08x, its caller got 0x%08x; words%s", opName(op),
                        unsigned(h), unsigned(expected), text());
        }
    }
    if (h != D3D_OK)
    {
        d.errors[op].fetch_add(1, std::memory_order_relaxed);
        d.errorCodes[op].store(h, std::memory_order_relaxed);
        if (d.errorsLogged[op] < ErrorLogLimit)
        {
            ++d.errorsLogged[op];
            pf::logInfo("flush_async: %s returned 0x%08x (its caller got 0x%08x); words%s", opName(op), unsigned(h),
                        unsigned(expected), text());
        }
    }
}

// The result a queued call returns to its caller, that of the last record put; stored as its last word. Inside a
// flush: D3D_OK, stored as NotReturned, except for calls that change what draws need bound.
inline HRESULT queued()
{
    Deferral& d = s_defer;
    const uint32_t op = d.lastOp;
    if (d.inWindow && op != MethodSetStreamSource && op != MethodSetIndices && op != MethodSetVertexDeclaration
        && op != OpSetFVF && op != MethodDrawPrimitiveUP)
    {
        d.last[d.lastWords] = NotReturned;
        return D3D_OK;
    }
    const HRESULT h = expected(op, d.last);
    d.last[d.lastWords] = h;
    return h;
}

// Runs the record at p; returns its length in words.
uint32_t runRecord(const uint32_t* p)
{
    Deferral& d = s_defer;
    WinApplication* app = d.app;
    x86::CPU& cpu = d.runCpu;
    const uint32_t op = p[0] & 0xff;
    const uint32_t length = p[0] >> 8;
    const uint32_t* w = p + 1;
    const float* f = reinterpret_cast<const float*>(w);
    const auto dev2 = [&](reg32 guest) { return guest == d.deviceGuest ? d.device : device(guest); };
    const auto dev = [&] { return dev2(w[0]); };
    HRESULT h = D3D_OK;
    switch (op)
    {
    case MethodSetTransform:
        h = dev()->SetTransform(app, cpu, w[1], w[2] ? reinterpret_cast<const D3DMATRIX*>(w + 3) : nullptr);
        break;
    case MethodSetMaterial:
        h = dev()->SetMaterial(app, cpu, w[1] ? w + 2 : nullptr);
        break;
    case MethodSetLight:
        h = dev()->SetLight(app, cpu, w[1], w[2] ? w + 3 : nullptr);
        break;
    case MethodLightEnable:
        h = dev()->LightEnable(app, cpu, w[1], BOOL(w[2]));
        break;
    case MethodSetRenderState:
        h = dev()->SetRenderState(app, cpu, w[1], w[2]);
        break;
    case OpSetTextureStageState:
        h = dev()->SetTextureStageState(app, cpu, w[1], w[2], w[3]);
        break;
    case OpSetSamplerState:
        h = dev()->SetSamplerState(app, cpu, w[1], w[2], w[3]);
        break;
    case MethodSetTexture:
        h = dev()->SetTexture(app, cpu, w[1], lookup(w[2]));
        break;
    case MethodSetVertexDeclaration:
        h = dev()->SetVertexDeclaration(app, cpu, static_cast<VertexDeclaration*>(lookup(w[1])));
        break;
    case MethodSetStreamSource:
        h = dev()->SetStreamSource(app, cpu, w[1], static_cast<Buffer*>(lookup(w[2])), w[3], w[4]);
        break;
    case MethodSetIndices:
        h = dev()->SetIndices(app, cpu, static_cast<Buffer*>(lookup(w[1])));
        break;
    case MethodDrawIndexedPrimitive:
        h = dev()->DrawIndexedPrimitive(app, cpu, w[1], LONG(w[2]), w[3], w[4], w[5], w[6]);
        break;
    case MethodDrawPrimitiveUP:
        h = dev()->DrawPrimitiveUP(app, cpu, w[1], w[2], w[4] ? w + 5 : nullptr, w[3]);
        break;
    case MethodEffectSetInt:
        h = effect::setInt(app, cpu, w[0], w[1], x86::sreg32(w[2]));
        break;
    case MethodEffectSetFloat:
        h = effect::setFloat(app, cpu, w[0], w[1], asFloat(w[2]));
        break;
    case MethodEffectSetVector:
        h = effect::setVector(app, cpu, w[0], w[1], w[2] ? f + 3 : nullptr);
        break;
    case MethodEffectSetVectorArray:
        h = effect::setVectorArray(app, cpu, w[0], w[1], w[3] ? f + 4 : nullptr, w[2]);
        break;
    case MethodEffectSetMatrix:
        h = effect::setMatrix(app, cpu, w[0], w[1], w[2] ? f + 3 : nullptr);
        break;
    case MethodEffectSetMatrixArray:
        h = effect::setMatrixArray(app, cpu, w[0], w[1], w[3] ? f + 4 : nullptr, w[2]);
        break;
    case OpSetMatrixProduct:
    {
        const float* matrices[3] = { f + 4, f + 20, f + 36 };
        h = effect::setProduct(w[0], w[1], matrices, w[2], w[3]);
        break;
    }
    case MethodEffectSetTexture:
        h = effect::setTexture(app, cpu, w[0], w[1], w[2]);
        break;
    case MethodEffectBegin:
    {
        UINT passes = 0;
        h = effect::begin(app, cpu, w[0], w[1] ? &passes : nullptr, w[2]);
        if (w[1] && passes != w[3])
        {
            d.passes.fetch_add(1, std::memory_order_relaxed);
            if (d.logged < ResultLogLimit)
            {
                ++d.logged;
                pf::logInfo("flush_async: queued Effect::Begin stored %u passes, its caller got %u", unsigned(passes),
                            unsigned(w[3]));
            }
        }
        break;
    }
    case MethodEffectPass:
        h = effect::pass(app, cpu, w[0], w[1]);
        break;
    case MethodEffectEnd:
        h = effect::end(app, cpu, w[0]);
        break;
    case OpSetViewport:
        h = dev()->SetViewport(app, cpu, w[1] ? reinterpret_cast<const D3DVIEWPORT9*>(w + 2) : nullptr);
        break;
    case OpSetFVF:
        h = dev()->SetFVF(app, cpu, w[1]);
        break;
    case OpClear:
        h = dev()->Clear(app, cpu, w[1], w[6] ? reinterpret_cast<const D3DRECT*>(w + 7) : nullptr, w[2], w[3],
                         asFloat(w[4]), w[5]);
        break;
    case OpSetRenderTarget:
        h = dev()->SetRenderTarget(app, cpu, w[1], static_cast<Surface*>(lookup(w[2])));
        break;
    case OpSetDepthStencilSurface:
        h = dev()->SetDepthStencilSurface(app, cpu, static_cast<Surface*>(lookup(w[1])));
        break;
    case OpDrawFrameVertices:
    {
        Device* const target = dev();
        void* data = target->frameVertices(app, cpu, w[6]);
        if (data)
            memcpy(data, w + 7, w[6]);
        h = target->drawIndexedFrameVertices(app, cpu, w[1], w[2], w[3], w[4], w[5], data);
        break;
    }
    case OpPresent:
    {
        // Present tests the rectangles for null only.
        const RECT rect = {};
        h = dev()->Present(app, cpu, w[1] ? &rect : nullptr, w[2] ? &rect : nullptr, w[3], w[4]);
        break;
    }
    case OpPassDraw:
    {
        const HRESULT passed = effect::pass(app, cpu, w[0], w[1]);
        if (__builtin_expect(passed != D3D_OK, 0))
            noteResult(MethodEffectPass, w, 2, passed, NotReturned);
        h = dev2(w[2])->DrawIndexedPrimitive(app, cpu, w[3], LONG(w[4]), w[5], w[6], w[7], w[8]);
        if (__builtin_expect(h != D3D_OK, 0))
            noteResult(MethodDrawIndexedPrimitive, w + 2, 7, h, NotReturned);
        h = D3D_OK;
        break;
    }
    case OpCalls:
    {
        const uint32_t* e = w;
        const uint32_t* end = w + length - 1;
        while (e < end)
        {
            const uint32_t kind = e[0] & 0xff, words = e[0] >> 8;
            const reg32 target = e[1], index = e[2];
            const uint32_t* data = e + 3;
            const float* floats = reinterpret_cast<const float*>(data);
            HRESULT r;
            uint32_t method;
            switch (kind)
            {
            case CallGroup::KindInt:
                r = effect::setInt(app, cpu, target, index, x86::sreg32(data[0]));
                method = MethodEffectSetInt;
                break;
            case CallGroup::KindFloat:
                r = effect::setFloat(app, cpu, target, index, floats[0]);
                method = MethodEffectSetFloat;
                break;
            case CallGroup::KindVector:
                r = effect::setVector(app, cpu, target, index, floats);
                method = MethodEffectSetVector;
                break;
            case CallGroup::KindVectorArray:
                r = effect::setVectorArray(app, cpu, target, index, floats, words / 4);
                method = MethodEffectSetVectorArray;
                break;
            case CallGroup::KindMatrix:
                r = effect::setMatrix(app, cpu, target, index, floats);
                method = MethodEffectSetMatrix;
                break;
            case CallGroup::KindProduct:
            {
                const float* matrices[3] = { floats + 1, floats + 17, floats + 33 };
                r = effect::setProduct(target, index, matrices, (words - 1) / 16, data[0]);
                method = OpSetMatrixProduct;
                break;
            }
            case CallGroup::KindEffectTexture:
                r = effect::setTexture(app, cpu, target, index, data[0]);
                method = MethodEffectSetTexture;
                break;
            case CallGroup::KindRenderState:
                r = dev2(target)->SetRenderState(app, cpu, index, data[0]);
                method = MethodSetRenderState;
                break;
            case CallGroup::KindDeviceTexture:
                r = dev2(target)->SetTexture(app, cpu, index, lookup(data[0]));
                method = MethodSetTexture;
                break;
            default:
                r = dev2(target)->SetTransform(app, cpu, index, reinterpret_cast<const D3DMATRIX*>(data));
                method = MethodSetTransform;
                break;
            }
            if (__builtin_expect(r != D3D_OK, 0))
                noteResult(method, e + 1, 2 + words, r, NotReturned);
            e += 3 + words;
        }
        break;
    }
    case OpFpscr:
        if (readFpscr() != w[0])
            writeFpscr(w[0]);
        break;
    default:
        pf::fatal("flush_async: queue record 0x%08x", unsigned(p[0]));
    }
    if (op != OpFpscr)
    {
        const HRESULT returned = HRESULT(w[length - 1]);
        if (__builtin_expect(h != D3D_OK || h != returned, 0))
            noteResult(op, w, length - 1, h, returned);
    }
    return 1 + length;
}

// Runs the records [from, to); returns to.
uint32_t runRecords(uint32_t from, uint32_t to, bool publish)
{
    Deferral& d = s_defer;
    uint32_t r = from, n = 0;
    while (r != to)
    {
        const uint32_t* p = d.ring + (r & QueueMask);
        if ((p[0] & 0xff) == OpWrap)
        {
            r += QueueWords - (r & QueueMask);
            continue;
        }
        r += runRecord(p);
        if (publish && (++n & ReadPublishMask) == 0)
        {
            d.read.store(r, std::memory_order_seq_cst);
            notifyDrain();
        }
    }
    return r;
}

// DeferInline: the draining thread runs everything queued.
void runInline()
{
    Deferral& d = s_defer;
    const uint32_t start = clockUs();
    const uint32_t fpscr = readFpscr();
    d.runningInline = true;
    const uint32_t r = runRecords(d.read.load(std::memory_order_relaxed), d.head, false);
    d.runningInline = false;
    if (readFpscr() != fpscr)
        writeFpscr(fpscr);
    d.read.store(r, std::memory_order_relaxed);
    const uint32_t us = clockUs() - start;
    d.runUs.fetch_add(us, std::memory_order_relaxed);
    pf::perf::event(pf::perf::EventQueueRunUs, us);
}

int queueEntry(void*)
{
    Deferral& d = s_defer;
    d.threadId.store(pf::Thread::currentId(), std::memory_order_seq_cst);
    if (pf::perf::enabled())
        pf::perf::trackThread("d3d9_queue");
    uint32_t r = d.read.load(std::memory_order_relaxed);
    for (;;)
    {
        uint32_t w = d.write.load(std::memory_order_acquire);
        if (w == r)
        {
            const uint32_t start = clockUs();
            for (uint32_t spin = 1; w == r; ++spin)
            {
                if ((spin & SpinClockMask) == 0 && clockUs() - start >= IdleSpinUs)
                    break;
                w = d.write.load(std::memory_order_acquire);
            }
            if (w == r)
            {
                d.sleeping.store(1, std::memory_order_seq_cst);
                if (d.write.load(std::memory_order_seq_cst) == r || d.sleeping.exchange(0) == 0)
                    d.wake->wait();
                continue;
            }
        }
        const uint32_t start = clockUs();
        r = runRecords(r, w, true);
        const uint32_t us = clockUs() - start;
        d.runUs.fetch_add(us, std::memory_order_relaxed);
        pf::perf::event(pf::perf::EventQueueRunUs, us);
        d.read.store(r, std::memory_order_seq_cst);
        notifyDrain();
    }
    return 0;
}

// Makes the written records visible to the queue thread and wakes it.
void publish()
{
    Deferral& d = s_defer;
    d.unpublishedDraws = 0;
    if (d.mode != DeferThread)
        return;
    d.write.store(d.head, std::memory_order_seq_cst);
    if (d.sleeping.load(std::memory_order_seq_cst) == 1 && d.sleeping.exchange(0) == 1)
        d.wake->post();
}

// Until the queue has run up to target.
void waitRead(uint32_t target)
{
    Deferral& d = s_defer;
    const auto reached = [&] { return int32_t(d.read.load(std::memory_order_acquire) - target) >= 0; };
    if (reached())
        return;
    const uint32_t start = clockUs();
    for (uint32_t spin = 1; !reached(); ++spin)
    {
        if ((spin & SpinClockMask) != 0 || clockUs() - start < WaitSpinUs)
            continue;
        d.waiting.store(1, std::memory_order_seq_cst);
        if (reached())
        {
            // The queue thread took the flag: its post is consumed.
            if (d.waiting.exchange(0) == 0)
                d.done->wait();
            break;
        }
        d.done->wait();
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint32_t us = clockUs() - start;
    d.stats.waitUs += us;
    pf::perf::event(pf::perf::EventDrainWaits, 1);
    pf::perf::event(pf::perf::EventDrainWaitUs, us);
}

// Room for words more words at the head.
void makeRoom(uint32_t words)
{
    Deferral& d = s_defer;
    if (int32_t(d.head + words - d.roomEnd) <= 0)
        return;
    d.roomEnd = d.read.load(std::memory_order_acquire) + QueueWords;
    if (int32_t(d.head + words - d.roomEnd) <= 0)
        return;
    if (d.mode == DeferInline)
    {
        runInline();
    }
    else
    {
        publish();
        waitRead(d.head + words - QueueWords);
    }
    d.roomEnd = d.read.load(std::memory_order_acquire) + QueueWords;
}

// put when the record does not fit before the ring's end or before the unrun records.
__attribute__((noinline)) uint32_t* putSlow(uint32_t op, uint32_t words)
{
    Deferral& d = s_defer;
    const uint32_t need = words + 2;
    uint32_t at = d.head & QueueMask;
    if (at + need > QueueWords)
    {
        const uint32_t skip = QueueWords - at;
        makeRoom(skip + need);
        d.ring[at] = OpWrap;
        d.head += skip;
        at = 0;
    }
    else
    {
        makeRoom(need);
    }
    uint32_t* p = d.ring + at;
    p[0] = op | (words + 1) << 8;
    d.head += need;
    ++d.stats.records[op];
    g_deferPending = true;
    d.last = p + 1;
    d.lastWords = words;
    d.lastOp = op;
    return p + 1;
}

// Appends a record of words payload words and the result word, and returns its payload.
inline uint32_t* put(uint32_t op, uint32_t words)
{
    Deferral& d = s_defer;
    const uint32_t need = words + 2;
    const uint32_t at = d.head & QueueMask;
    if (__builtin_expect(at + need > QueueWords || int32_t(d.head + need - d.roomEnd) > 0, 0))
        return putSlow(op, words);
    uint32_t* p = d.ring + at;
    p[0] = op | (words + 1) << 8;
    d.head += need;
    ++d.stats.records[op];
    g_deferPending = true;
    d.last = p + 1;
    d.lastWords = words;
    d.lastOp = op;
    return p + 1;
}

// A draw was queued: the records are published every PublishDraws draws.
inline void publishDraw()
{
    Deferral& d = s_defer;
    if (++d.unpublishedDraws >= PublishDraws)
        publish();
}

inline void copyWords(uint32_t* to, const void* from, uint32_t words)
{
    memcpy(to, from, size_t(words) * 4);
}

// A call of an open window that runs at once.
inline void fallback()
{
    ++s_defer.stats.fallbacks;
    drain();
}

void startThread()
{
    Deferral& d = s_defer;
    d.wake = new pf::Semaphore("d3d9_queue_wake", 0);
    d.done = new pf::Semaphore("d3d9_queue_done", 0);
    d.thread = new pf::Thread("d3d9_queue", &queueEntry, nullptr, d.priority, QueueStack,
                              pf::Thread::Cpu0 << d.core);
    d.thread->start();
    pf::logInfo("flush_async: queue thread on core %d, priority %d", d.core, d.priority);
}

}

void Capture::clear()
{
    mode = Record;
    words.clear();
    calls.clear();
    cpus.clear();
    next = 0;
    replayed = 0;
    differs = false;
    differingCall = 0;
    expected = got = MethodCount;
    differingWord = expectedWord = gotWord = 0;
}

const char* methodName(uint8_t method)
{
    return method < MethodCount ? s_methodNames[method] : "none";
}

void begin(Capture& capture, Capture::Mode mode)
{
    drain();
    capture.mode = mode;
    capture.next = 0;
    capture.replayed = 0;
    s_capture = &capture;
    s_paused = 0;
}

void end()
{
    s_capture = nullptr;
}

bool capturing()
{
    return s_capture != nullptr;
}

reg32 calloutAddress(uint32_t index)
{
    return s_calloutMemory->getBlockStart() + 4 * index;
}

void setCalloutTarget(uint32_t index, reg32 target)
{
    s_calloutTargets[index] = target;
}

void init(WinApplication* app)
{
    s_app = app;
    s_calloutMemory = new MemMap(4096);
    for (uint32_t i = 0; i < CalloutCount; ++i)
    {
        app->getMemory<x86::reg32>(calloutAddress(i)) = calloutAddress(i);
        char name[32];
        snprintf(name, sizeof(name), "d3d9 callout %u", unsigned(i));
        app->registerMethod(calloutAddress(i), { name, s_calloutMethods[i] });
    }
}

void callGuest(WinApplication* app, x86::CPU& cpu, reg32 address)
{
    if (active())
        runGuest(app, cpu, address);
    else
        app->dynamic_call(address, cpu);
}

// ---- Deferral ----

void configureDeferral(DeferMode mode, bool frame, int core, int priority)
{
    Deferral& d = s_defer;
    if (d.ring)
        return;
    d.mode = mode;
    d.frameScope = frame;
    d.core = core;
    d.priority = priority;
    if (mode != DeferOff)
        d.ring = new uint32_t[QueueWords];
}

bool deferBegin(WinApplication* app, x86::CPU& cpu, reg32 guest)
{
    Deferral& d = s_defer;
    if (d.mode == DeferOff || d.inWindow || s_capture || (d.owned && d.ownerFs != cpu.efs))
        return false;
    if (!d.device)
    {
        Object* object = lookup(guest);
        if (!object || object->kind() != KindDevice)
            return false;
        if (!d.app)
        {
            d.app = app;
            d.runCpu = cpu;
            if (d.mode == DeferThread)
                startThread();
        }
        d.deviceGuest = guest;
        d.device = static_cast<Device*>(object);
        refreshBindings();
    }
    else if (guest != d.deviceGuest)
    {
        return false;
    }
    const uint32_t fpscr = readFpscr();
    if (!d.fpscrSent || fpscr != d.fpscr)
    {
        put(OpFpscr, 1)[0] = fpscr;
        d.fpscr = fpscr;
        d.fpscrSent = true;
    }
    d.owned = true;
    d.ownerFs = cpu.efs;
    d.inWindow = true;
    ++d.stats.windows;
    return true;
}

void deferEnd()
{
    Deferral& d = s_defer;
    publish();
    d.inWindow = false;
    if (!d.frameScope)
        d.owned = false;
}

void afterDirect()
{
    refreshBindings();
}

void deviceGone(const Device* device)
{
    Deferral& d = s_defer;
    if (device != d.device)
        return;
    d.owned = false;
    d.inWindow = false;
    d.device = nullptr;
    d.deviceGuest = 0;
}

void drainSlow(reg32 site)
{
    Deferral& d = s_defer;
    if (d.head != d.drained)
    {
        if (d.mode == DeferInline)
        {
            runInline();
        }
        else if (int32_t(d.read.load(std::memory_order_acquire) - d.head) < 0)
        {
            publish();
            ++d.stats.drains;
            if (d.inWindow)
                ++d.stats.windowDrains;
            const uint32_t start = clockUs();
            waitRead(d.head);
            noteSite(site, clockUs() - start);
        }
        else
        {
            std::atomic_thread_fence(std::memory_order_acquire);
        }
        d.drained = d.head;
    }
    for (Object* object : d.deletes)
        delete object;
    d.stats.deletes += uint32_t(d.deletes.size());
    d.deletes.clear();
    g_deferPending = false;
}

DeferStats takeDeferStats()
{
    Deferral& d = s_defer;
    DeferStats s = d.stats;
    s.words = d.head - d.statsHead;
    d.statsHead = d.head;
    s.calls = 0;
    for (uint32_t i = 0; i < DeferOpCount; ++i)
        s.calls += s.records[i];
    s.runUs = d.runUs.exchange(0, std::memory_order_relaxed);
    s.mismatches = d.mismatches.exchange(0, std::memory_order_relaxed);
    s.passes = d.passes.exchange(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < DeferOpCount; ++i)
    {
        s.errors[i] = d.errors[i].exchange(0, std::memory_order_relaxed);
        s.errorCodes[i] = d.errorCodes[i].load(std::memory_order_relaxed);
    }
    for (uint32_t i = 0; i < DeferSiteCount; ++i)
    {
        s.sites[i] = DeferStats::Site{ d.sites[i].address, d.sites[i].count, d.sites[i].us };
        d.sites[i] = Deferral::Site{};
    }
    d.stats = DeferStats{};
    return s;
}

const char* deferOpName(uint32_t op)
{
    return opName(op);
}

const char* drainSiteName(reg32 address)
{
    if (address == 0)
        return "native callers";
    if (address == 0xffffffffu)
        return "others";
    const char* name = s_app ? s_app->getMethodName(address) : nullptr;
    return name ? name : "?";
}

bool onQueueThread()
{
    const Deferral& d = s_defer;
    if (d.runningInline)
        return true;
    const int id = d.threadId.load(std::memory_order_relaxed);
    return id != 0 && pf::Thread::currentId() == id;
}

void deleteLater(Object* object)
{
    s_defer.deletes.push_back(object);
}

void countBoundOnly(int delta)
{
    s_defer.boundOnly += uint32_t(delta);
}

// ---- Device ----

// The calls that are not queued, of the methods called most.
#define NFSU_DIRECT static __attribute__((noinline)) HRESULT
NFSU_DIRECT setRenderStateDirect(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD state, DWORD value);
NFSU_DIRECT setTextureDirect(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD stage, reg32 texture);
NFSU_DIRECT drawIndexedPrimitiveDirect(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD type, LONG baseVertex,
                                       UINT minIndex, UINT vertices, UINT startIndex, UINT count);
NFSU_DIRECT effectSetIntDirect(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, x86::sreg32 value);
NFSU_DIRECT effectSetFloatDirect(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, float value);
NFSU_DIRECT effectSetVectorDirect(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, const float* vector);
NFSU_DIRECT effectSetVectorArrayDirect(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle,
                                       const float* vectors, UINT count);
NFSU_DIRECT effectSetMatrixDirect(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, const float* matrix);
NFSU_DIRECT effectSetMatrixProductDirect(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle,
                                         const float* a, const float* b, const float* c);
NFSU_DIRECT effectSetTextureDirect(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, reg32 texture);
NFSU_DIRECT effectPassDirect(WinApplication* app, x86::CPU& cpu, reg32 guest, UINT pass);
#undef NFSU_DIRECT

HRESULT setTransform(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD state, const float* matrix)
{
    if (deferringDevice(cpu, guest))
    {
        uint32_t* w = put(MethodSetTransform, matrix ? 19 : 3);
        w[0] = guest;
        w[1] = state;
        w[2] = matrix ? 1 : 0;
        if (matrix)
            copyWords(w + 3, matrix, 16);
        return queued();
    }
    drain();
    if (active())
    {
        Record r(MethodSetTransform);
        r.word(guest);
        r.word(state);
        r.floats(matrix, 16);
        if (r.replaying())
        {
            const Capture::Call* call = r.replay();
            return call ? call->result : D3D_OK;
        }
        const HRESULT h = device(guest)->SetTransform(app, cpu, state, reinterpret_cast<const D3DMATRIX*>(matrix));
        r.done(h);
        return h;
    }
    return device(guest)->SetTransform(app, cpu, state, reinterpret_cast<const D3DMATRIX*>(matrix));
}

HRESULT setMaterial(WinApplication* app, x86::CPU& cpu, reg32 guest, const DWORD* material)
{
    if (deferringDevice(cpu, guest))
    {
        uint32_t* w = put(MethodSetMaterial, material ? 19 : 2);
        w[0] = guest;
        w[1] = material ? 1 : 0;
        if (material)
            copyWords(w + 2, material, 17);
        return queued();
    }
    drain();
    if (active())
    {
        Record r(MethodSetMaterial);
        r.word(guest);
        r.dwords(material, 17);
        if (r.replaying())
        {
            const Capture::Call* call = r.replay();
            return call ? call->result : D3D_OK;
        }
        const HRESULT h = device(guest)->SetMaterial(app, cpu, material);
        r.done(h);
        return h;
    }
    return device(guest)->SetMaterial(app, cpu, material);
}

HRESULT setLight(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD index, const DWORD* light)
{
    if (deferringDevice(cpu, guest))
    {
        uint32_t* w = put(MethodSetLight, light ? 29 : 3);
        w[0] = guest;
        w[1] = index;
        w[2] = light ? 1 : 0;
        if (light)
            copyWords(w + 3, light, 26);
        return queued();
    }
    drain();
    if (active())
    {
        Record r(MethodSetLight);
        r.word(guest);
        r.word(index);
        r.dwords(light, 26);
        if (r.replaying())
        {
            const Capture::Call* call = r.replay();
            return call ? call->result : D3D_OK;
        }
        const HRESULT h = device(guest)->SetLight(app, cpu, index, light);
        r.done(h);
        return h;
    }
    return device(guest)->SetLight(app, cpu, index, light);
}

HRESULT lightEnable(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD index, BOOL enable)
{
    if (deferringDevice(cpu, guest))
    {
        uint32_t* w = put(MethodLightEnable, 3);
        w[0] = guest;
        w[1] = index;
        w[2] = reg32(enable);
        return queued();
    }
    drain();
    if (active())
    {
        Record r(MethodLightEnable);
        r.word(guest);
        r.word(index);
        r.word(enable);
        if (r.replaying())
        {
            const Capture::Call* call = r.replay();
            return call ? call->result : D3D_OK;
        }
        const HRESULT h = device(guest)->LightEnable(app, cpu, index, enable);
        r.done(h);
        return h;
    }
    return device(guest)->LightEnable(app, cpu, index, enable);
}

HRESULT setRenderState(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD state, DWORD value)
{
    if (deferringDevice(cpu, guest))
    {
        uint32_t* w = put(MethodSetRenderState, 3);
        w[0] = guest;
        w[1] = state;
        w[2] = value;
        return queued();
    }
    return setRenderStateDirect(app, cpu, guest, state, value);
}

HRESULT setRenderStateDirect(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD state, DWORD value)
{
    drain();
    if (active())
    {
        Record r(MethodSetRenderState);
        r.word(guest);
        r.word(state);
        r.word(value);
        if (r.replaying())
        {
            const Capture::Call* call = r.replay();
            return call ? call->result : D3D_OK;
        }
        const HRESULT h = device(guest)->SetRenderState(app, cpu, state, value);
        r.done(h);
        return h;
    }
    return device(guest)->SetRenderState(app, cpu, state, value);
}

HRESULT setTextureStageState(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD stage, DWORD type, DWORD value)
{
    if (deferringDevice(cpu, guest))
    {
        uint32_t* w = put(OpSetTextureStageState, 4);
        w[0] = guest;
        w[1] = stage;
        w[2] = type;
        w[3] = value;
        return queued();
    }
    drain();
    return device(guest)->SetTextureStageState(app, cpu, stage, type, value);
}

HRESULT setSamplerState(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD sampler, DWORD type, DWORD value)
{
    if (deferringDevice(cpu, guest))
    {
        uint32_t* w = put(OpSetSamplerState, 4);
        w[0] = guest;
        w[1] = sampler;
        w[2] = type;
        w[3] = value;
        return queued();
    }
    drain();
    return device(guest)->SetSamplerState(app, cpu, sampler, type, value);
}

HRESULT setViewport(WinApplication* app, x86::CPU& cpu, reg32 guest, const DWORD* viewport)
{
    if (deferringDevice(cpu, guest))
    {
        uint32_t* w = put(OpSetViewport, viewport ? 8 : 2);
        w[0] = guest;
        w[1] = viewport ? 1 : 0;
        if (viewport)
            copyWords(w + 2, viewport, 6);
        return queued();
    }
    drain();
    return device(guest)->SetViewport(app, cpu, reinterpret_cast<const D3DVIEWPORT9*>(viewport));
}

HRESULT setFVF(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD fvf)
{
    if (deferringBinding(cpu, guest))
    {
        uint32_t* w = put(OpSetFVF, 2);
        w[0] = guest;
        w[1] = fvf;
        return queued();
    }
    if (deferring(cpu))
        fallback();
    drain();
    const HRESULT h = device(guest)->SetFVF(app, cpu, fvf);
    refreshBindings();
    return h;
}

HRESULT clear(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD count, const DWORD* rects, DWORD flags,
              DWORD color, float z, DWORD stencil)
{
    const uint32_t words = rects && count ? count * 4 : 0;
    if (deferringDraw(cpu, guest) && words <= MaxArrayFloats)
    {
        uint32_t* w = put(OpClear, 7 + words);
        w[0] = guest;
        w[1] = count;
        w[2] = flags;
        w[3] = color;
        w[4] = bits(z);
        w[5] = stencil;
        w[6] = rects ? 1 : 0;
        if (words)
            copyWords(w + 7, rects, words);
        const HRESULT h = queued();
        publishDraw();
        return h;
    }
    if (deferring(cpu))
        fallback();
    drain();
    const HRESULT h = device(guest)->Clear(app, cpu, count, reinterpret_cast<const D3DRECT*>(rects), flags, color, z,
                                           stencil);
    refreshBindings();
    return h;
}

HRESULT beginScene(WinApplication* app, x86::CPU& cpu, reg32 guest)
{
    if (deferringDraw(cpu, guest))
        return D3D_OK;
    drain();
    const HRESULT h = device(guest)->BeginScene(app, cpu);
    refreshBindings();
    return h;
}

HRESULT endScene(WinApplication* app, x86::CPU& cpu, reg32 guest)
{
    return device(guest)->EndScene(app, cpu);
}

HRESULT setRenderTarget(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD index, reg32 surface)
{
    if (deferringBinding(cpu, guest))
    {
        uint32_t* w = put(OpSetRenderTarget, 3);
        w[0] = guest;
        w[1] = index;
        w[2] = surface;
        return queued();
    }
    if (deferring(cpu))
        fallback();
    drain();
    return device(guest)->SetRenderTarget(app, cpu, index, static_cast<Surface*>(lookup(surface)));
}

HRESULT setDepthStencilSurface(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 surface)
{
    if (deferringBinding(cpu, guest))
    {
        uint32_t* w = put(OpSetDepthStencilSurface, 2);
        w[0] = guest;
        w[1] = surface;
        return queued();
    }
    if (deferring(cpu))
        fallback();
    drain();
    return device(guest)->SetDepthStencilSurface(app, cpu, static_cast<Surface*>(lookup(surface)));
}

HRESULT drawFrameVertices(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD type, UINT minIndex, UINT vertices,
                          UINT startIndex, UINT count, const void* data, uint32_t bytes)
{
    const uint32_t words = (bytes + 3) / 4;
    if (deferringDraw(cpu, guest) && words <= MaxRecordWords)
    {
        uint32_t* w = put(OpDrawFrameVertices, 7 + words);
        w[0] = guest;
        w[1] = type;
        w[2] = minIndex;
        w[3] = vertices;
        w[4] = startIndex;
        w[5] = count;
        w[6] = bytes;
        if (words)
        {
            w[6 + words] = 0;
            memcpy(w + 7, data, bytes);
        }
        const HRESULT h = queued();
        publishDraw();
        return h;
    }
    if (deferring(cpu))
        fallback();
    drain();
    Device* d = device(guest);
    void* frame = d->frameVertices(app, cpu, bytes);
    if (cpu.terminate)
        return D3D_OK;
    if (frame)
        memcpy(frame, data, bytes);
    const HRESULT h = d->drawIndexedFrameVertices(app, cpu, type, minIndex, vertices, startIndex, count, frame);
    refreshBindings();
    return h;
}

HRESULT present(WinApplication* app, x86::CPU& cpu, reg32 guest, const void* source, const void* dest, reg32 window,
                reg32 dirtyRegion)
{
    if (deferringDraw(cpu, guest))
    {
        Texture::trimKept();
        uint32_t* w = put(OpPresent, 5);
        w[0] = guest;
        w[1] = source ? 1 : 0;
        w[2] = dest ? 1 : 0;
        w[3] = window;
        w[4] = dirtyRegion;
        s_defer.frameOpen = false;
        const HRESULT h = queued();
        publish();
        return h;
    }
    if (deferring(cpu))
        fallback();
    drain();
    const HRESULT h = device(guest)->Present(app, cpu, static_cast<const RECT*>(source),
                                             static_cast<const RECT*>(dest), window, dirtyRegion);
    refreshBindings();
    return h;
}

HRESULT passDrawIndexed(WinApplication* app, x86::CPU& cpu, reg32 effect, UINT pass, reg32 guest, DWORD type,
                        LONG baseVertex, UINT minIndex, UINT vertices, UINT startIndex, UINT count)
{
    if (s_defer.inWindow && deferringDraw(cpu, guest))
    {
        uint32_t* w = put(OpPassDraw, 9);
        s_defer.stats.compactCalls += 2;
        w[0] = effect;
        w[1] = pass;
        w[2] = guest;
        w[3] = type;
        w[4] = reg32(baseVertex);
        w[5] = minIndex;
        w[6] = vertices;
        w[7] = startIndex;
        w[8] = count;
        const HRESULT h = queued();
        publishDraw();
        return h;
    }
    effectPass(app, cpu, effect, pass);
    return drawIndexedPrimitive(app, cpu, guest, type, baseVertex, minIndex, vertices, startIndex, count);
}

// ---- CallGroup ----

CallGroup::CallGroup(WinApplication* app, x86::CPU& cpu)
    :   m_app(app), m_cpu(cpu), m_queued(s_defer.inWindow && deferring(cpu)), m_bindings(s_defer.boundOnly == 0),
        m_device(s_defer.deviceGuest), m_fpscr(s_defer.fpscr)
{
}

void CallGroup::queue()
{
    if (!m_size)
        return;
    uint32_t* w = put(OpCalls, m_size);
    copyWords(w, m_words, m_size);
    queued();
    s_defer.stats.compactCalls += m_count;
    m_size = 0;
    m_count = 0;
}

HRESULT CallGroup::effectSetIntAtOnce(reg32 effect, reg32 handle, x86::sreg32 value)
{
    queue();
    return effectSetInt(m_app, m_cpu, effect, handle, value);
}

HRESULT CallGroup::effectSetFloatAtOnce(reg32 effect, reg32 handle, float value)
{
    queue();
    return effectSetFloat(m_app, m_cpu, effect, handle, value);
}

HRESULT CallGroup::effectSetVectorAtOnce(reg32 effect, reg32 handle, const float* vector)
{
    queue();
    return effectSetVector(m_app, m_cpu, effect, handle, vector);
}

HRESULT CallGroup::effectSetVectorArrayAtOnce(reg32 effect, reg32 handle, const float* vectors, UINT count)
{
    queue();
    return effectSetVectorArray(m_app, m_cpu, effect, handle, vectors, count);
}

HRESULT CallGroup::effectSetMatrixAtOnce(reg32 effect, reg32 handle, const float* matrix)
{
    queue();
    return effectSetMatrix(m_app, m_cpu, effect, handle, matrix);
}

HRESULT CallGroup::effectSetMatrixProductAtOnce(reg32 effect, reg32 handle, const float* a, const float* b,
                                                const float* c)
{
    queue();
    return effectSetMatrixProduct(m_app, m_cpu, effect, handle, a, b, c);
}

HRESULT CallGroup::effectSetTextureAtOnce(reg32 effect, reg32 handle, reg32 texture)
{
    queue();
    return effectSetTexture(m_app, m_cpu, effect, handle, texture);
}

HRESULT CallGroup::setRenderStateAtOnce(reg32 device, DWORD state, DWORD value)
{
    queue();
    return bridge::setRenderState(m_app, m_cpu, device, state, value);
}

HRESULT CallGroup::setTextureAtOnce(reg32 device, DWORD stage, reg32 texture)
{
    queue();
    return bridge::setTexture(m_app, m_cpu, device, stage, texture);
}

HRESULT CallGroup::setTransformAtOnce(reg32 device, DWORD state, const float* matrix)
{
    queue();
    return bridge::setTransform(m_app, m_cpu, device, state, matrix);
}

HRESULT setTexture(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD stage, reg32 texture)
{
    if (deferringBinding(cpu, guest))
    {
        uint32_t* w = put(MethodSetTexture, 3);
        w[0] = guest;
        w[1] = stage;
        w[2] = texture;
        return queued();
    }
    return setTextureDirect(app, cpu, guest, stage, texture);
}

HRESULT setTextureDirect(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD stage, reg32 texture)
{
    if (deferring(cpu))
        fallback();
    drain();
    if (active())
    {
        Record r(MethodSetTexture);
        r.word(guest);
        r.word(stage);
        r.word(texture);
        if (r.replaying())
        {
            const Capture::Call* call = r.replay();
            return call ? call->result : D3D_OK;
        }
        const HRESULT h = device(guest)->SetTexture(app, cpu, stage, lookup(texture));
        r.done(h);
        return h;
    }
    return device(guest)->SetTexture(app, cpu, stage, lookup(texture));
}

HRESULT setVertexDeclaration(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 declaration)
{
    if (deferringBinding(cpu, guest))
    {
        uint32_t* w = put(MethodSetVertexDeclaration, 2);
        w[0] = guest;
        w[1] = declaration;
        return queued();
    }
    if (deferring(cpu))
        fallback();
    drain();
    if (active())
    {
        Record r(MethodSetVertexDeclaration);
        r.word(guest);
        r.word(declaration);
        if (r.replaying())
        {
            const Capture::Call* call = r.replay();
            return call ? call->result : D3D_OK;
        }
        const HRESULT h = device(guest)->SetVertexDeclaration(
            app, cpu, static_cast<VertexDeclaration*>(lookup(declaration)));
        r.done(h);
        return h;
    }
    const HRESULT h = device(guest)->SetVertexDeclaration(app, cpu,
                                                          static_cast<VertexDeclaration*>(lookup(declaration)));
    refreshBindings();
    return h;
}

HRESULT setStreamSource(WinApplication* app, x86::CPU& cpu, reg32 guest, UINT stream, reg32 buffer, UINT offset,
                        UINT stride)
{
    if (deferringBinding(cpu, guest))
    {
        uint32_t* w = put(MethodSetStreamSource, 5);
        w[0] = guest;
        w[1] = stream;
        w[2] = buffer;
        w[3] = offset;
        w[4] = stride;
        return queued();
    }
    if (deferring(cpu))
        fallback();
    drain();
    if (active())
    {
        Record r(MethodSetStreamSource);
        r.word(guest);
        r.word(stream);
        r.word(buffer);
        r.word(offset);
        r.word(stride);
        if (r.replaying())
        {
            const Capture::Call* call = r.replay();
            return call ? call->result : D3D_OK;
        }
        const HRESULT h = device(guest)->SetStreamSource(app, cpu, stream, static_cast<Buffer*>(lookup(buffer)),
                                                         offset, stride);
        r.done(h);
        return h;
    }
    const HRESULT h = device(guest)->SetStreamSource(app, cpu, stream, static_cast<Buffer*>(lookup(buffer)), offset,
                                                     stride);
    refreshBindings();
    return h;
}

HRESULT setIndices(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 buffer)
{
    if (deferringBinding(cpu, guest))
    {
        uint32_t* w = put(MethodSetIndices, 2);
        w[0] = guest;
        w[1] = buffer;
        return queued();
    }
    if (deferring(cpu))
        fallback();
    drain();
    if (active())
    {
        Record r(MethodSetIndices);
        r.word(guest);
        r.word(buffer);
        if (r.replaying())
        {
            const Capture::Call* call = r.replay();
            return call ? call->result : D3D_OK;
        }
        const HRESULT h = device(guest)->SetIndices(app, cpu, static_cast<Buffer*>(lookup(buffer)));
        r.done(h);
        return h;
    }
    const HRESULT h = device(guest)->SetIndices(app, cpu, static_cast<Buffer*>(lookup(buffer)));
    refreshBindings();
    return h;
}

HRESULT drawIndexedPrimitive(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD type, LONG baseVertex,
                             UINT minIndex, UINT vertices, UINT startIndex, UINT count)
{
    if (deferringDraw(cpu, guest))
    {
        uint32_t* w = put(MethodDrawIndexedPrimitive, 7);
        w[0] = guest;
        w[1] = type;
        w[2] = reg32(baseVertex);
        w[3] = minIndex;
        w[4] = vertices;
        w[5] = startIndex;
        w[6] = count;
        const HRESULT h = queued();
        publishDraw();
        return h;
    }
    return drawIndexedPrimitiveDirect(app, cpu, guest, type, baseVertex, minIndex, vertices, startIndex, count);
}

HRESULT drawIndexedPrimitiveDirect(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD type, LONG baseVertex,
                                   UINT minIndex, UINT vertices, UINT startIndex, UINT count)
{
    if (deferring(cpu))
        fallback();
    drain();
    if (active())
    {
        Record r(MethodDrawIndexedPrimitive);
        r.word(guest);
        r.word(type);
        r.word(reg32(baseVertex));
        r.word(minIndex);
        r.word(vertices);
        r.word(startIndex);
        r.word(count);
        if (r.replaying())
        {
            const Capture::Call* call = r.replay();
            return call ? call->result : D3D_OK;
        }
        const HRESULT h = device(guest)->DrawIndexedPrimitive(app, cpu, type, baseVertex, minIndex, vertices,
                                                              startIndex, count);
        r.done(h);
        return h;
    }
    const HRESULT h = device(guest)->DrawIndexedPrimitive(app, cpu, type, baseVertex, minIndex, vertices, startIndex,
                                                          count);
    refreshBindings();
    return h;
}

HRESULT drawPrimitiveUP(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD type, UINT count, const void* data,
                        UINT stride)
{
    const uint32_t bytes = primitiveVertices(type, count) * stride;
    const uint32_t words = (bytes + 3) / 4;
    if (deferringDraw(cpu, guest) && s_defer.boundOnly == 0 && words <= MaxRecordWords)
    {
        uint32_t* w = put(MethodDrawPrimitiveUP, data ? 5 + words : 5);
        w[0] = guest;
        w[1] = type;
        w[2] = count;
        w[3] = stride;
        w[4] = data ? 1 : 0;
        if (data && words)
        {
            w[4 + words] = 0;
            memcpy(w + 5, data, bytes);
        }
        const HRESULT h = queued();
        publishDraw();
        return h;
    }
    if (deferring(cpu))
        fallback();
    drain();
    if (active())
    {
        Record r(MethodDrawPrimitiveUP);
        r.word(guest);
        r.word(type);
        r.word(count);
        r.word(stride);
        r.bytes(data, bytes);
        if (r.replaying())
        {
            const Capture::Call* call = r.replay();
            return call ? call->result : D3D_OK;
        }
        const HRESULT h = device(guest)->DrawPrimitiveUP(app, cpu, type, count, data, stride);
        r.done(h);
        return h;
    }
    const HRESULT h = device(guest)->DrawPrimitiveUP(app, cpu, type, count, data, stride);
    refreshBindings();
    return h;
}

// ---- Effect ----

HRESULT effectSetInt(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, x86::sreg32 value)
{
    if (deferringEffect(cpu, handle))
    {
        uint32_t* w = put(MethodEffectSetInt, 3);
        w[0] = guest;
        w[1] = handle;
        w[2] = reg32(value);
        return queued();
    }
    return effectSetIntDirect(app, cpu, guest, handle, value);
}

HRESULT effectSetIntDirect(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, x86::sreg32 value)
{
    if (deferring(cpu))
        fallback();
    drain();
    if (active())
    {
        Record r(MethodEffectSetInt);
        r.word(guest);
        r.word(handle);
        r.word(reg32(value));
        if (r.replaying())
        {
            const Capture::Call* call = r.replay();
            return call ? call->result : D3D_OK;
        }
        const HRESULT h = effect::setInt(app, cpu, guest, handle, value);
        r.done(h);
        return h;
    }
    return effect::setInt(app, cpu, guest, handle, value);
}

HRESULT effectSetFloat(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, float value)
{
    if (deferringEffect(cpu, handle))
    {
        uint32_t* w = put(MethodEffectSetFloat, 3);
        w[0] = guest;
        w[1] = handle;
        w[2] = bits(value);
        return queued();
    }
    return effectSetFloatDirect(app, cpu, guest, handle, value);
}

HRESULT effectSetFloatDirect(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, float value)
{
    if (deferring(cpu))
        fallback();
    drain();
    if (active())
    {
        Record r(MethodEffectSetFloat);
        r.word(guest);
        r.word(handle);
        r.word(bits(value));
        if (r.replaying())
        {
            const Capture::Call* call = r.replay();
            return call ? call->result : D3D_OK;
        }
        const HRESULT h = effect::setFloat(app, cpu, guest, handle, value);
        r.done(h);
        return h;
    }
    return effect::setFloat(app, cpu, guest, handle, value);
}

HRESULT effectSetVector(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, const float* vector)
{
    if (deferringEffect(cpu, handle))
    {
        uint32_t* w = put(MethodEffectSetVector, vector ? 7 : 3);
        w[0] = guest;
        w[1] = handle;
        w[2] = vector ? 1 : 0;
        if (vector)
            copyWords(w + 3, vector, 4);
        return queued();
    }
    return effectSetVectorDirect(app, cpu, guest, handle, vector);
}

HRESULT effectSetVectorDirect(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, const float* vector)
{
    if (deferring(cpu))
        fallback();
    drain();
    if (active())
    {
        Record r(MethodEffectSetVector);
        r.word(guest);
        r.word(handle);
        r.floats(vector, 4);
        if (r.replaying())
        {
            const Capture::Call* call = r.replay();
            return call ? call->result : D3D_OK;
        }
        const HRESULT h = effect::setVector(app, cpu, guest, handle, vector);
        r.done(h);
        return h;
    }
    return effect::setVector(app, cpu, guest, handle, vector);
}

HRESULT effectSetVectorArray(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, const float* vectors,
                             UINT count)
{
    if (deferringEffect(cpu, handle) && count <= MaxArrayFloats / 4)
    {
        const uint32_t floats = vectors ? count * 4 : 0;
        uint32_t* w = put(MethodEffectSetVectorArray, 4 + floats);
        w[0] = guest;
        w[1] = handle;
        w[2] = count;
        w[3] = vectors ? 1 : 0;
        if (vectors)
            copyWords(w + 4, vectors, floats);
        return queued();
    }
    return effectSetVectorArrayDirect(app, cpu, guest, handle, vectors, count);
}

HRESULT effectSetVectorArrayDirect(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, const float* vectors,
                                   UINT count)
{
    if (deferring(cpu))
        fallback();
    drain();
    if (active())
    {
        Record r(MethodEffectSetVectorArray);
        r.word(guest);
        r.word(handle);
        r.word(count);
        r.floats(vectors, count <= 64 ? count * 4 : 256);
        if (r.replaying())
        {
            const Capture::Call* call = r.replay();
            return call ? call->result : D3D_OK;
        }
        const HRESULT h = effect::setVectorArray(app, cpu, guest, handle, vectors, count);
        r.done(h);
        return h;
    }
    return effect::setVectorArray(app, cpu, guest, handle, vectors, count);
}

HRESULT effectSetMatrix(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, const float* matrix)
{
    if (deferringEffect(cpu, handle))
    {
        uint32_t* w = put(MethodEffectSetMatrix, matrix ? 19 : 3);
        w[0] = guest;
        w[1] = handle;
        w[2] = matrix ? 1 : 0;
        if (matrix)
            copyWords(w + 3, matrix, 16);
        return queued();
    }
    return effectSetMatrixDirect(app, cpu, guest, handle, matrix);
}

HRESULT effectSetMatrixDirect(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, const float* matrix)
{
    if (deferring(cpu))
        fallback();
    drain();
    if (active())
    {
        Record r(MethodEffectSetMatrix);
        r.word(guest);
        r.word(handle);
        r.floats(matrix, 16);
        if (r.replaying())
        {
            const Capture::Call* call = r.replay();
            return call ? call->result : D3D_OK;
        }
        const HRESULT h = effect::setMatrix(app, cpu, guest, handle, matrix);
        r.done(h);
        return h;
    }
    return effect::setMatrix(app, cpu, guest, handle, matrix);
}

HRESULT effectSetMatrixArray(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, const float* matrices,
                             UINT count)
{
    if (deferringEffect(cpu, handle) && count <= MaxArrayFloats / 16)
    {
        const uint32_t floats = matrices ? count * 16 : 0;
        uint32_t* w = put(MethodEffectSetMatrixArray, 4 + floats);
        w[0] = guest;
        w[1] = handle;
        w[2] = count;
        w[3] = matrices ? 1 : 0;
        if (matrices)
            copyWords(w + 4, matrices, floats);
        return queued();
    }
    if (deferring(cpu))
        fallback();
    drain();
    if (active())
    {
        Record r(MethodEffectSetMatrixArray);
        r.word(guest);
        r.word(handle);
        r.word(count);
        r.floats(matrices, count <= 16 ? count * 16 : 256);
        if (r.replaying())
        {
            const Capture::Call* call = r.replay();
            return call ? call->result : D3D_OK;
        }
        const HRESULT h = effect::setMatrixArray(app, cpu, guest, handle, matrices, count);
        r.done(h);
        return h;
    }
    return effect::setMatrixArray(app, cpu, guest, handle, matrices, count);
}

HRESULT effectSetMatrixProduct(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, const float* a,
                               const float* b, const float* c)
{
    const uint32_t count = c ? 3 : 2;
    if (deferringEffect(cpu, handle))
    {
        uint32_t* w = put(OpSetMatrixProduct, 4 + 16 * count);
        w[0] = guest;
        w[1] = handle;
        w[2] = count;
        w[3] = s_defer.fpscr;
        copyWords(w + 4, a, 16);
        copyWords(w + 20, b, 16);
        if (c)
            copyWords(w + 36, c, 16);
        return queued();
    }
    return effectSetMatrixProductDirect(app, cpu, guest, handle, a, b, c);
}

HRESULT effectSetMatrixProductDirect(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, const float* a,
                                     const float* b, const float* c)
{
    const uint32_t count = c ? 3 : 2;
    if (deferring(cpu))
        fallback();
    drain();
    if (s_capture)
    {
        float m[16];
        multiply(a, b, m);
        if (c)
            multiply(m, c, m);
        return effectSetMatrix(app, cpu, guest, handle, m);
    }
    const float* matrices[3] = { a, b, c };
    return effect::setProduct(guest, handle, matrices, count, readFpscr());
}

HRESULT effectSetTexture(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, reg32 texture)
{
    if (deferringEffect(cpu, handle) && s_defer.boundOnly == 0)
    {
        uint32_t* w = put(MethodEffectSetTexture, 3);
        w[0] = guest;
        w[1] = handle;
        w[2] = texture;
        return queued();
    }
    return effectSetTextureDirect(app, cpu, guest, handle, texture);
}

HRESULT effectSetTextureDirect(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, reg32 texture)
{
    if (deferring(cpu))
        fallback();
    drain();
    if (active())
    {
        Record r(MethodEffectSetTexture);
        r.word(guest);
        r.word(handle);
        r.word(texture);
        if (r.replaying())
        {
            const Capture::Call* call = r.replay();
            return call ? call->result : D3D_OK;
        }
        const HRESULT h = effect::setTexture(app, cpu, guest, handle, texture);
        r.done(h);
        return h;
    }
    return effect::setTexture(app, cpu, guest, handle, texture);
}

HRESULT effectBegin(WinApplication* app, x86::CPU& cpu, reg32 guest, UINT* passes, DWORD flags)
{
    if (deferring(cpu))
    {
        const UINT count = passes ? effect::passCount(guest) : 0;
        uint32_t* w = put(MethodEffectBegin, 4);
        w[0] = guest;
        w[1] = passes ? 1 : 0;
        w[2] = flags;
        w[3] = count;
        if (passes)
            *passes = count;
        return queued();
    }
    drain();
    if (active())
    {
        Record r(MethodEffectBegin);
        r.word(guest);
        r.word(passes ? 1 : 0);
        r.word(flags);
        if (r.replaying())
        {
            const Capture::Call* call = r.replay();
            if (call && passes)
                *passes = call->output;
            return call ? call->result : D3D_OK;
        }
        const HRESULT h = effect::begin(app, cpu, guest, passes, flags);
        r.done(h).output = passes ? *passes : 0;
        return h;
    }
    return effect::begin(app, cpu, guest, passes, flags);
}

HRESULT effectPass(WinApplication* app, x86::CPU& cpu, reg32 guest, UINT pass)
{
    if (deferring(cpu))
    {
        uint32_t* w = put(MethodEffectPass, 2);
        w[0] = guest;
        w[1] = pass;
        return queued();
    }
    return effectPassDirect(app, cpu, guest, pass);
}

HRESULT effectPassDirect(WinApplication* app, x86::CPU& cpu, reg32 guest, UINT pass)
{
    drain();
    if (active())
    {
        Record r(MethodEffectPass);
        r.word(guest);
        r.word(pass);
        if (r.replaying())
        {
            const Capture::Call* call = r.replay();
            return call ? call->result : D3D_OK;
        }
        const HRESULT h = effect::pass(app, cpu, guest, pass);
        r.done(h);
        return h;
    }
    return effect::pass(app, cpu, guest, pass);
}

HRESULT effectEnd(WinApplication* app, x86::CPU& cpu, reg32 guest)
{
    if (deferring(cpu))
    {
        put(MethodEffectEnd, 1)[0] = guest;
        return queued();
    }
    drain();
    if (active())
    {
        Record r(MethodEffectEnd);
        r.word(guest);
        if (r.replaying())
        {
            const Capture::Call* call = r.replay();
            return call ? call->result : D3D_OK;
        }
        const HRESULT h = effect::end(app, cpu, guest);
        r.done(h);
        return h;
    }
    return effect::end(app, cpu, guest);
}

// ---- D3DX math ----

void matrixMultiply(const float* a, const float* b, float* out)
{
    multiply(a, b, out);
}

void vec4Transform(const float* v, const float* m, float* out)
{
    float r[4];
    for (int j = 0; j < 4; ++j)
        r[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
    memcpy(out, r, sizeof(r));
}

}

// ---- Guest entries (stdcall: this, then the arguments) ----

namespace guest
{

using bridge::reg32;

namespace
{

inline reg32 arg(const x86::CPU& cpu, uint32_t n)
{
    return rc::ld32(cpu.esp + 4 + 4 * n);
}

template< typename T >
inline const T* pointer(reg32 address)
{
    return address ? reinterpret_cast<const T*>(g_guestBase + address) : nullptr;
}

inline float asFloat(reg32 value)
{
    float f;
    memcpy(&f, &value, 4);
    return f;
}

}

void SetTransform(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::setTransform(app, cpu, arg(cpu, 0), arg(cpu, 1), pointer<float>(arg(cpu, 2)));
    cpu.esp += 16;
}

void SetMaterial(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::setMaterial(app, cpu, arg(cpu, 0), pointer<DWORD>(arg(cpu, 1)));
    cpu.esp += 12;
}

void SetLight(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::setLight(app, cpu, arg(cpu, 0), arg(cpu, 1), pointer<DWORD>(arg(cpu, 2)));
    cpu.esp += 16;
}

void LightEnable(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::lightEnable(app, cpu, arg(cpu, 0), arg(cpu, 1), BOOL(arg(cpu, 2)));
    cpu.esp += 16;
}

void SetRenderState(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::setRenderState(app, cpu, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2));
    cpu.esp += 16;
}

void SetTextureStageState(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::setTextureStageState(app, cpu, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3));
    cpu.esp += 20;
}

void SetSamplerState(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::setSamplerState(app, cpu, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3));
    cpu.esp += 20;
}

void Present(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::present(app, cpu, arg(cpu, 0), pointer<uint8_t>(arg(cpu, 1)), pointer<uint8_t>(arg(cpu, 2)),
                              arg(cpu, 3), arg(cpu, 4));
    cpu.esp += 24;
}

void SetViewport(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::setViewport(app, cpu, arg(cpu, 0), pointer<DWORD>(arg(cpu, 1)));
    cpu.esp += 12;
}

void SetFVF(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::setFVF(app, cpu, arg(cpu, 0), arg(cpu, 1));
    cpu.esp += 12;
}

void Clear(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::clear(app, cpu, arg(cpu, 0), arg(cpu, 1), pointer<DWORD>(arg(cpu, 2)), arg(cpu, 3),
                            arg(cpu, 4), asFloat(arg(cpu, 5)), arg(cpu, 6));
    cpu.esp += 32;
}

void BeginScene(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::beginScene(app, cpu, arg(cpu, 0));
    cpu.esp += 8;
}

void EndScene(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::endScene(app, cpu, arg(cpu, 0));
    cpu.esp += 8;
}

void SetRenderTarget(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::setRenderTarget(app, cpu, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2));
    cpu.esp += 16;
}

void SetDepthStencilSurface(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::setDepthStencilSurface(app, cpu, arg(cpu, 0), arg(cpu, 1));
    cpu.esp += 12;
}

void SetTexture(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::setTexture(app, cpu, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2));
    cpu.esp += 16;
}

void SetVertexDeclaration(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::setVertexDeclaration(app, cpu, arg(cpu, 0), arg(cpu, 1));
    cpu.esp += 12;
}

void SetStreamSource(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::setStreamSource(app, cpu, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3), arg(cpu, 4));
    cpu.esp += 24;
}

void SetIndices(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::setIndices(app, cpu, arg(cpu, 0), arg(cpu, 1));
    cpu.esp += 12;
}

void DrawIndexedPrimitive(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::drawIndexedPrimitive(app, cpu, arg(cpu, 0), arg(cpu, 1), LONG(arg(cpu, 2)), arg(cpu, 3),
                                           arg(cpu, 4), arg(cpu, 5), arg(cpu, 6));
    cpu.esp += 32;
}

void DrawPrimitiveUP(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::drawPrimitiveUP(app, cpu, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), pointer<uint8_t>(arg(cpu, 3)),
                                      arg(cpu, 4));
    cpu.esp += 24;
}

void EffectSetInt(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::effectSetInt(app, cpu, arg(cpu, 0), arg(cpu, 1), x86::sreg32(arg(cpu, 2)));
    cpu.esp += 16;
}

void EffectSetFloat(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::effectSetFloat(app, cpu, arg(cpu, 0), arg(cpu, 1), asFloat(arg(cpu, 2)));
    cpu.esp += 16;
}

void EffectSetVector(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::effectSetVector(app, cpu, arg(cpu, 0), arg(cpu, 1), pointer<float>(arg(cpu, 2)));
    cpu.esp += 16;
}

void EffectSetVectorArray(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::effectSetVectorArray(app, cpu, arg(cpu, 0), arg(cpu, 1), pointer<float>(arg(cpu, 2)),
                                           arg(cpu, 3));
    cpu.esp += 20;
}

void EffectSetMatrix(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::effectSetMatrix(app, cpu, arg(cpu, 0), arg(cpu, 1), pointer<float>(arg(cpu, 2)));
    cpu.esp += 16;
}

void EffectSetMatrixArray(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::effectSetMatrixArray(app, cpu, arg(cpu, 0), arg(cpu, 1), pointer<float>(arg(cpu, 2)),
                                           arg(cpu, 3));
    cpu.esp += 20;
}

void EffectSetTexture(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::effectSetTexture(app, cpu, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2));
    cpu.esp += 16;
}

void EffectBegin(WinApplication* app, x86::CPU& cpu)
{
    const reg32 passes = arg(cpu, 1);
    cpu.eax = bridge::effectBegin(app, cpu, arg(cpu, 0),
                                  passes ? reinterpret_cast<UINT*>(g_guestBase + passes) : nullptr, arg(cpu, 2));
    cpu.esp += 16;
}

void EffectPass(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::effectPass(app, cpu, arg(cpu, 0), arg(cpu, 1));
    cpu.esp += 12;
}

void EffectEnd(WinApplication* app, x86::CPU& cpu)
{
    cpu.eax = bridge::effectEnd(app, cpu, arg(cpu, 0));
    cpu.esp += 8;
}

}

}}
