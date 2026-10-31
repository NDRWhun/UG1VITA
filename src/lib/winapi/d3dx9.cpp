// D3DX 9 (9.0b interface order) natively: effects and effect pools for the game's seven fx files, and the four
// math functions the game calls. See include/winapi/d3dx9.h.

#include <winapi/d3dx9.h>
#include <winapi/d3d9_internal.h>
#include <winapi/d3d9_native.h>
#include <platform/log.h>
#include <platform/sha256.h>
#include <algorithm>
#include <cstring>
#include <string>

namespace win32 { namespace d3d9
{

namespace
{

// The 47 parameter names of the game's lookup table (0x00701860), in its order.
enum Param
{
    PWorldViewProj, PViewProj, PWorldView, PWorld, PView, PProj, PBlendMatrices, PHeadlightWVP, PLocalEyePos,
    PDiffusemap, PHeadlightTex, PHarmonicCoeff, PAmbientCoeff, PDiffuseMin, PDiffuseRange, PSpecularMin,
    PSpecularRange, PSpecularPower, PEnvmapMin, PEnvmapRange, PEnvmapPower, PHeadlightColor, PHeadlightPos,
    PHeadlightFarClip, PHeadlightClipTex, PEnvMapTex, PReflectedTex, PReflectedProj, PReflectedTexMin,
    PFilterWeights, PFilterOffsets, PFilterTexture0, PFilterTexture1, PFilterTexture2, PFilterTexture3, PFogOffset,
    PFogScale, PFogFalloffY, PFogColor, PHeadlightClipMat, PBaseAlphaTestEnable, PBaseAlphaRef,
    PBaseAlphaBlendEnable, PBaseSrcBlend, PBaseDestBlend, PTextureOffset, PTextureOffsetMatrix, ParamCount
};

enum Type
{
    TypeFloat,      // floats (vectors, matrices, arrays)
    TypeInt,
    TypeTexture
};

struct ParamInfo
{
    const char* name;
    Type        type;
    uint16_t    floats;     // storage size for TypeFloat/TypeInt
    bool        shared;     // in the effect pool
};

const ParamInfo s_params[ParamCount] = {
    { "WorldViewProj", TypeFloat, 16, false }, { "ViewProj", TypeFloat, 16, false },
    { "WorldView", TypeFloat, 16, false }, { "World", TypeFloat, 16, false }, { "View", TypeFloat, 16, false },
    { "Proj", TypeFloat, 16, false }, { "BlendMatrices", TypeFloat, 256, false },
    { "HeadlightWVP", TypeFloat, 16, false }, { "LocalEyePos", TypeFloat, 4, false },
    { "diffusemap", TypeTexture, 0, true }, { "HeadlightTex", TypeTexture, 0, false },
    { "HarmonicCoeff", TypeFloat, 40, false }, { "AmbientCoeff", TypeInt, 1, false },
    { "DiffuseMin", TypeFloat, 4, false }, { "DiffuseRange", TypeFloat, 4, false },
    { "SpecularMin", TypeFloat, 4, false }, { "SpecularRange", TypeFloat, 4, false },
    { "SpecularPower", TypeFloat, 1, false }, { "EnvmapMin", TypeFloat, 4, false },
    { "EnvmapRange", TypeFloat, 4, false }, { "EnvmapPower", TypeFloat, 1, false },
    { "HeadlightColor", TypeFloat, 4, false }, { "HeadlightPos", TypeFloat, 4, false },
    { "HeadlightFarClip", TypeFloat, 1, false }, { "HeadlightClipTex", TypeTexture, 0, false },
    { "EnvMapTex", TypeTexture, 0, false }, { "ReflectedTex", TypeTexture, 0, false },
    { "ReflectedProj", TypeFloat, 16, false }, { "ReflectedTexMin", TypeFloat, 1, false },
    { "FilterWeights", TypeFloat, 16, false }, { "FilterOffsets", TypeFloat, 16, false },
    { "FilterTexture0", TypeTexture, 0, false }, { "FilterTexture1", TypeTexture, 0, false },
    { "FilterTexture2", TypeTexture, 0, false }, { "FilterTexture3", TypeTexture, 0, false },
    { "FogOffset", TypeFloat, 1, false }, { "FogScale", TypeFloat, 1, false },
    { "FogFalloffY", TypeFloat, 1, false }, { "FogColor", TypeFloat, 4, false },
    { "HeadlightClipMat", TypeFloat, 16, false }, { "BaseAlphaTestEnable", TypeInt, 1, true },
    { "BaseAlphaRef", TypeInt, 1, true }, { "BaseAlphaBlendEnable", TypeInt, 1, true },
    { "BaseSrcBlend", TypeInt, 1, true }, { "BaseDestBlend", TypeInt, 1, true },
    { "TextureOffset", TypeFloat, 4, true }, { "TextureOffsetMatrix", TypeFloat, 16, true },
};

enum EffectKind
{
    EffectWorld, EffectWorldHeadlight, EffectWorldReflect, EffectWorldHeadlightReflect, EffectWorldBone, EffectCar,
    EffectFilter, EffectCount
};

#define BIT(p) (uint64_t(1) << (p))
const uint64_t s_base = BIT(PBaseAlphaTestEnable) | BIT(PBaseAlphaRef) | BIT(PBaseAlphaBlendEnable)
                      | BIT(PBaseSrcBlend) | BIT(PBaseDestBlend);
const uint64_t s_headlight = BIT(PHeadlightWVP) | BIT(PHeadlightClipMat) | BIT(PHeadlightTex)
                           | BIT(PHeadlightColor) | BIT(PHeadlightClipTex);

struct EffectInfo
{
    const char* name;
    const char* sha256;         // of the decoded fx text
    const char* marker;         // text only this source has (fallback when the hash differs)
    uint64_t    params;
    uint8_t     techniques;
    uint8_t     passes[4];      // per technique
};

// Markers are checked in this order.
const EffectInfo s_effects[EffectCount] = {
    { "world", "83ef44ae922838083b3af2c5bd970da9716924511b2991b22ca41e279bb65c3f", "World Effects",
      BIT(PWorldViewProj) | BIT(PTextureOffset) | BIT(PTextureOffsetMatrix) | BIT(PDiffusemap) | s_base,
      2, { 1, 1 } },
    { "worldheadlight", "97a13569372d1c614f02de5fbdc948f1ed93ef06825435e7e02ba8f757168f75", "HeadlightTex",
      BIT(PWorldViewProj) | BIT(PTextureOffset) | BIT(PTextureOffsetMatrix) | BIT(PDiffusemap) | s_headlight
      | s_base, 4, { 1, 1, 2, 1 } },
    { "worldreflect", "7bc39fb8c0b8a3fb355e68714843c3c51fc6c777cebefbce8879042a6fa5a468", "ReflectedTex",
      BIT(PWorldViewProj) | BIT(PReflectedTex) | BIT(PReflectedProj) | BIT(PDiffusemap) | s_base, 4, { 1, 1, 2, 1 } },
    { "worldheadlightreflect", "28d3c0ad5a2301efdcceaacd154caf2df3511bed58ce8639390c31592ff7f99e", "ReflectedTexMin",
      BIT(PWorldViewProj) | s_headlight | BIT(PReflectedTex) | BIT(PReflectedProj) | BIT(PReflectedTexMin)
      | BIT(PDiffusemap) | s_base, 4, { 1, 1, 2, 1 } },
    { "worldbone", "55d91a4a65098a8c77f42b2f2ffd2311a000c6dd20e91eabb7044744c13c6426", "BlendMatrices",
      BIT(PWorldViewProj) | BIT(PLocalEyePos) | BIT(PHarmonicCoeff) | BIT(PBlendMatrices) | BIT(PDiffuseMin)
      | BIT(PDiffuseRange) | BIT(PDiffusemap) | s_base, 1, { 1 } },
    { "car", "8af5f01f48a048fed40574b1c958d1c25ec902d49c7ef2d9cac220e8dc578bc7", "EnvMapTex",
      BIT(PWorldViewProj) | BIT(PWorldView) | BIT(PLocalEyePos) | BIT(PHarmonicCoeff) | BIT(PAmbientCoeff)
      | BIT(PDiffuseMin) | BIT(PDiffuseRange) | BIT(PSpecularMin) | BIT(PSpecularRange) | BIT(PEnvmapMin)
      | BIT(PEnvmapRange) | BIT(PSpecularPower) | BIT(PEnvmapPower) | BIT(PEnvMapTex) | BIT(PDiffusemap) | s_base,
      3, { 1, 1, 1 } },
    { "filter", "6d23a19cb1396f45d2010a8c9aba04649beb3993b27352b587c34471023b3392", "FilterWeights",
      BIT(PFilterWeights) | BIT(PFilterOffsets) | BIT(PFilterTexture0) | BIT(PFilterTexture1) | BIT(PFilterTexture2)
      | BIT(PFilterTexture3) | s_base, 1, { 4 } },
};

const uint8_t s_markerOrder[EffectCount] = { EffectWorldBone, EffectFilter, EffectCar, EffectWorldHeadlightReflect,
                                             EffectWorldHeadlight, EffectWorldReflect, EffectWorld };

// Handles: parameters, techniques and passes in ranges below any guest address (strings are names).
enum : x86::reg32
{
    HandleParam = 0x1000,
    HandleTechnique = 0x2000,
    HandlePass = 0x3000
};

uint32_t s_offsets[ParamCount];
uint32_t s_totalFloats = 0;

void initOffsets()
{
    if (s_totalFloats)
        return;
    for (uint32_t p = 0; p < ParamCount; ++p)
    {
        s_offsets[p] = s_totalFloats;
        s_totalFloats += s_params[p].floats;
    }
}

struct Values;
// Value blocks by id (the render thread mirrors their floats), and the blocks to send whole.
std::vector<Values*>    s_blocks;
std::vector<uint16_t>   s_dirty;

// A matrix parameter recorded as the product of 2 or 3 matrices (CmdProduct); its floats are computed when the game
// thread reads them.
struct Pending
{
    int         param;
    uint32_t    count;
    uint32_t    fpscr;
    float       matrices[48];
};

// Parameter values: floats (ints stored as their bits) and textures.
struct Values
{
    enum { PendingSlots = 4 };

