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

// Queue records: a header word (operation in bits 0-7, payload words above) and the payload. Operations are the
// capture methods and these.
enum : uint8_t
{
    OpSetTextureStageState = MethodCount,
    OpSetSamplerState,
    OpSetMatrixProduct,     // effect, handle, count, FPSCR, count matrices
    OpFpscr,                // the FPSCR the following calls run with
    OpWrap                  // the next record is at the start of the ring
};

const uint32_t QueueWords = 1u << 18;           // 1 MiB ring
const uint32_t QueueMask = QueueWords - 1;
const uint32_t MaxRecordWords = QueueWords / 8; // larger calls run at once
const uint32_t MaxArrayFloats = 1024;
// The queue thread makes its progress visible every this many records.
const uint32_t ReadPublishMask = 255;
// The queue thread spins this long for more calls before it sleeps; a drain spins this long before it sleeps.
const uint32_t IdleSpinUs = 30;
const uint32_t WaitSpinUs = 50;
const uint32_t SpinClockMask = 15;
const uint32_t ResultLogLimit = 10;
const uint32_t QueueStack = 64 * 1024;

const char* opName(uint32_t op)
{
    switch (op)
    {
    case OpSetTextureStageState: return "SetTextureStageState";
    case OpSetSamplerState:      return "SetSamplerState";
    case OpSetMatrixProduct:     return "Effect::SetMatrix (product)";
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
    const x86::CPU* owner = nullptr;    // the open window's CPU
    reg32 deviceGuest = 0;
    Device* device = nullptr;
    uint32_t head = 0;                  // words written
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

    // Statistics.
    DeferStats stats = {};
    std::atomic<uint32_t> runUs{0}, results{0}, passes{0};
    uint32_t logged = 0;
};

Deferral s_defer;

inline bool deferring(const x86::CPU& cpu)
{
    return s_defer.owner == &cpu;
}

inline bool deferringDevice(const x86::CPU& cpu, reg32 guest)
{
    return s_defer.owner == &cpu && guest == s_defer.deviceGuest;
}

inline bool deferringDraw(const x86::CPU& cpu, reg32 guest)
{
    return deferringDevice(cpu, guest) && s_defer.device->stream() != nullptr;
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

void noteResult(uint32_t op, HRESULT result)
{
    Deferral& d = s_defer;
    d.results.fetch_add(1, std::memory_order_relaxed);
    if (d.logged < ResultLogLimit)
    {
        ++d.logged;
        pf::logInfo("flush_async: queued %s returned 0x%08x, its caller got D3D_OK", opName(op), unsigned(result));
    }
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
    HRESULT h = D3D_OK;
    switch (op)
    {
    case MethodSetTransform:
        h = device(w[0])->SetTransform(app, cpu, w[1], w[2] ? reinterpret_cast<const D3DMATRIX*>(w + 3) : nullptr);
        break;
    case MethodSetMaterial:
        h = device(w[0])->SetMaterial(app, cpu, w[1] ? w + 2 : nullptr);
        break;
    case MethodSetLight:
        h = device(w[0])->SetLight(app, cpu, w[1], w[2] ? w + 3 : nullptr);
        break;
    case MethodLightEnable:
        h = device(w[0])->LightEnable(app, cpu, w[1], BOOL(w[2]));
        break;
    case MethodSetRenderState:
        h = device(w[0])->SetRenderState(app, cpu, w[1], w[2]);
        break;
    case OpSetTextureStageState:
        h = device(w[0])->SetTextureStageState(app, cpu, w[1], w[2], w[3]);
        break;
    case OpSetSamplerState:
        h = device(w[0])->SetSamplerState(app, cpu, w[1], w[2], w[3]);
        break;
    case MethodSetTexture:
        h = device(w[0])->SetTexture(app, cpu, w[1], lookup(w[2]));
        break;
    case MethodSetVertexDeclaration:
        h = device(w[0])->SetVertexDeclaration(app, cpu, static_cast<VertexDeclaration*>(lookup(w[1])));
        break;
    case MethodSetStreamSource:
        h = device(w[0])->SetStreamSource(app, cpu, w[1], static_cast<Buffer*>(lookup(w[2])), w[3], w[4]);
        break;
    case MethodSetIndices:
        h = device(w[0])->SetIndices(app, cpu, static_cast<Buffer*>(lookup(w[1])));
        break;
    case MethodDrawIndexedPrimitive:
        h = device(w[0])->DrawIndexedPrimitive(app, cpu, w[1], LONG(w[2]), w[3], w[4], w[5], w[6]);
        break;
    case MethodDrawPrimitiveUP:
        h = device(w[0])->DrawPrimitiveUP(app, cpu, w[1], w[2], w[4] ? w + 5 : nullptr, w[3]);
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
    case OpFpscr:
        if (readFpscr() != w[0])
            writeFpscr(w[0]);
        break;
    default:
        pf::fatal("flush_async: queue record 0x%08x", unsigned(p[0]));
    }
    if (__builtin_expect(h != D3D_OK, 0))
        noteResult(op, h);
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
    d.runUs.fetch_add(clockUs() - start, std::memory_order_relaxed);
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
        d.runUs.fetch_add(clockUs() - start, std::memory_order_relaxed);
        d.read.store(r, std::memory_order_seq_cst);
        notifyDrain();
    }
    return 0;
}

// Makes the written records visible to the queue thread and wakes it.
void publish()
{
    Deferral& d = s_defer;
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
    d.stats.waitUs += clockUs() - start;
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

// Appends a record of words payload words and returns its payload.
uint32_t* put(uint32_t op, uint32_t words)
{
    Deferral& d = s_defer;
    const uint32_t need = words + 1;
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
    p[0] = op | words << 8;
    d.head += need;
    ++d.stats.calls;
    d.stats.words += need;
    g_deferPending = true;
    return p + 1;
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

void configureDeferral(DeferMode mode, int core, int priority)
{
    Deferral& d = s_defer;
    if (d.ring)
        return;
    d.mode = mode;
    d.core = core;
    d.priority = priority;
    if (mode != DeferOff)
        d.ring = new uint32_t[QueueWords];
}

bool deferBegin(WinApplication* app, x86::CPU& cpu, reg32 guest)
{
    Deferral& d = s_defer;
    if (d.mode == DeferOff || d.owner || s_capture)
        return false;
    Object* object = lookup(guest);
    if (!object || object->kind() != KindDevice || !static_cast<Device*>(object)->stream())
        return false;
    if (!d.app)
    {
        d.app = app;
        d.runCpu = cpu;
        if (d.mode == DeferThread)
            startThread();
    }
    const uint32_t fpscr = readFpscr();
    if (!d.fpscrSent || fpscr != d.fpscr)
    {
        put(OpFpscr, 1)[0] = fpscr;
        d.fpscr = fpscr;
        d.fpscrSent = true;
    }
    d.owner = &cpu;
    d.deviceGuest = guest;
    d.device = static_cast<Device*>(object);
    ++d.stats.windows;
    return true;
}

void deferEnd()
{
    Deferral& d = s_defer;
    publish();
    d.owner = nullptr;
    d.deviceGuest = 0;
    d.device = nullptr;
}

void drainSlow()
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
            if (d.owner)
                ++d.stats.windowDrains;
            waitRead(d.head);
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
    s.runUs = d.runUs.exchange(0, std::memory_order_relaxed);
    s.results = d.results.exchange(0, std::memory_order_relaxed);
    s.passes = d.passes.exchange(0, std::memory_order_relaxed);
    d.stats = DeferStats{};
    return s;
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
        return D3D_OK;
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
        return D3D_OK;
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
        return D3D_OK;
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
        return D3D_OK;
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
        return D3D_OK;
    }
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
        return D3D_OK;
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
        return D3D_OK;
    }
    drain();
    return device(guest)->SetSamplerState(app, cpu, sampler, type, value);
}

