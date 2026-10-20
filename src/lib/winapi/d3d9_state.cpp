// Fixed-function stage 0 and alpha test resolution of the Direct3D layer: pure functions.

#include <winapi/d3d9_internal.h>

namespace win32 { namespace d3d9
{

namespace
{

// One channel group (rgb or alpha) of the stage result: mix(1, texel, texture) * diffuse^d * factor^f * scale
// + texel * add.
struct Channel
{
    float texture, diffuse, factor, scale, add;
};

void addArgument(Channel& c, DWORD arg, bool textured)
{
    if (arg & ~D3DTA_SELECTMASK)
        D3D_LOG_ONCE("d3d9: texture argument modifiers 0x%x ignored", arg & ~D3DTA_SELECTMASK);
    switch (arg & D3DTA_SELECTMASK)
    {
    case D3DTA_TEXTURE:
        // Stage 0 without a texture reads white.
        if (textured)
        {
            if (c.texture != 0.0f)
                D3D_LOG_ONCE("d3d9: texture * texture approximated by texture");
            c.texture = 1.0f;
        }
        break;
    case D3DTA_DIFFUSE:
    case D3DTA_CURRENT:
        c.diffuse = 1.0f;
        break;
    case D3DTA_TFACTOR:
        c.factor = 1.0f;
        break;
    default:
        D3D_LOG_ONCE("d3d9: texture argument %u approximated by the diffuse colour", arg & D3DTA_SELECTMASK);
        c.diffuse = 1.0f;
        break;
    }
}

Channel resolveOp(DWORD op, DWORD arg1, DWORD arg2, bool textured)
{
    Channel c = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    switch (op)
    {
    case D3DTOP_SELECTARG1:
        addArgument(c, arg1, textured);
        break;
    case D3DTOP_SELECTARG2:
        addArgument(c, arg2, textured);
        break;
    case D3DTOP_MODULATE:
    case D3DTOP_MODULATE2X:
    case D3DTOP_MODULATE4X:
        addArgument(c, arg1, textured);
        addArgument(c, arg2, textured);
        c.scale = op == D3DTOP_MODULATE ? 1.0f : op == D3DTOP_MODULATE2X ? 2.0f : 4.0f;
        break;
    case D3DTOP_ADD:
    {
        const bool texture1 = (arg1 & D3DTA_SELECTMASK) == D3DTA_TEXTURE;
        const bool texture2 = (arg2 & D3DTA_SELECTMASK) == D3DTA_TEXTURE;
        if (texture1 != texture2)
        {
            // texel + other: the other argument as the multiplied term, the texel added.
            addArgument(c, texture1 ? arg2 : arg1, textured);
            c.add = textured ? 1.0f : 0.0f;
            if (!textured)
                c.diffuse = 1.0f;
            break;
        }
        D3D_LOG_ONCE("d3d9: ADD of arguments 0x%x and 0x%x approximated by MODULATE", arg1, arg2);
        addArgument(c, arg1, textured);
        addArgument(c, arg2, textured);
        break;
    }
    case D3DTOP_DISABLE:
        c.diffuse = 1.0f;
        break;
    default:
        D3D_LOG_ONCE("d3d9: texture operation %u approximated by MODULATE", op);
        addArgument(c, arg1, textured);
        addArgument(c, arg2, textured);
        break;
    }
    return c;
}

}

FixedResult resolveFixed(const FixedStage& stage)
{
    const Channel color = resolveOp(stage.colorOp, stage.colorArg1, stage.colorArg2, stage.textured);
    // An alpha operation is not used when the colour operation is DISABLE: the result is the diffuse colour.
    const Channel alpha = stage.colorOp == D3DTOP_DISABLE
                        ? resolveOp(D3DTOP_DISABLE, 0, 0, false)
                        : resolveOp(stage.alphaOp, stage.alphaArg1, stage.alphaArg2, stage.textured);
    FixedResult r;
    for (int i = 0; i < 4; ++i)
    {
        const Channel& c = i < 3 ? color : alpha;
        r.textureMask[i] = c.texture;
        r.scale[i] = c.scale;
        r.addMask[i] = c.add;
        r.diffuseMask[i] = c.diffuse;
        r.factorMask[i] = c.factor;
    }
    const bool sampled = color.texture != 0.0f || color.add != 0.0f || alpha.texture != 0.0f || alpha.add != 0.0f;
    if (!sampled)
        r.fragmentProgram = render::FpFixedColor;
    else if ((stage.transformFlags & D3DTTFF_PROJECTED) && (stage.transformFlags & 0xff) >= D3DTTFF_COUNT3)
        r.fragmentProgram = render::FpFixedProjected;
    else
        r.fragmentProgram = render::FpFixedTexture;
    return r;
}

void texturePad(uint32_t components, float pad[4])
{
    // The GPU fills missing components with (0, 0, 0, 1); Direct3D expects 1 after the last and 0 beyond.
    pad[0] = pad[1] = pad[2] = pad[3] = 0.0f;
    switch (components)
    {
    case 1: pad[1] = 1.0f; pad[3] = -1.0f; break;
    case 2: pad[2] = 1.0f; pad[3] = -1.0f; break;
    default: break;
    }
}

bool alphaTest(DWORD func, DWORD ref, float out[2])
{
    const float r = float(ref & 0xff) / 255.0f;
    const float half = 0.5f / 255.0f;
    switch (func)
    {
    case 1:     // NEVER
        out[0] = 0.0f;
        out[1] = 1.0f;
        return true;
    case 2:     // LESS: discard a >= r
        out[0] = -1.0f;
        out[1] = -(r - half);
        return true;
    case 4:     // LESSEQUAL: discard a > r
        out[0] = -1.0f;
        out[1] = -(r + half);
        return true;
    case 5:     // GREATER: discard a <= r
        out[0] = 1.0f;
        out[1] = r + half;
        return true;
    case 3:     // EQUAL
        D3D_LOG_ONCE("d3d9: alpha test EQUAL approximated by GREATEREQUAL");
        out[0] = 1.0f;
        out[1] = r - half;
        return true;
    case 7:     // GREATEREQUAL: discard a < r
        out[0] = 1.0f;
        out[1] = r - half;
        return true;
    case 6:     // NOTEQUAL
        D3D_LOG_ONCE("d3d9: alpha test NOTEQUAL approximated by ALWAYS");
        return false;
    default:    // ALWAYS
        return false;
    }
}

void multiply(const float* a, const float* b, float* out)
{
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(out, r, sizeof(r));
}

}}