    std::vector<float>  floats;
    Object*             textures[ParamCount] = {};
    uint16_t            id;
    bool                dirty = false;
    uint32_t            pendingCount = 0;
    Pending             pending[PendingSlots];

    Pending* findPending(int p)
    {
        for (uint32_t i = 0; i < pendingCount; ++i)
            if (pending[i].param == p)
                return &pending[i];
        return nullptr;
    }
    // Computes the floats of a pending parameter.
    void settle(int p)
    {
        for (uint32_t i = 0; i < pendingCount; ++i)
        {
            if (pending[i].param != p)
                continue;
            matrixProduct(pending[i].matrices, pending[i].count, pending[i].fpscr, floats.data() + s_offsets[p]);
            pending[i] = pending[--pendingCount];
            return;
        }
    }
    void settleAll()
    {
        while (pendingCount)
            settle(pending[0].param);
    }

    Values()
    {
        initOffsets();
        floats.assign(s_totalFloats, 0.0f);
        id = uint16_t(std::find(s_blocks.begin(), s_blocks.end(), nullptr) - s_blocks.begin());
        if (id == s_blocks.size())
            s_blocks.push_back(this);
        else
            s_blocks[id] = this;
        markDirty();
    }
    ~Values()
    {
        for (Object* t : textures)
            if (t)
                t->unbind();
        s_blocks[id] = nullptr;
        if (dirty)
            s_dirty.erase(std::find(s_dirty.begin(), s_dirty.end(), id));
    }
    void markDirty()
    {
        if (!dirty)
            s_dirty.push_back(id);
        dirty = true;
    }
};

// Value blocks as a pass reads them (render thread, mirrored blocks).
struct View
{
    EffectKind      kind;
    uint32_t        pass;
    const float*    own;
    const float*    shared;     // null without a pool

    View(const EffectDraw& effect, const float* own, const float* shared)
        :   kind(effect.kind < EffectCount ? EffectKind(effect.kind) : EffectFilter), pass(effect.pass), own(own)
        ,   shared(shared) {}
    const float* floats(int p) const    { return (s_params[p].shared && shared ? shared : own) + s_offsets[p]; }
    int intValue(int p) const           { x86::sreg32 v; memcpy(&v, floats(p), 4); return v; }
};

// Texture parameters of each kind's samplers.
const int8_t s_samplerParams[EffectCount][render::MaxSamplers] = {
    { PDiffusemap, -1, -1, -1 },
    { PDiffusemap, PHeadlightTex, PHeadlightClipTex, -1 },
    { PDiffusemap, PReflectedTex, -1, -1 },
    { PDiffusemap, PHeadlightTex, PHeadlightClipTex, PReflectedTex },
    { PDiffusemap, -1, -1, -1 },
    { PDiffusemap, PEnvMapTex, -1, -1 },
    { PFilterTexture0, PFilterTexture1, PFilterTexture2, PFilterTexture3 },
};

// Sampler stage of each kind's samplers (ps_1_1: tN reads stage N; car samples the diffuse map with t1).
const int8_t s_samplerStages[EffectCount][render::MaxSamplers] = {
    { 0, -1, -1, -1 },
    { 0, 1, 2, -1 },
    { 0, 1, -1, -1 },
    { 0, 1, 2, 3 },
    { 0, -1, -1, -1 },
    { 1, 0, -1, -1 },
    { 0, 1, 2, 3 },
};

uint32_t samplerCount(EffectKind kind, uint32_t pass)
{
    switch (kind)
    {
    case EffectWorldHeadlight:          return 3;
    case EffectWorldHeadlightReflect:   return 4;
    case EffectWorldReflect:
    case EffectCar:                     return 2;
    case EffectFilter:                  return pass < 4 ? pass + 1 : 4;
    default:                            return 1;
    }
}

}

class EffectPool : public Object
{
public:
    EffectPool() : Object(KindEffectPool) {}
    Values values;
};

class Effect : public Object, public EffectPass
{
public:
    Effect(Device* device, EffectPool* pool, EffectKind kind)
        :   Object(KindEffect), m_device(device), m_pool(pool), m_kind(kind)
    {
        device->addRef();
        if (pool)
            pool->addRef();
    }
    ~Effect() override
    {
        if (m_device->effectPass() == this)
            m_device->setEffectPass(nullptr);
        if (m_pool)
            m_pool->release();
        m_device->release();
    }

