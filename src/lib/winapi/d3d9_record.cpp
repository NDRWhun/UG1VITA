// Recorded frames: the game thread appends state changes and draws to the frame's stream (d3d9_device.cpp,
// d3dx9.cpp); the render thread replays them into a mirror of the device and effect state and resolves each draw
// (programs, fixed function, alpha test, blend factor, constants) before the frame is drawn.

#include <winapi/d3d9_internal.h>
#include <platform/log.h>
#include <atomic>
#include <cstdlib>

namespace win32 { namespace d3d9
{

static_assert(sizeof(DrawRecord) % 4 == 0, "DrawRecord is whole words");
static_assert(sizeof(FixedState) % 4 == 0, "FixedState is whole words");

namespace
{

enum : DWORD
{
    D3DRS_ZENABLE = 7, D3DRS_ZWRITEENABLE = 14, D3DRS_ALPHATESTENABLE = 15, D3DRS_SRCBLEND = 19,
    D3DRS_DESTBLEND = 20, D3DRS_CULLMODE = 22, D3DRS_ZFUNC = 23, D3DRS_ALPHAREF = 24, D3DRS_ALPHAFUNC = 25,
    D3DRS_ALPHABLENDENABLE = 27, D3DRS_FOGENABLE = 28, D3DRS_TEXTUREFACTOR = 60, D3DRS_LIGHTING = 137,
    D3DRS_COLORWRITEENABLE = 168, D3DRS_BLENDOP = 171, D3DRS_BLENDFACTOR = 193,

    D3DSAMP_ADDRESSU = 1, D3DSAMP_ADDRESSV = 2, D3DSAMP_MAGFILTER = 5, D3DSAMP_MINFILTER = 6, D3DSAMP_MIPFILTER = 7
};

const float s_identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

// The recording of frame n is s_streams[n % StreamCount]; s_resolved: the last frame resolved.
const uint32_t StreamCount = 4;
Stream s_streams[StreamCount];
std::atomic<uint32_t> s_resolved{0};

uint8_t address(DWORD mode)
{
    switch (mode)
    {
    case 2:  return render::AddressMirror;
    case 3:
    case 4:  return render::AddressClamp;
    case 5:  return render::AddressMirror;
    default: return render::AddressWrap;
    }
}

render::Sampler sampler(const DWORD* states)
{
    render::Sampler s;
    s.addressU = address(states[D3DSAMP_ADDRESSU]);
    s.addressV = address(states[D3DSAMP_ADDRESSV]);
    s.filter = uint8_t((states[D3DSAMP_MINFILTER] >= 2 ? 1 : 0) | (states[D3DSAMP_MAGFILTER] >= 2 ? 2 : 0));
    s.mip = uint8_t(states[D3DSAMP_MIPFILTER] >= 2 ? render::MipLinear
                  : states[D3DSAMP_MIPFILTER] == 1 ? render::MipPoint : render::MipNone);
    return s;
}

// The device sampler states written after the pass (bits: Device::deviceSamplers of the stage) replace the pass's.
void deviceSampler(const DWORD* states, uint32_t bits, render::Sampler& s)
{
    const render::Sampler d = sampler(states);
    if (bits & DeviceAddressU)
        s.addressU = d.addressU;
    if (bits & DeviceAddressV)
        s.addressV = d.addressV;
    if (bits & DeviceMinFilter)
        s.filter = uint8_t((s.filter & ~1) | (d.filter & 1));
    if (bits & DeviceMagFilter)
        s.filter = uint8_t((s.filter & ~2) | (d.filter & 2));
    if (bits & DeviceMipFilter)
        s.mip = d.mip;
}

void color(DWORD argb, float out[4])
{
    out[0] = float((argb >> 16) & 255) / 255.0f;
    out[1] = float((argb >> 8) & 255) / 255.0f;
    out[2] = float(argb & 255) / 255.0f;
    out[3] = float(argb >> 24) / 255.0f;
}

// What the constants of a draw depend on; equal keys reuse the previous constants.
struct ConstantsKey
{
    uint32_t    source;         // 0 fixed function, else the effect's block and pass
    uint32_t    version;
    DWORD       alphaFunc, alphaRef;
    float       blendFactor[4];
    uint8_t     vertexProgram, fragmentProgram, texComponents, alphaTest;
};

// Render thread only.
class Resolver
{
public:
    void resolve(render::Frame& frame);

private:
    bool fillState(render::Frame& frame, const DrawRecord& record, render::DrawCall& draw);
    const float* block(uint16_t id) const
    {
        return id < m_blocks.size() && !m_blocks[id].empty() ? m_blocks[id].data() : nullptr;
    }