HRESULT setTexture(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD stage, reg32 texture)
{
    if (deferringBinding(cpu, guest))
    {
        uint32_t* w = put(MethodSetTexture, 3);
        w[0] = guest;
        w[1] = stage;
        w[2] = texture;
        return D3D_OK;
    }
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
        return D3D_OK;
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
    return device(guest)->SetVertexDeclaration(app, cpu, static_cast<VertexDeclaration*>(lookup(declaration)));
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
        return D3D_OK;
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
    return device(guest)->SetStreamSource(app, cpu, stream, static_cast<Buffer*>(lookup(buffer)), offset, stride);
}

HRESULT setIndices(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 buffer)
{
    if (deferringBinding(cpu, guest))
    {
        uint32_t* w = put(MethodSetIndices, 2);
        w[0] = guest;
        w[1] = buffer;
        return D3D_OK;
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
    return device(guest)->SetIndices(app, cpu, static_cast<Buffer*>(lookup(buffer)));
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
        publish();
        return D3D_OK;
    }
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
    return device(guest)->DrawIndexedPrimitive(app, cpu, type, baseVertex, minIndex, vertices, startIndex, count);
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
        publish();
        return D3D_OK;
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
    return device(guest)->DrawPrimitiveUP(app, cpu, type, count, data, stride);
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
        return D3D_OK;
    }
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
        return D3D_OK;
    }
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
        return D3D_OK;
    }
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
        return D3D_OK;
    }
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
        return D3D_OK;
    }
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
        return D3D_OK;
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
        return D3D_OK;
    }
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
        return D3D_OK;
    }
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
        return D3D_OK;
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
        return D3D_OK;
    }
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
        return D3D_OK;
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