    // EffectPass
    void record(EffectDraw& effect, const render::TextureStorage* textures[render::MaxSamplers]) const override;

    HRESULT GetDesc(WinApplication* app, x86::CPU& cpu, DWORD* desc);
    x86::reg32 GetParameterByName(WinApplication* app, x86::CPU& cpu, x86::reg32 parent, const char* name);
    x86::reg32 GetParameterBySemantic(WinApplication* app, x86::CPU& cpu, x86::reg32 parent, const char* semantic);
    x86::reg32 GetTechnique(WinApplication* app, x86::CPU& cpu, UINT index);
    x86::reg32 GetTechniqueByName(WinApplication* app, x86::CPU& cpu, const char* name);
    x86::reg32 GetPass(WinApplication* app, x86::CPU& cpu, x86::reg32 technique, UINT index);
    HRESULT SetValue(WinApplication* app, x86::CPU& cpu, x86::reg32 param, const void* data, UINT bytes);
    HRESULT GetValue(WinApplication* app, x86::CPU& cpu, x86::reg32 param, void* data, UINT bytes);
    HRESULT SetBool(WinApplication* app, x86::CPU& cpu, x86::reg32 param, BOOL value);
    HRESULT SetInt(WinApplication* app, x86::CPU& cpu, x86::reg32 param, x86::sreg32 value);
    HRESULT GetInt(WinApplication* app, x86::CPU& cpu, x86::reg32 param, x86::sreg32* value);
    HRESULT SetFloat(WinApplication* app, x86::CPU& cpu, x86::reg32 param, float value);
    HRESULT GetFloat(WinApplication* app, x86::CPU& cpu, x86::reg32 param, float* value);
    HRESULT SetFloatArray(WinApplication* app, x86::CPU& cpu, x86::reg32 param, const float* values, UINT count);
    HRESULT SetVector(WinApplication* app, x86::CPU& cpu, x86::reg32 param, const float* vector);
    HRESULT GetVector(WinApplication* app, x86::CPU& cpu, x86::reg32 param, float* vector);
    HRESULT SetVectorArray(WinApplication* app, x86::CPU& cpu, x86::reg32 param, const float* vectors, UINT count);
    HRESULT SetMatrix(WinApplication* app, x86::CPU& cpu, x86::reg32 param, const float* matrix);
    HRESULT GetMatrix(WinApplication* app, x86::CPU& cpu, x86::reg32 param, float* matrix);
    HRESULT SetMatrixArray(WinApplication* app, x86::CPU& cpu, x86::reg32 param, const float* matrices, UINT count);
    HRESULT SetMatrixTranspose(WinApplication* app, x86::CPU& cpu, x86::reg32 param, const float* matrix);
    HRESULT SetMatrixTransposeArray(WinApplication* app, x86::CPU& cpu, x86::reg32 param, const float* matrices,
                                    UINT count);
    HRESULT SetTexture(WinApplication* app, x86::CPU& cpu, x86::reg32 param, Object* texture);
    HRESULT GetTexture(WinApplication* app, x86::CPU& cpu, x86::reg32 param, x86::reg32* texture);
    HRESULT GetPool(WinApplication* app, x86::CPU& cpu, x86::reg32* pool);
    HRESULT SetTechnique(WinApplication* app, x86::CPU& cpu, x86::reg32 technique);
    x86::reg32 GetCurrentTechnique(WinApplication* app, x86::CPU& cpu);
    HRESULT ValidateTechnique(WinApplication* app, x86::CPU& cpu, x86::reg32 technique);
    HRESULT FindNextValidTechnique(WinApplication* app, x86::CPU& cpu, x86::reg32 technique, x86::reg32* next);
    BOOL IsParameterUsed(WinApplication* app, x86::CPU& cpu, x86::reg32 param, x86::reg32 technique);
    HRESULT Begin(WinApplication* app, x86::CPU& cpu, UINT* passes, DWORD flags);
    HRESULT Pass(WinApplication* app, x86::CPU& cpu, UINT pass);
    HRESULT End(WinApplication* app, x86::CPU& cpu);
    HRESULT GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device);
    HRESULT OnLostDevice(WinApplication* app, x86::CPU& cpu);
    HRESULT OnResetDevice(WinApplication* app, x86::CPU& cpu);

private:
    // The parameter of a handle or name in this effect, or -1.
    int param(x86::reg32 handle) const;
    Values& values(int p)                   { return s_params[p].shared && m_pool ? m_pool->values : m_values; }
    const Values& values(int p) const       { return s_params[p].shared && m_pool ? m_pool->values : m_values; }
    float* floats(int p)
    {
        Values& v = values(p);
        if (v.pendingCount)
            v.settle(p);
        return v.floats.data() + s_offsets[p];
    }
    const float* floats(int p) const        { return const_cast<Effect*>(this)->floats(p); }
    int intValue(int p) const               { x86::sreg32 v; memcpy(&v, floats(p), 4); return v; }
    HRESULT setFloats(x86::reg32 handle, const float* data, uint32_t count, Type type);

public:
    // SetMatrix of the product of count (2 or 3) matrices, recorded unmultiplied; the render thread multiplies with
    // fpscr.
    HRESULT setProduct(x86::reg32 handle, const float* const* matrices, uint32_t count, uint32_t fpscr);
    // The pass count Begin stores.
    UINT passCount() const                  { return info().passes[m_technique]; }
    // The type of each parameter handle in this effect (bridge::effect::paramTypes).
    const uint8_t* paramTypes() const
    {
        static_assert(uint32_t(HandleParam) == bridge::effect::ParamHandleBase
                      && uint32_t(ParamCount) <= bridge::effect::ParamHandleCount, "parameter handles");
        static uint8_t s_types[EffectCount][bridge::effect::ParamHandleCount];
        static bool s_built = false;
        if (!s_built)
        {
            for (int k = 0; k < EffectCount; ++k)
                for (uint32_t i = 0; i < bridge::effect::ParamHandleCount; ++i)
                {
                    const bool used = i < uint32_t(ParamCount) && (s_effects[k].params & BIT(i));
                    s_types[k][i] = !used ? uint8_t(bridge::effect::ParamNone)
                                  : s_params[i].type == TypeFloat ? uint8_t(bridge::effect::ParamFloat)
                                  : s_params[i].type == TypeInt ? uint8_t(bridge::effect::ParamInt)
                                                                : uint8_t(bridge::effect::ParamTexture);
                }
            s_built = true;
        }
        return s_types[m_kind];
    }
    HRESULT checkPass(UINT pass) const      { return pass >= info().passes[m_technique] ? D3DERR_INVALIDCALL : D3D_OK; }

private:
    // Writes n floats of a parameter; a change is sent to the render thread.
    void store(int p, const void* data, uint32_t n);
    const EffectInfo& info() const          { return s_effects[m_kind]; }