    FixedState                      m_state = {};
    std::vector<std::vector<float>> m_blocks;       // effect value blocks by id
    uint32_t                        m_fixedVersion = 1, m_effectVersion = 1;
    const render::TextureStorage*   m_lastTexture = nullptr;    // stage 0 of the last fixed-function draw
    uint32_t                        m_lastEffect = 0;
    bool                            m_keyValid = false;
    ConstantsKey                    m_key = {};
    uint32_t                        m_vertexConstants = render::NoConstants;
    uint32_t                        m_fragmentConstants = render::NoConstants;
};

Resolver s_resolver;

bool Resolver::fillState(render::Frame& f, const DrawRecord& record, render::DrawCall& draw)
{
    render::DrawState& state = draw.state;
    memset(&state, 0, sizeof(state));
    const DWORD* rs = m_state.renderStates;
    const bool positionT = record.positionT != 0;
    state.layout = record.layout;
    state.primitive = record.primitive;
    state.colorMask = uint8_t(rs[D3DRS_COLORWRITEENABLE] & 0xf);
    state.depthEnable = rs[D3DRS_ZENABLE] ? 1 : 0;
    state.depthWrite = rs[D3DRS_ZWRITEENABLE] ? 1 : 0;
    state.depthFunc = uint8_t(rs[D3DRS_ZFUNC]);
    state.cullMode = uint8_t(rs[D3DRS_CULLMODE]);
    if (rs[D3DRS_BLENDOP] != 1)
        D3D_LOG_ONCE("d3d9: blend operation %u approximated by ADD", rs[D3DRS_BLENDOP]);

    bool alphaEnabled = rs[D3DRS_ALPHATESTENABLE] != 0;
    DWORD alphaRef = rs[D3DRS_ALPHAREF];
    uint32_t source = 0;
    uint32_t version = m_fixedVersion;
    FixedResult fixed = {};
    const EffectDraw& effect = record.effect;
    const float* own = nullptr;
    const float* shared = nullptr;

    if (effect.kind != NoEffect)
    {
        own = block(effect.own);
        shared = block(effect.shared);
        if (!own)
        {
            D3D_LOG_ONCE("d3d9: effect draw without parameter values dropped");
            draw.count = 0;
            return true;
        }
        EffectBinding b;
        memset(&b, 0, sizeof(b));
        const bool projected = (m_state.stageStates[0][D3DTSS_TEXTURETRANSFORMFLAGS] & D3DTTFF_PROJECTED) != 0 &&
                               record.texComponents == 4;
        effectBinding(effect, own, shared, projected, b);
        state.vertexProgram = b.vertexProgram;
        state.fragmentProgram = b.fragmentProgram;
        for (uint32_t i = 0; i < b.samplerCount && i < render::MaxSamplers; ++i)
        {
            state.textures[i] = record.textures[i];
            state.samplers[i] = b.samplers[i];
            const int stage = record.deviceSamplers ? effectSamplerStage(effect, i) : -1;
            if (stage >= 0)
                deviceSampler(m_state.samplerStates[stage], record.deviceSamplers >> (stage * 8), state.samplers[i]);
        }
        if (b.cullNone)
            state.cullMode = 1;
        if (b.depthOff)
            state.depthEnable = state.depthWrite = 0;
        state.blendEnable = b.blend ? 1 : 0;
        state.srcBlend = uint8_t(b.srcBlend);
        state.dstBlend = uint8_t(b.dstBlend);
        alphaEnabled = b.alphaTest;
        alphaRef = b.alphaRef;
        source = 1u + effect.own + (uint32_t(effect.pass) << 16);
        if (source != m_lastEffect)
            ++m_effectVersion;
        m_lastEffect = source;
        version = m_effectVersion;
    }
    else
    {
        if (rs[D3DRS_LIGHTING] && !positionT)
            D3D_LOG_ONCE("d3d9: fixed-function lighting is not emulated (vertex colours used)");
        if (rs[D3DRS_FOGENABLE])
            D3D_LOG_ONCE("d3d9: fixed-function fog is not emulated");
        const DWORD* tss = m_state.stageStates[0];
        const render::TextureStorage* texture = record.textures[0];
        if (texture != m_lastTexture)
            version = ++m_fixedVersion;
        m_lastTexture = texture;
        if (texture && texture->desc.cube)
        {
            D3D_LOG_ONCE("d3d9: cube texture on fixed-function stage 0 ignored");
            texture = nullptr;
        }
        if (m_state.stageStates[1][D3DTSS_COLOROP] != D3DTOP_DISABLE)
            D3D_LOG_ONCE("d3d9: fixed-function stages after 0 are not emulated");
        FixedStage stage = { tss[D3DTSS_COLOROP], tss[D3DTSS_COLORARG1], tss[D3DTSS_COLORARG2], tss[D3DTSS_ALPHAOP],
                             tss[D3DTSS_ALPHAARG1], tss[D3DTSS_ALPHAARG2], tss[D3DTSS_TEXTURETRANSFORMFLAGS],
                             texture != nullptr };
        fixed = resolveFixed(stage);
        state.vertexProgram = positionT ? render::VpFixedScreen : render::VpFixed;
        state.fragmentProgram = fixed.fragmentProgram;
        if (fixed.fragmentProgram != render::FpFixedColor)
        {
            state.textures[0] = texture;
            state.samplers[0] = sampler(m_state.samplerStates[0]);
        }
        state.blendEnable = rs[D3DRS_ALPHABLENDENABLE] ? 1 : 0;
        state.srcBlend = uint8_t(rs[D3DRS_SRCBLEND]);
        state.dstBlend = uint8_t(rs[D3DRS_DESTBLEND]);
    }

    float test[2] = { 0.0f, 0.0f };
    state.alphaTest = alphaEnabled && alphaTest(rs[D3DRS_ALPHAFUNC], alphaRef, test) ? 1 : 0;

    const render::FragmentProgramInfo& fragment =
        render::fragmentProgram(render::FragmentProgram(state.fragmentProgram));
    BlendFactorDraw factor = {};
    if (state.blendEnable)
    {
        factor = resolveBlendFactor(state.srcBlend, state.dstBlend, rs[D3DRS_BLENDOP], rs[D3DRS_BLENDFACTOR],
                                    fragment.blendFactor && !state.alphaTest, (state.colorMask & 8) != 0);
        state.srcBlend = uint8_t(factor.src);
        state.dstBlend = uint8_t(factor.dst);
        state.factorOutput = factor.output;
    }

    ConstantsKey key;
    memset(&key, 0, sizeof(key));
    key.source = source;
    key.version = version;
    key.alphaFunc = rs[D3DRS_ALPHAFUNC];
    key.alphaRef = alphaRef;
    memcpy(key.blendFactor, factor.factor, sizeof(key.blendFactor));
    key.vertexProgram = state.vertexProgram;
    key.fragmentProgram = state.fragmentProgram;
    key.texComponents = uint8_t(record.texComponents | (positionT ? 0x80 : 0));
    key.alphaTest = state.alphaTest;
    if (m_keyValid && memcmp(&key, &m_key, sizeof(key)) == 0)
    {
        draw.vertexConstants = m_vertexConstants;
        draw.fragmentConstants = m_fragmentConstants;
        return true;
    }

    const uint32_t vertexFloats = render::vertexProgram(render::VertexProgram(state.vertexProgram)).constantFloats;
    const uint32_t fragmentFloats = fragment.constantFloats;
    float* vc = nullptr;
    float* fc = nullptr;
    draw.vertexConstants = draw.fragmentConstants = render::NoConstants;
    if (vertexFloats && !(vc = render::allocateConstants(f, vertexFloats, &draw.vertexConstants)))
        return false;
    if (fragmentFloats && !(fc = render::allocateConstants(f, fragmentFloats, &draw.fragmentConstants)))
        return false;

    if (effect.kind != NoEffect)
    {
        if (vc)
            effectVertexConstants(effect, own, shared, vc);
        if (fc)
            effectFragmentConstants(effect, own, shared, fc);
    }
    else
    {
        render::FixedVertexConstants& v = *reinterpret_cast<render::FixedVertexConstants*>(vc);
        float wv[16];
        multiply(m_state.transforms[TransformWorld].m, m_state.transforms[TransformView].m, wv);
        multiply(wv, m_state.transforms[TransformProjection].m, v.wvp);
        // Window coordinates of the viewport to clip space.
        const D3DVIEWPORT9& vp = m_state.viewport;
        const float w = float(vp.Width ? vp.Width : 1), h = float(vp.Height ? vp.Height : 1);
        v.screen[0] = 2.0f / w;
        v.screen[1] = -2.0f / h;
        v.screen[2] = -1.0f - 2.0f * float(vp.X) / w;
        v.screen[3] = 1.0f + 2.0f * float(vp.Y) / h;
        const DWORD flags = m_state.stageStates[0][D3DTSS_TEXTURETRANSFORMFLAGS];
        memcpy(v.texMatrix, (flags & 0xff) ? m_state.transforms[TransformTexture0].m : s_identity, sizeof(v.texMatrix));
        if ((flags & D3DTTFF_PROJECTED) && (flags & 0xff) == D3DTTFF_COUNT4)
        {
            // Divided by the fourth coordinate: move it to the third, which the program divides by.
            for (int row = 0; row < 4; ++row)
                v.texMatrix[row * 4 + 2] = v.texMatrix[row * 4 + 3];
        }
        texturePad(record.texComponents, v.texPad);
        memcpy(v.diffuseMask, fixed.diffuseMask, sizeof(v.diffuseMask));
        memcpy(v.factorMask, fixed.factorMask, sizeof(v.factorMask));
        color(rs[D3DRS_TEXTUREFACTOR], v.factor);
        render::FixedFragmentConstants& p = *reinterpret_cast<render::FixedFragmentConstants*>(fc);
        memset(&p, 0, sizeof(p));
        memcpy(p.textureMask, fixed.textureMask, sizeof(p.textureMask));
        memcpy(p.scale, fixed.scale, sizeof(p.scale));
        memcpy(p.addMask, fixed.addMask, sizeof(p.addMask));
    }
    if (fc && fragmentFloats >= 2)
    {
        fc[0] = test[0];
        fc[1] = test[1];
    }
    if (fc && state.factorOutput)
        memcpy(fc + fragment.blendFactor, factor.factor, sizeof(factor.factor));
    m_key = key;
    m_keyValid = true;
    m_vertexConstants = draw.vertexConstants;
    m_fragmentConstants = draw.fragmentConstants;
    return true;
}

void Resolver::resolve(render::Frame& f)
{
    m_keyValid = false;
    bool full = false;
    const uint32_t* w = f.commands;
    const uint32_t* end = w + f.commandCount;
    while (w < end)
    {
        const uint32_t header = *w++;
        const uint32_t a = (header >> 8) & 0xff, b = header >> 16;
        switch (Command(header & 0xff))
        {
        case CmdRenderState:
            m_state.renderStates[header >> 8 & 0xff] = *w++;
            ++m_fixedVersion;
            break;
        case CmdStageState:
            m_state.stageStates[a & 7][b < 33 ? b : 0] = *w++;
            ++m_fixedVersion;
            break;
        case CmdSamplerState:
            m_state.samplerStates[a & 15][b < 14 ? b : 0] = *w++;
            ++m_fixedVersion;
            break;
        case CmdTransform:
            memcpy(&m_state.transforms[a < TransformCount ? a : 0], w, sizeof(D3DMATRIX));
            w += 16;
            ++m_fixedVersion;
            break;
        case CmdViewport:
            memcpy(&m_state.viewport, w, sizeof(D3DVIEWPORT9));
            w += sizeof(D3DVIEWPORT9) / 4;
            ++m_fixedVersion;
            break;
        case CmdFixedState:
            memcpy(&m_state, w, sizeof(FixedState));
            w += sizeof(FixedState) / 4;
            ++m_fixedVersion;
            break;
        case CmdValues:
        {
            const uint32_t id = header >> 8, offset = w[0] & 0xffff, count = w[0] >> 16;
            if (id >= m_blocks.size())
                m_blocks.resize(id + 1);
            std::vector<float>& values = m_blocks[id];
            if (values.empty())
                values.assign(effectBlockFloats(), 0.0f);
            if (offset + count <= values.size())
                memcpy(values.data() + offset, w + 1, count * 4);
            w += 1 + count;
            ++m_effectVersion;
            break;
        }
        case CmdDraw:
        {
            DrawRecord r;
            memcpy(&r, w, sizeof(r));
            w += sizeof(r) / 4;
            render::DrawCall& d = f.draws[r.draw];
            if (!full && !fillState(f, r, d))
            {
                pf::logError("d3d9: frame %u: constant arena full, draws dropped", f.index);
                full = true;
            }
            if (full)
            {
                memset(&d.state, 0, sizeof(d.state));
                d.count = 0;
            }
            break;
        }
        default:
            pf::fatal("d3d9: frame %u: bad command 0x%08x", f.index, header);
        }
    }
    s_resolved.store(f.index, std::memory_order_release);
}

}

void Stream::grow(uint32_t words)
{
    uint32_t capacity = this->capacity ? this->capacity : 64 * 1024;
    while (capacity < size + words)
        capacity *= 2;
    uint32_t* grown = static_cast<uint32_t*>(realloc(data, capacity * 4));
    if (!grown)
        pf::fatal("d3d9: out of memory for a %u KB frame recording", capacity / 256);
    data = grown;
    this->capacity = capacity;
}

Stream& frameStream(uint32_t frameIndex)
{
    if (frameIndex > StreamCount && s_resolved.load(std::memory_order_acquire) < frameIndex - StreamCount)
        pf::fatal("d3d9: frame %u begun before frame %u was resolved", frameIndex, frameIndex - StreamCount);
    Stream& s = s_streams[frameIndex % StreamCount];
    s.size = 0;
    return s;
}

void resolveFrame(render::Frame& frame)
{
    s_resolver.resolve(frame);
}

}}
