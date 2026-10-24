// Direct calls into the Direct3D 9 device and the D3DX effects, their capture and replay (d3d9_native.h), and the
// guest entries of the bridged device methods.

#include <winapi/d3d9_native.h>
#include <winapi/d3d9_internal.h>
#include <platform/log.h>

#include <cstdio>
#include <cstring>
#include <recomp.h>

namespace win32 { namespace d3d9 { namespace bridge
{

namespace
{

Capture* s_capture = nullptr;
uint32_t s_paused = 0;
WinApplication* s_app = nullptr;
MemMap* s_calloutMemory = nullptr;
reg32 s_calloutTargets[CalloutCount] = {};

const char* const s_methodNames[MethodCount] = {
    "SetTransform", "SetMaterial", "SetLight", "LightEnable", "SetRenderState", "SetTexture",
    "SetVertexDeclaration", "Effect::SetInt", "Effect::SetFloat", "Effect::SetVector", "Effect::SetVectorArray",
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

void callout0(WinApplication* app, x86::CPU& cpu)
{
    callout(app, cpu, 0);
}

void callout1(WinApplication* app, x86::CPU& cpu)
{
    callout(app, cpu, 1);
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
    const MethodPtr methods[CalloutCount] = { &callout0, &callout1 };
    for (uint32_t i = 0; i < CalloutCount; ++i)
    {
        app->getMemory<x86::reg32>(calloutAddress(i)) = calloutAddress(i);
        char name[32];
        snprintf(name, sizeof(name), "d3d9 callout %u", unsigned(i));
        app->registerMethod(calloutAddress(i), { name, methods[i] });
    }
}

void callGuest(WinApplication* app, x86::CPU& cpu, reg32 address)
{
    if (active())
        runGuest(app, cpu, address);
    else
        app->dynamic_call(address, cpu);
}

// ---- Device ----

HRESULT setTransform(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD state, const float* matrix)
{
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

HRESULT setTexture(WinApplication* app, x86::CPU& cpu, reg32 guest, DWORD stage, reg32 texture)
{
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

// ---- Effect ----

HRESULT effectSetInt(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, x86::sreg32 value)
{
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

HRESULT effectSetTexture(WinApplication* app, x86::CPU& cpu, reg32 guest, reg32 handle, reg32 texture)
{
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