    Device*     m_device;
    EffectPool* m_pool;
    EffectKind  m_kind;
    Values      m_values;
    uint32_t    m_technique = 0;
    uint32_t    m_pass = 0;
};

}}

namespace win32
{

D3D9_UNPACKER(d3d9::Effect)
D3D9_UNPACKER(d3d9::EffectPool)

}

namespace win32 { namespace d3d9
{

namespace
{

render::Sampler samplerOf(uint8_t address, bool mip)
{
    return render::Sampler{ address, address, 3, uint8_t(mip ? render::MipLinear : render::MipNone) };
}

}

int Effect::param(x86::reg32 handle) const
{
    int p = -1;
    if (handle >= HandleParam && handle < HandleParam + ParamCount)
    {
        p = int(handle - HandleParam);
    }
    else if (handle >= 0x10000)
    {
        // A name instead of a handle.
        const char* name = reinterpret_cast<const char*>(g_guestBase + handle);
        for (int i = 0; i < ParamCount; ++i)
        {
            if (strcmp(s_params[i].name, name) == 0)
                p = i;
        }
    }
    return p >= 0 && (info().params & BIT(p)) ? p : -1;
}

HRESULT Effect::setFloats(x86::reg32 handle, const float* data, uint32_t count, Type type)
{
    const int p = param(handle);
    if (p < 0 || !data || s_params[p].type != type)
        return D3DERR_INVALIDCALL;
    const uint32_t n = count < s_params[p].floats ? count : s_params[p].floats;
    store(p, data, n);
    return D3D_OK;
}

void Effect::store(int p, const void* data, uint32_t n)
{
    if (sameWords(floats(p), data, n))
        return;
    memcpy(floats(p), data, n * 4);
    Values& v = values(p);
    Stream* stream = m_device->stream();
    if (!stream || v.dirty)
    {
        v.markDirty();
        return;
    }
    uint32_t* w = stream->append(2 + n);
    w[0] = CmdValues | uint32_t(v.id) << 8;
    w[1] = s_offsets[p] | n << 16;
    memcpy(w + 2, floats(p), n * 4);
}

HRESULT Effect::setProduct(x86::reg32 handle, const float* const* matrices, uint32_t count, uint32_t fpscr)
{
    float in[48];
    for (uint32_t i = 0; i < count; ++i)
        memcpy(in + 16 * i, matrices[i], 64);
    const int p = param(handle);
    Stream* stream = m_device->stream();
    if (p < 0 || s_params[p].type != TypeFloat || s_params[p].floats < 16 || !stream || values(p).dirty)
    {
        float product[16];
        matrixProduct(in, count, fpscr, product);
        return setFloats(handle, product, 16, TypeFloat);
    }
    Values& v = values(p);
    Pending* e = v.findPending(p);
    if (e && e->count == count && e->fpscr == fpscr && sameWords(e->matrices, in, 16 * count))
        return D3D_OK;
    if (!e)
    {
        if (v.pendingCount == Values::PendingSlots)
            v.settle(v.pending[0].param);
        e = &v.pending[v.pendingCount++];
        e->param = p;
    }
    e->count = count;
    e->fpscr = fpscr;
    memcpy(e->matrices, in, 64 * count);
    uint32_t* w = stream->append(3 + 16 * count);
    w[0] = CmdProduct | uint32_t(v.id) << 8;
    w[1] = s_offsets[p] | count << 16;
    w[2] = fpscr;
    memcpy(w + 3, in, 64 * count);
    return D3D_OK;
}

void Effect::record(EffectDraw& effect, const render::TextureStorage* textures[render::MaxSamplers]) const
{
    effect.own = m_values.id;
    effect.shared = m_pool ? m_pool->values.id : uint16_t(NoBlock);
    effect.kind = uint8_t(m_kind);
    effect.pass = uint8_t(m_pass);
    effect.pad[0] = effect.pad[1] = 0;
    const uint32_t count = samplerCount(m_kind, m_pass);
    for (uint32_t i = 0; i < render::MaxSamplers; ++i)
    {
        const int p = s_samplerParams[m_kind][i];
        const Object* t = i < count && p >= 0 ? values(p).textures[p] : nullptr;
        textures[i] = t && (t->kind() == KindTexture || t->kind() == KindCubeTexture)
                    ? static_cast<const Texture*>(t)->storage() : nullptr;
    }
}

void syncEffectValues(Stream& stream)
{
    if (s_dirty.empty())
        return;
    for (uint16_t id : s_dirty)
    {
        Values* v = s_blocks[id];
        v->settleAll();
        uint32_t* w = stream.append(2 + s_totalFloats);
        w[0] = CmdValues | uint32_t(id) << 8;
        w[1] = s_totalFloats << 16;
        memcpy(w + 2, v->floats.data(), s_totalFloats * 4);
        v->dirty = false;
    }
    s_dirty.clear();
}

void markEffectValuesDirty()
{
    for (Values* v : s_blocks)
        if (v)
            v->markDirty();
}

int effectSamplerStage(const EffectDraw& effect, uint32_t slot)
{
    const EffectKind kind = effect.kind < EffectCount ? EffectKind(effect.kind) : EffectFilter;
    return slot < samplerCount(kind, effect.pass) && slot < render::MaxSamplers ? s_samplerStages[kind][slot] : -1;
}

uint32_t effectBlockFloats()
{
    initOffsets();
    return s_totalFloats;
}

void effectBinding(const EffectDraw& effect, const float* own, const float* shared, bool projected,
                   EffectBinding& b)
{
    const View v(effect, own, shared);
    const bool wrapMip = true;
    b.cullNone = v.kind != EffectCar;
    b.depthOff = v.kind == EffectFilter;
    b.alphaTest = v.intValue(PBaseAlphaTestEnable) != 0;
    b.alphaRef = DWORD(v.intValue(PBaseAlphaRef));
    b.blend = v.intValue(PBaseAlphaBlendEnable) != 0;
    b.srcBlend = DWORD(v.intValue(PBaseSrcBlend));
    b.dstBlend = DWORD(v.intValue(PBaseDestBlend));
    b.samplers[0] = samplerOf(render::AddressWrap, wrapMip);
    b.samplerCount = uint8_t(samplerCount(v.kind, v.pass));
    switch (v.kind)
    {
    case EffectWorld:
        if (projected)
        {
            D3D_LOG_ONCE("d3dx9: world effect draw with projected texture coordinates (stage 0 PROJECTED)");
            b.vertexProgram = render::VpWorldProjected;
            b.fragmentProgram = render::FpWorldProjected;
            break;
        }
        b.vertexProgram = render::VpWorld;
        b.fragmentProgram = render::FpWorld;
        break;
    case EffectWorldBone:
        b.vertexProgram = render::VpWorldBone;
        b.fragmentProgram = render::FpWorldBone;
        break;
    case EffectWorldHeadlight:
    case EffectWorldHeadlightReflect:
        b.vertexProgram = v.kind == EffectWorldHeadlight ? render::VpWorldHeadlight : render::VpWorldHeadlightReflect;
        b.fragmentProgram = v.kind == EffectWorldHeadlight ? render::FpWorldHeadlight
                                                           : render::FpWorldHeadlightReflect;
        b.samplers[1] = samplerOf(render::AddressClamp, true);
        b.samplers[2] = samplerOf(render::AddressClamp, true);
        b.samplers[3] = samplerOf(render::AddressWrap, true);
        break;
    case EffectWorldReflect:
        b.vertexProgram = render::VpWorldReflect;
        b.fragmentProgram = render::FpWorldReflect;
        b.samplers[1] = samplerOf(render::AddressWrap, true);
        break;
    case EffectCar:
        b.vertexProgram = render::VpCar;
        b.fragmentProgram = render::FpCar;
        b.samplers[1] = samplerOf(render::AddressClamp, true);
        break;
    case EffectFilter:
    default:
        b.vertexProgram = render::VpFilter;
        b.fragmentProgram = uint8_t(render::FpFilter1 + (v.pass < 4 ? v.pass : 3));
        for (int i = 0; i < 4; ++i)
            b.samplers[i] = samplerOf(render::AddressClamp, false);
        break;
    }
    if (projected && v.kind != EffectWorld)
        D3D_LOG_ONCE("d3dx9: projected stage 0 texture coordinates of effect kind %u not emulated", unsigned(v.kind));
}

void effectVertexConstants(const EffectDraw& effect, const float* own, const float* shared, float* out)
{
    const View v(effect, own, shared);
    auto copy = [&v](float* to, int p) { memcpy(to, v.floats(p), s_params[p].floats * sizeof(float)); };
    switch (v.kind)
    {
    case EffectWorld:
    {
        render::WorldVertexConstants& c = *reinterpret_cast<render::WorldVertexConstants*>(out);
        copy(c.wvp, PWorldViewProj);
        copy(c.textureOffset, PTextureOffset);
        break;
    }
    case EffectWorldHeadlight:
    {
        render::WorldHeadlightVertexConstants& c = *reinterpret_cast<render::WorldHeadlightVertexConstants*>(out);
        copy(c.wvp, PWorldViewProj);
        copy(c.headlightWvp, PHeadlightWVP);
        copy(c.textureOffset, PTextureOffset);
        break;
    }
    case EffectWorldReflect:
        copy(reinterpret_cast<render::WorldReflectVertexConstants*>(out)->wvp, PWorldViewProj);
        break;
    case EffectWorldHeadlightReflect:
    {
        render::WorldHeadlightReflectVertexConstants& c =
            *reinterpret_cast<render::WorldHeadlightReflectVertexConstants*>(out);
        copy(c.wvp, PWorldViewProj);
        copy(c.headlightWvp, PHeadlightWVP);
        break;
    }
    case EffectWorldBone:
    {
        render::WorldBoneVertexConstants& c = *reinterpret_cast<render::WorldBoneVertexConstants*>(out);
        copy(c.wvp, PWorldViewProj);
        copy(c.localEyePos, PLocalEyePos);
        copy(c.harmonic, PHarmonicCoeff);
        copy(c.diffuseMin, PDiffuseMin);
        copy(c.diffuseRange, PDiffuseRange);
        copy(c.bones, PBlendMatrices);
        break;
    }
    case EffectCar:
    {
        render::CarVertexConstants& c = *reinterpret_cast<render::CarVertexConstants*>(out);
        copy(c.wvp, PWorldViewProj);
        copy(c.worldView, PWorldView);
        copy(c.localEyePos, PLocalEyePos);
        copy(c.harmonic, PHarmonicCoeff);
        copy(c.diffuseMin, PDiffuseMin);
        copy(c.diffuseRange, PDiffuseRange);
        copy(c.specularMin, PSpecularMin);
        copy(c.specularRange, PSpecularRange);
        c.powers[0] = v.floats(PSpecularPower)[0];
        c.powers[1] = v.floats(PEnvmapPower)[0];
        c.powers[2] = c.powers[3] = 0.0f;
        break;
    }
    default:
        break;
    }
}

bool effectCarDiffuseAlpha(const EffectDraw& effect, const float* own, const float* shared, float* lowest)
{
    const View v(effect, own, shared);
    if (v.kind != EffectCar)
        return false;
    const float min = v.floats(PDiffuseMin)[3], range = v.floats(PDiffuseRange)[3];
    // car_v: alpha = saturate(0.5 * (min + max(0.01, v.n) * range)).
    if (!(range >= 0.0f) || !(min == min))
        return false;
    const float a = 0.5f * (min + 0.01f * range);
    *lowest = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
    return true;
}

void effectFragmentConstants(const EffectDraw& effect, const float* own, const float* shared, float* out)
{
    const View v(effect, own, shared);
    // ps_1_1 constant registers hold [-1, 1].
    auto copy = [&v](float* to, int p, uint32_t n)
    {
        const float* from = v.floats(p);
        for (uint32_t i = 0; i < n; ++i)
            to[i] = from[i] < -1.0f ? -1.0f : from[i] > 1.0f ? 1.0f : from[i];
    };
    switch (v.kind)
    {
    case EffectWorldHeadlight:
    case EffectWorldHeadlightReflect:
        copy(reinterpret_cast<render::WorldHeadlightFragmentConstants*>(out)->headlightColor, PHeadlightColor, 4);
        break;
    case EffectCar:
        copy(reinterpret_cast<render::CarFragmentConstants*>(out)->envmapMin, PEnvmapMin, 4);
        copy(reinterpret_cast<render::CarFragmentConstants*>(out)->envmapRange, PEnvmapRange, 4);
        break;
    case EffectFilter:
        copy(reinterpret_cast<render::FilterFragmentConstants*>(out)->weights, PFilterWeights, 16);
        break;
    default:
        break;
    }
}

HRESULT Effect::GetDesc(WinApplication* app, x86::CPU& cpu, DWORD* desc)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!desc)
        return D3DERR_INVALIDCALL;
    uint32_t params = 0;
    for (int p = 0; p < ParamCount; ++p)
        params += (info().params & BIT(p)) ? 1 : 0;
    desc[0] = 0;                    // Creator
    desc[1] = params;
    desc[2] = info().techniques;
    desc[3] = 0;                    // Functions
    return D3D_OK;
}

x86::reg32 Effect::GetParameterByName(WinApplication* app, x86::CPU& cpu, x86::reg32 parent, const char* name)
{
    RC_USE(app);
    RC_USE(cpu);
    if (parent || !name)
        return 0;
    for (int p = 0; p < ParamCount; ++p)
    {
        if ((info().params & BIT(p)) && strcmp(s_params[p].name, name) == 0)
            return HandleParam + p;
    }
    return 0;
}

x86::reg32 Effect::GetParameterBySemantic(WinApplication* app, x86::CPU& cpu, x86::reg32 parent,
                                          const char* semantic)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(parent);
    D3D_LOG_ONCE("d3dx9: GetParameterBySemantic(%s) is not supported", semantic ? semantic : "");
    return 0;
}

x86::reg32 Effect::GetTechnique(WinApplication* app, x86::CPU& cpu, UINT index)
{
    RC_USE(app);
    RC_USE(cpu);
    return index < info().techniques ? HandleTechnique + index : 0;
}

x86::reg32 Effect::GetTechniqueByName(WinApplication* app, x86::CPU& cpu, const char* name)
{
    RC_USE(app);
    RC_USE(cpu);
    // Technique 0 carries the effect's name (world, car, filter).
    return name && strcmp(name, m_kind == EffectCar ? "car" : m_kind == EffectFilter ? "filter" : "world") == 0
         ? x86::reg32(HandleTechnique) : 0;
}

x86::reg32 Effect::GetPass(WinApplication* app, x86::CPU& cpu, x86::reg32 technique, UINT index)
{
    RC_USE(app);
    RC_USE(cpu);
    const uint32_t t = technique - HandleTechnique;
    if (t >= info().techniques || index >= info().passes[t])
        return 0;
    return HandlePass + t * 16 + index;
}

HRESULT Effect::SetValue(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const void* data, UINT bytes)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !data)
        return D3DERR_INVALIDCALL;
    if (s_params[p].type == TypeTexture)
        return D3DERR_INVALIDCALL;
    const uint32_t n = bytes / 4 < s_params[p].floats ? bytes / 4 : s_params[p].floats;
    store(p, data, n);
    return D3D_OK;
}

HRESULT Effect::GetValue(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, void* data, UINT bytes)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !data || s_params[p].type == TypeTexture)
        return D3DERR_INVALIDCALL;
    const uint32_t n = bytes / 4 < s_params[p].floats ? bytes / 4 : s_params[p].floats;
    memcpy(data, floats(p), n * 4);
    return D3D_OK;
}

HRESULT Effect::SetBool(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, BOOL value)
{
    return SetInt(app, cpu, handle, value ? 1 : 0);
}

HRESULT Effect::SetInt(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::sreg32 value)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0)
        return D3DERR_INVALIDCALL;
    if (s_params[p].type == TypeInt)
    {
        store(p, &value, 1);
    }
    else if (s_params[p].type == TypeFloat)
    {
        const float f = float(value);
        store(p, &f, 1);
    }
    else
    {
        return D3DERR_INVALIDCALL;
    }
    return D3D_OK;
}

HRESULT Effect::GetInt(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::sreg32* value)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !value || s_params[p].type == TypeTexture)
        return D3DERR_INVALIDCALL;
    *value = s_params[p].type == TypeInt ? intValue(p) : x86::sreg32(floats(p)[0]);
    return D3D_OK;
}

HRESULT Effect::SetFloat(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float value)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p >= 0 && s_params[p].type == TypeInt)
        return SetInt(app, cpu, handle, x86::sreg32(value));
    return setFloats(handle, &value, 1, TypeFloat);
}

HRESULT Effect::GetFloat(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float* value)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !value || s_params[p].type != TypeFloat)
        return D3DERR_INVALIDCALL;
    *value = floats(p)[0];
    return D3D_OK;
}

HRESULT Effect::SetFloatArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* values, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    return setFloats(handle, values, count, TypeFloat);
}

HRESULT Effect::SetVector(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* vector)
{
    RC_USE(app);
    RC_USE(cpu);
    return setFloats(handle, vector, 4, TypeFloat);
}

HRESULT Effect::GetVector(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float* vector)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !vector || s_params[p].type != TypeFloat)
        return D3DERR_INVALIDCALL;
    memcpy(vector, floats(p), (s_params[p].floats < 4 ? s_params[p].floats : 4) * 4);
    return D3D_OK;
}

HRESULT Effect::SetVectorArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* vectors,
                               UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    return setFloats(handle, vectors, count * 4, TypeFloat);
}

HRESULT Effect::SetMatrix(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* matrix)
{
    RC_USE(app);
    RC_USE(cpu);
    return setFloats(handle, matrix, 16, TypeFloat);
}

HRESULT Effect::GetMatrix(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, float* matrix)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !matrix || s_params[p].floats < 16)
        return D3DERR_INVALIDCALL;
    memcpy(matrix, floats(p), 64);
    return D3D_OK;
}

HRESULT Effect::SetMatrixArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* matrices,
                               UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    return setFloats(handle, matrices, count * 16, TypeFloat);
}

HRESULT Effect::SetMatrixTranspose(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, const float* matrix)
{
    return SetMatrixTransposeArray(app, cpu, handle, matrix, 1);
}

HRESULT Effect::SetMatrixTransposeArray(WinApplication* app, x86::CPU& cpu, x86::reg32 handle,
                                        const float* matrices, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!matrices || count > 16)
        return D3DERR_INVALIDCALL;
    float t[256];
    for (UINT n = 0; n < count; ++n)
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                t[n * 16 + i * 4 + j] = matrices[n * 16 + j * 4 + i];
    return setFloats(handle, t, count * 16, TypeFloat);
}

HRESULT Effect::SetTexture(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, Object* texture)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || s_params[p].type != TypeTexture)
        return D3DERR_INVALIDCALL;
    if (texture && texture->kind() != KindTexture && texture->kind() != KindCubeTexture)
        return D3DERR_INVALIDCALL;
    Object*& slot = values(p).textures[p];
    if (texture)
        texture->bind();
    if (slot)
        slot->unbind();
    slot = texture;
    // The active pass samples the new value from now on (it was set after any device SetTexture of the stage).
    if (const EffectPass* active = m_device->effectPass())
    {
        const Effect* e = static_cast<const Effect*>(active);
        if (&e->values(p) == &values(p))
        {
            const uint32_t count = samplerCount(e->m_kind, e->m_pass);
            for (uint32_t i = 0; i < count && i < render::MaxSamplers; ++i)
            {
                if (s_samplerParams[e->m_kind][i] == p)
                    m_device->effectTextureApplied(uint32_t(s_samplerStages[e->m_kind][i]));
            }
        }
    }
    return D3D_OK;
}

HRESULT Effect::GetTexture(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::reg32* texture)
{
    RC_USE(app);
    RC_USE(cpu);
    const int p = param(handle);
    if (p < 0 || !texture || s_params[p].type != TypeTexture)
        return D3DERR_INVALIDCALL;
    Object* t = values(p).textures[p];
    if (t)
        t->addRef();
    *texture = t ? t->guest() : 0;
    return D3D_OK;
}

HRESULT Effect::GetPool(WinApplication* app, x86::CPU& cpu, x86::reg32* pool)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!pool)
        return D3DERR_INVALIDCALL;
    if (m_pool)
        m_pool->addRef();
    *pool = m_pool ? m_pool->guest() : 0;
    return D3D_OK;
}

HRESULT Effect::SetTechnique(WinApplication* app, x86::CPU& cpu, x86::reg32 technique)
{
    RC_USE(app);
    RC_USE(cpu);
    const uint32_t t = technique - HandleTechnique;
    if (t >= info().techniques)
        return D3DERR_INVALIDCALL;
    if (t != 0)
        pf::logError("d3dx9: %s technique %u has no programs; technique 0 is drawn", info().name, t);
    m_technique = t;
    return D3D_OK;
}

x86::reg32 Effect::GetCurrentTechnique(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return HandleTechnique + m_technique;
}

HRESULT Effect::ValidateTechnique(WinApplication* app, x86::CPU& cpu, x86::reg32 technique)
{
    RC_USE(app);
    RC_USE(cpu);
    // Only technique 0 (vs_1_1/ps_1_1) has the port's programs; the fixed-function ones fail.
    return technique == HandleTechnique ? D3D_OK : E_FAIL;
}

HRESULT Effect::FindNextValidTechnique(WinApplication* app, x86::CPU& cpu, x86::reg32 technique, x86::reg32* next)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!next)
        return D3DERR_INVALIDCALL;
    *next = technique == 0 ? x86::reg32(HandleTechnique) : 0;
    return D3D_OK;
}

BOOL Effect::IsParameterUsed(WinApplication* app, x86::CPU& cpu, x86::reg32 handle, x86::reg32 technique)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(technique);
    return param(handle) >= 0;
}

HRESULT Effect::Begin(WinApplication* app, x86::CPU& cpu, UINT* passes, DWORD flags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(flags);
    if (passes)
        *passes = info().passes[m_technique];
    m_pass = 0;
    return D3D_OK;
}

HRESULT Effect::Pass(WinApplication* app, x86::CPU& cpu, UINT pass)
{
    RC_USE(app);
    RC_USE(cpu);
    if (pass >= info().passes[m_technique])
        return D3DERR_INVALIDCALL;
    // Parameters keep applying while the pass is active (9.0b): draws read them as they are then.
    m_pass = pass;
    m_device->setEffectPass(this);
    m_device->effectPassApplied();
    return D3D_OK;
}

HRESULT Effect::End(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    if (m_device->effectPass() == this)
        m_device->setEffectPass(nullptr);
    return D3D_OK;
}

HRESULT Effect::GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!device)
        return D3DERR_INVALIDCALL;
    m_device->addRef();
    *device = m_device->guest();
    return D3D_OK;
}

HRESULT Effect::OnLostDevice(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return D3D_OK;
}

HRESULT Effect::OnResetDevice(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return D3D_OK;
}

// ---- Vtables (ID3DXEffect of D3DX 9.0b: Begin at slot 64, Pass at 65) ----------------------------------------------

namespace
{

const MethodEntry s_effect[] = {
    D3D_NAMED("QueryInterface", Object, QueryInterface), D3D_NAMED("AddRef", Object, AddRef),
    D3D_NAMED("Release", Object, Release),
    D3D_METHOD(Effect, GetDesc), D3D_TRAP(GetParameterDesc), D3D_TRAP(GetTechniqueDesc), D3D_TRAP(GetPassDesc),
    D3D_TRAP(GetFunctionDesc), D3D_TRAP(GetParameter), D3D_METHOD(Effect, GetParameterByName),
    D3D_METHOD(Effect, GetParameterBySemantic), D3D_TRAP(GetParameterElement), D3D_METHOD(Effect, GetTechnique),
    D3D_METHOD(Effect, GetTechniqueByName), D3D_METHOD(Effect, GetPass), D3D_TRAP(GetPassByName),
    D3D_TRAP(GetFunction), D3D_TRAP(GetFunctionByName), D3D_TRAP(GetAnnotation), D3D_TRAP(GetAnnotationByName),
    D3D_METHOD(Effect, SetValue), D3D_METHOD(Effect, GetValue), D3D_METHOD(Effect, SetBool), D3D_TRAP(GetBool),
    D3D_TRAP(SetBoolArray), D3D_TRAP(GetBoolArray), D3D_GUEST_NAMED("SetInt", EffectSetInt), D3D_METHOD(Effect, GetInt),
    D3D_TRAP(SetIntArray), D3D_TRAP(GetIntArray), D3D_GUEST_NAMED("SetFloat", EffectSetFloat), D3D_METHOD(Effect, GetFloat),
    D3D_METHOD(Effect, SetFloatArray), D3D_TRAP(GetFloatArray), D3D_GUEST_NAMED("SetVector", EffectSetVector),
    D3D_METHOD(Effect, GetVector), D3D_GUEST_NAMED("SetVectorArray", EffectSetVectorArray), D3D_TRAP(GetVectorArray),
    D3D_GUEST_NAMED("SetMatrix", EffectSetMatrix), D3D_METHOD(Effect, GetMatrix), D3D_GUEST_NAMED("SetMatrixArray", EffectSetMatrixArray),
    D3D_TRAP(GetMatrixArray), D3D_TRAP(SetMatrixPointerArray), D3D_TRAP(GetMatrixPointerArray),
    D3D_METHOD(Effect, SetMatrixTranspose), D3D_TRAP(GetMatrixTranspose), D3D_METHOD(Effect, SetMatrixTransposeArray),
    D3D_TRAP(GetMatrixTransposeArray), D3D_TRAP(SetMatrixTransposePointerArray),
    D3D_TRAP(GetMatrixTransposePointerArray), D3D_TRAP(SetString), D3D_TRAP(GetString),
    D3D_GUEST_NAMED("SetTexture", EffectSetTexture), D3D_METHOD(Effect, GetTexture), D3D_TRAP(SetPixelShader),
    D3D_TRAP(GetPixelShader), D3D_TRAP(SetVertexShader), D3D_TRAP(GetVertexShader),
    D3D_METHOD(Effect, GetPool), D3D_METHOD(Effect, SetTechnique), D3D_METHOD(Effect, GetCurrentTechnique),
    D3D_METHOD(Effect, ValidateTechnique), D3D_METHOD(Effect, FindNextValidTechnique),
    D3D_METHOD(Effect, IsParameterUsed), D3D_GUEST_NAMED("Begin", EffectBegin), D3D_GUEST_NAMED("Pass", EffectPass),
    D3D_GUEST_NAMED("End", EffectEnd), D3D_METHOD(Effect, GetDevice), D3D_METHOD(Effect, OnLostDevice),
    D3D_METHOD(Effect, OnResetDevice), D3D_TRAP(Slot70), D3D_TRAP(Slot71), D3D_TRAP(Slot72), D3D_TRAP(Slot73),
    D3D_TRAP(Slot74), D3D_TRAP(Slot75),
};
static_assert(sizeof(s_effect) / sizeof(s_effect[0]) == 76, "ID3DXEffect slots");

const MethodEntry s_pool[] = {
    D3D_NAMED("QueryInterface", Object, QueryInterface), D3D_NAMED("AddRef", Object, AddRef),
    D3D_NAMED("Release", Object, Release),
};

}

void initEffectInterfaces(WinApplication* app)
{
    buildInterface(app, KindEffect, "ID3DXEffect", s_effect, sizeof(s_effect) / sizeof(s_effect[0]));
    buildInterface(app, KindEffectPool, "ID3DXEffectPool", s_pool, sizeof(s_pool) / sizeof(s_pool[0]));
}

// ---- Direct calls (d3d9_native.h) ----------------------------------------------------------------------------------

namespace bridge { namespace effect
{

namespace
{

inline Effect* object(reg32 guest)
{
    return static_cast<Effect*>(lookup(guest));
}

}

HRESULT setInt(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, x86::sreg32 value)
{
    return object(effect)->SetInt(app, cpu, handle, value);
}

HRESULT setFloat(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, float value)
{
    return object(effect)->SetFloat(app, cpu, handle, value);
}

HRESULT setVector(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* vector)
{
    return object(effect)->SetVector(app, cpu, handle, vector);
}

HRESULT setVectorArray(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* vectors,
                       UINT count)
{
    return object(effect)->SetVectorArray(app, cpu, handle, vectors, count);
}

HRESULT setMatrix(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* matrix)
{
    return object(effect)->SetMatrix(app, cpu, handle, matrix);
}

HRESULT setMatrixArray(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, const float* matrices,
                       UINT count)
{
    return object(effect)->SetMatrixArray(app, cpu, handle, matrices, count);
}

HRESULT setTexture(WinApplication* app, x86::CPU& cpu, reg32 effect, reg32 handle, reg32 texture)
{
    return object(effect)->SetTexture(app, cpu, handle, lookup(texture));
}

HRESULT setProduct(reg32 effect, reg32 handle, const float* const* matrices, UINT count, uint32_t fpscr)
{
    return object(effect)->setProduct(handle, matrices, count, fpscr);
}

UINT passCount(reg32 effect)
{
    return object(effect)->passCount();
}

const uint8_t* paramTypes(reg32 effect)
{
    const Object* o = lookup(effect);
    return o && o->kind() == KindEffect ? static_cast<const Effect*>(o)->paramTypes() : nullptr;
}

HRESULT checkPass(reg32 effect, UINT pass)
{
    return object(effect)->checkPass(pass);
}

HRESULT begin(WinApplication* app, x86::CPU& cpu, reg32 effect, UINT* passes, DWORD flags)
{
    return object(effect)->Begin(app, cpu, passes, flags);
}

HRESULT pass(WinApplication* app, x86::CPU& cpu, reg32 effect, UINT pass)
{
    return object(effect)->Pass(app, cpu, pass);
}

HRESULT end(WinApplication* app, x86::CPU& cpu, reg32 effect)
{
    return object(effect)->End(app, cpu);
}

}}

// ---- Entry points --------------------------------------------------------------------------------------------------

namespace
{

HRESULT createEffectPool(WinApplication* app, x86::CPU& cpu, x86::reg32* pool)
{
    RC_USE(cpu);
    if (!pool)
        return D3DERR_INVALIDCALL;
    *pool = publish(app, new EffectPool());
    return D3D_OK;
}

HRESULT createEffect(WinApplication* app, x86::CPU& cpu, Device* device, const char* source, UINT length,
                     x86::reg32 defines, x86::reg32 include, DWORD flags, EffectPool* pool, x86::reg32* effect,
                     x86::reg32* errors)
{
    RC_USE(cpu);
    RC_USE(defines);
    RC_USE(include);
    RC_USE(flags);
    if (errors)
        *errors = 0;
    if (!device || !source || !effect)
        return D3DERR_INVALIDCALL;
    const std::string hash = pf::sha256Hex(reinterpret_cast<const uint8_t*>(source), length);
    int kind = -1;
    for (int k = 0; k < EffectCount; ++k)
    {
        if (hash == s_effects[k].sha256)
            kind = k;
    }
    if (kind < 0)
    {
        const std::string text(source, length);
        for (uint8_t k : s_markerOrder)
        {
            if (text.find(s_effects[k].marker) != std::string::npos)
            {
                kind = k;
                break;
            }
        }
        if (kind < 0)
        {
            pf::logError("d3dx9: unknown effect source (%u bytes, SHA-256 %s)", length, hash.c_str());
            return E_FAIL;
        }
        pf::logInfo("d3dx9: effect source %s differs from the known one; recognised as %s", hash.c_str(),
                    s_effects[kind].name);
    }
    *effect = publish(app, new Effect(device, pool, EffectKind(kind)));
    pf::logInfo("d3dx9: effect %s created", s_effects[kind].name);
    return D3D_OK;
}

float* hostFloats(x86::reg32 address)
{
    return reinterpret_cast<float*>(g_guestBase + address);
}

void store(x86::reg32 address, const float* values, uint32_t count)
{
    memcpy(g_guestBase + address, values, count * sizeof(float));
}

x86::reg32 matrixMultiply(WinApplication* app, x86::CPU& cpu, x86::reg32 out, x86::reg32 a, x86::reg32 b)
{
    RC_USE(app);
    RC_USE(cpu);
    float ma[16], mb[16], r[16];
    memcpy(ma, hostFloats(a), 64);
    memcpy(mb, hostFloats(b), 64);
    multiply(ma, mb, r);
    store(out, r, 16);
    return out;
}

x86::reg32 vecTransform(x86::reg32 out, const float v[4], x86::reg32 m)
{
    float mm[16], r[4];
    memcpy(mm, hostFloats(m), 64);
    d3dx9::transform(v, mm, r);
    store(out, r, 4);
    return out;
}

x86::reg32 vec3Transform(WinApplication* app, x86::CPU& cpu, x86::reg32 out, x86::reg32 v, x86::reg32 m)
{
    RC_USE(app);
    RC_USE(cpu);
    float in[4] = { 0, 0, 0, 1 };
    memcpy(in, hostFloats(v), 12);
    return vecTransform(out, in, m);
}

x86::reg32 vec4Transform(WinApplication* app, x86::CPU& cpu, x86::reg32 out, x86::reg32 v, x86::reg32 m)
{
    RC_USE(app);
    RC_USE(cpu);
    float in[4];
    memcpy(in, hostFloats(v), 16);
    return vecTransform(out, in, m);
}

x86::reg32 matrixTranspose(WinApplication* app, x86::CPU& cpu, x86::reg32 out, x86::reg32 m)
{
    RC_USE(app);
    RC_USE(cpu);
    float in[16], r[16];
    memcpy(in, hostFloats(m), 64);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = in[j * 4 + i];
    store(out, r, 16);
    return out;
}

}

}}

namespace win32 { namespace d3dx9
{

void CreateEffectPool(WinApplication* app, x86::CPU& cpu)
{
    d3d9::initInterfaces(app);
    d3d9::bridge::drain();
    Wrapper<decltype(&d3d9::createEffectPool), &d3d9::createEffectPool>::stdcall(app, cpu);
}

void CreateEffect(WinApplication* app, x86::CPU& cpu)
{
    d3d9::initInterfaces(app);
    d3d9::bridge::drain();
    Wrapper<decltype(&d3d9::createEffect), &d3d9::createEffect>::stdcall(app, cpu);
}

void MatrixMultiply(WinApplication* app, x86::CPU& cpu)
{
    Wrapper<decltype(&d3d9::matrixMultiply), &d3d9::matrixMultiply>::stdcall(app, cpu);
}

void Vec3Transform(WinApplication* app, x86::CPU& cpu)
{
    Wrapper<decltype(&d3d9::vec3Transform), &d3d9::vec3Transform>::stdcall(app, cpu);
}

void Vec4Transform(WinApplication* app, x86::CPU& cpu)
{
    Wrapper<decltype(&d3d9::vec4Transform), &d3d9::vec4Transform>::stdcall(app, cpu);
}

void MatrixTranspose(WinApplication* app, x86::CPU& cpu)
{
    Wrapper<decltype(&d3d9::matrixTranspose), &d3d9::matrixTranspose>::stdcall(app, cpu);
}

void transform(const float v[4], const float m[16], float r[4])
{
    for (int j = 0; j < 4; ++j)
        r[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
}

}}
