// Native light contexts (lights.h): guest 0x5437f0, 0x542e40, 0x542c20, 0x544390, 0x542400 and their helpers
// 0x5413e0 (with 0x5414a0, 0x5411b0, 0x58dc50, 0x58dca0), 0x542360, 0x541bf0, 0x541fc0, 0x542dc0, 0x541c90 and
// 0x542940. Every x87 chain runs in double in the guest's order; values are rounded to float where the guest stores
// floats, its locals included. The context is built in a host copy and written to guest memory at the end, so a
// call the native path does not cover leaves guest memory as it was for the guest code to run.

#include "lights.h"
#include "mathfn.h"
#include "retstate.h"

#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <winapi/d3d9_internal.h>
#include <winapi/d3dx9.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fpu.h>
#include <recomp.h>

namespace nfsu
{

// The translated guest bodies of the overridden functions (generated).
void sub_5437f0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_542e40_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_542c20_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_544390_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_542400_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace lights
{

namespace
{

using x86::reg8;
using x86::reg16;
using x86::reg32;
using x86::reg64;
using x86::sreg16;
using x86::sreg32;
using x86::sreg64;

// Float constants in .rdata and the game's light settings in .data.
const reg32 Zero = 0x6cc7a4;
const reg32 One = 0x6cc7bc;
const reg32 Fixed16 = 0x6cc840;         // 65536
const reg32 TwoPow32 = 0x6cc858;
const reg32 Ninety = 0x6cccd4;
const reg32 OneEighty = 0x6ccb3c;
const reg32 Bright = 0x6b6340;          // 1.5
const reg32 SpotScale = 0x6cccd0;
const reg32 SpotBias = 0x6ccba8;
const reg32 EnvMin = 0x6b6348;
const reg32 EnvMax = 0x6b634c;
const reg32 Inv255 = 0x6cc970;
const reg32 K255 = 0x6cc974;
const reg32 LumaRed = 0x6cccc4;
const reg32 LumaGreen = 0x6ccccc;
const reg32 LumaBlue = 0x6cccc8;
const reg32 BandWeight = 0x6b6344;
const reg32 BackScale = 0x6cca98;
const reg32 Poly = 0x6cc864;            // x^6 .. x terms at +0, +4, +8, +0xc, +0x10, +0x14
const reg32 Exposure = 0x6f3ee0;
const reg32 ExposureLimit = 0x6f3ee4;
const reg32 Scale50 = 0x6f3df8;
const reg32 NormaliseLevel = 0x6f3e04;  // the three arguments of 0x541fc0
const reg32 NormaliseAlpha = 0x6f3e08;
const reg32 NormaliseAlphaLevel = 0x6f3e0c;
const reg32 NormaliseColour = 0x735548;
const reg32 LightTable = 0x6f3e18;      // directional lights: 7 entries of azimuth, elevation, intensity from +0xc
const reg32 LightTableAlt = 0x6f3e78;
const reg32 GameMode = 0x77a920;
const reg32 FillPath = 0x6f3edc;        // 0: the other fill path (guest code)
const reg32 CloneFill = 0x73554c;       // not 0: the clone fills the context instead
const reg32 LightTrees = 0x740a68;      // list head and sentinel
const reg32 LightGroups = 0x743c10;

const reg32 ContextSize = 0x1c0;
const int ContextFloats = 0x70;
const int MaxLights = 10;
const int Directions = 19;              // 7 table lights, up to 11 query lights, one spare

inline double K(reg32 address) { return double(rc::ldf32(address)); }
inline float bitsFloat(reg32 bits) { float f; memcpy(&f, &bits, 4); return f; }
inline reg32 floatBits(float f) { reg32 b; memcpy(&b, &f, 4); return b; }
// fld then fstp of a float.
inline float roundTrip(float f) { return float(double(f)); }

struct Settings
{
    bool native;
    bool shadow;
};

const Settings& settings()
{
    static const Settings s = []
    {
        const Settings r = { pf::Config::getBool("lights_native", true), pf::Config::getBool("shadow_lights", false) };
        pf::logInfo("lights: light contexts %s%s", r.native ? "native" : "guest",
                    r.native && r.shadow ? ", shadow compare" : "");
        return r;
    }();
    return s;
}

// A light context in host memory, as 0x70 floats.
struct Context
{
    float f[ContextFloats];
    float& at(reg32 offset) { return f[offset / 4]; }
    void load(reg32 address) { memcpy(f, rc::host(address), ContextSize); }
    void store(reg32 address) const { memcpy(rc::host(address), f, ContextSize); }
};

// What the guest leaves in eax, ecx and edx.
struct Exit
{
    reg32 eax, ecx, edx;
};

// ---- matrices and vectors ----

void loadMatrix(reg32 address, float m[16])
{
    memcpy(m, rc::host(address), 64);
}

// D3DXMatrixTranspose of the matrix at address with the translation row and the w column cleared.
void rotationTranspose(reg32 address, float t[16])
{
    float m[16];
    loadMatrix(address, m);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            t[i * 4 + j] = m[j * 4 + i];
    t[3] = t[7] = t[11] = t[15] = bitsFloat(0);
    t[12] = t[13] = t[14] = bitsFloat(0);
}

// Guest 0x567370: dst = src normalised, (1, 0, 0) for length 0.
void normalize(const float src[3], float dst[3])
{
    const double z = double(src[2]), y = double(src[1]), x = double(src[0]);
    double len = x * x;
    len += y * y;
    len += z * z;
    len = std::sqrt(len);
    if (len == K(Zero))
    {
        dst[0] = bitsFloat(0x3f800000u);
        dst[1] = bitsFloat(0);
        dst[2] = bitsFloat(0);
        return;
    }
    const double inv = K(One) / len;
    const float sy = src[1], sz = src[2];
    dst[0] = float(inv * double(src[0]));
    dst[1] = float(double(sy) * inv);
    dst[2] = float(double(sz) * inv);
}

// Guest 0x567450: dst = src scaled by 1 / sqrt(x^2 + y^2 + z^2 + 2 w), (1, 0, 0) for 0. Returns the length test:
// false for length 0.
bool normalize4(const float src[4], float dst[3])
{
    const double z = double(src[2]), y = double(src[1]), x = double(src[0]);
    double len = x * x;
    len += y * y;
    len += z * z;
    len += double(src[3]);
    len += double(src[3]);
    len = std::sqrt(len);
    if (len == K(Zero))
    {
        dst[0] = bitsFloat(0x3f800000u);
        dst[1] = bitsFloat(0);
        dst[2] = bitsFloat(0);
        return false;
    }
    const double inv = K(One) / len;
    const float sy = src[1], sz = src[2];
    dst[0] = float(inv * double(src[0]));
    dst[1] = float(double(sy) * inv);
    dst[2] = float(double(sz) * inv);
    return true;
}

// Guest 0x542360: the direction of an azimuth and an elevation in degrees, scaled by r.
void sphere(float azimuth, float elevation, float r, float out[3])
{
    const auto angle = [](float degrees)
    {
        const sreg32 i = sreg32(reg32(x86::truncInteger64(double(degrees) * K(Fixed16))));
        const sreg32 high = sreg32(reg32(uint64_t(sreg64(sreg32(0xb60b60b7u)) * sreg64(i)) >> 32));
        sreg32 a = sreg32(reg32(high) + reg32(i)) >> 8;
        a = sreg32(reg32(a) + (reg32(a) >> 31));
        return reg32(a);
    };
    float s1, c1, s2, c2;
    mathfn::sinCos16(angle(azimuth), s1, c1);
    mathfn::sinCos16(angle(elevation), s2, c2);
    out[0] = float(double(s2) * double(c1) * double(r));
    out[1] = float(double(s2) * double(s1) * double(r));
    out[2] = float(double(c2) * double(r));
}

// ---- light database query (0x5413e0) ----

struct Query
{
    reg32 light[MaxLights];
    float weight[MaxLights];
    int count;
    int ambient;            // entries of the ambient list (they bound the first list's walk)
};

// Guest 0x58dc50: the point lies inside the box (centre at +0, half sizes at +0x10).
inline bool inside(reg32 box, float px, float py, float pz)
{
    const double dx = std::fabs(double(px) - K(box));
    const double hx = K(box + 0x10);
    if (hx < dx || hx == dx)
        return false;
    const double dy = std::fabs(double(py) - K(box + 4));
    const double hy = K(box + 0x14);
    if (hy < dy || hy == dy)
        return false;
    const double dz = std::fabs(double(pz) - K(box + 8));
    const double hz = K(box + 0x18);
    return !(hz < dz || hz == dz);
}

// Guest 0x58dca0: the leaf of the tree whose box holds the point, depth first; 0 when none.
reg32 searchTree(reg32 tree, reg32 node, float px, float py, float pz)
{
    const sreg32 children = sreg16(rc::ld16(node + 0xe));
    for (sreg32 k = 0; k < children; ++k)
    {
        const sreg32 index = sreg16(rc::ld16(node + 0x1c + 2 * reg32(k)));
        const reg32 child = rc::ld32(tree) + reg32(index) * 0x30u;
        if (!inside(child, px, py, pz))
            continue;
        if (sreg16(rc::ld16(child + 0xe)) <= 0)
            return child;
        const reg32 found = searchTree(tree, child, px, py, pz);
        if (found != 0)
            return found;
    }
    return 0;
}

// Guest 0x5411b0: one light record against the point. The ambient lights only count (their colours feed the
// query's ambient result, which no caller reads).
void consider(Query& q, reg32 light, float px, float py, float pz)
{
    if (rc::ld8(light + 7) != 1)
        return;
    const double dx = std::fabs(double(px) - K(light + 0x10));
    const double dy = std::fabs(double(py) - K(light + 0x14));
    const double dz = std::fabs(double(pz) - K(light + 0x18));
    const double r = K(light + 0x1c);
    const float r2 = float(r * r);
    const float d2 = float((dz * dz + dy * dy) + dx * dx);
    if (!(double(d2) < double(r2)))
        return;
    const double inner = K(light + 0x30);
    const double ff = inner * inner;
    float w = bitsFloat(0x3f800000u);
    if (double(d2) > ff)
        w = float(K(One) - (double(d2) - ff) / (double(r2) - ff));
    const reg8 type = rc::ld8(light + 4);
    if (type != 0)
    {
        if (type == 2 || type == 3)
        {
            if (q.count < MaxLights)
            {
                q.light[q.count] = light;
                q.weight[q.count] = w;
            }
            ++q.count;
        }
        return;
    }
    if (rc::ld8(light + 5) == 2 && q.ambient < MaxLights && double(w) > K(Zero))
        ++q.ambient;
}

// The lights at the point: the light trees of the loaded sections (at most 10 lights, and only while the ambient
// list has room), then every light of the light groups.
void query(Query& q, reg32 position)
{
    const float px = rc::ldf32(position), py = rc::ldf32(position + 4), pz = rc::ldf32(position + 8);
    q.count = 0;
    q.ambient = 0;
    for (reg32 node = rc::ld32(LightTrees); node != LightTrees; node = rc::ld32(node))
    {
        if (q.count >= MaxLights || q.ambient >= MaxLights)
            break;
        const reg32 tree = rc::ld32(node + 0xc);
        const reg32 root = rc::ld32(tree);
        if (root == 0 || !inside(root, px, py, pz))
            continue;
        const reg32 leaf = sreg16(rc::ld16(root + 0xe)) > 0 ? searchTree(tree, root, px, py, pz) : root;
        if (leaf == 0)
            continue;
        const sreg32 n = -sreg32(sreg16(rc::ld16(leaf + 0xe)));
        const reg32 base = rc::ld32(node + 0x14);
        for (sreg32 i = 0; i < n; ++i)
        {
            if (q.count >= MaxLights)
                break;
            const sreg32 index = sreg16(rc::ld16(leaf + 0x1c + 2 * reg32(i)));
            consider(q, base + reg32(index) * 0x60u, px, py, pz);
        }
    }
    for (reg32 node = rc::ld32(LightGroups); node != LightGroups; node = rc::ld32(node))
    {
        if (rc::ld32(node + 0x28) == 0)
            continue;
        const reg32 end = node + 0x2c;
        for (reg32 e = rc::ld32(end); e != 0 && e != end; e = rc::ld32(e))
            consider(q, e + 8, px, py, pz);
    }
}

// ---- spherical harmonics ----

// Guest 0x541bf0: the coefficients of a coloured direction and of its back side.
void band(float* coefficients, const float colour[4], const float direction[4])
{
    const float k = rc::ldf32(BandWeight);
    mathfn::irradiance(coefficients, colour, k, direction[0], direction[1], direction[2]);
    float back[4];
    for (int i = 0; i < 4; ++i)
        back[i] = float(double(colour[i]) * K(BackScale));
    mathfn::irradiance(coefficients, back, k, float(-double(direction[0])), float(-double(direction[1])),
                       float(-double(direction[2])));
}

// Guest 0x541fc0: with colour set, rows of the colour columns scaled so the largest column has length level; with
// alpha set, the alpha column scaled to length alphaLevel.
void normaliseCoefficients(float* c, bool colour, float level, bool alpha, float alphaLevel)
{
    if (colour)
    {
        double largest = K(Zero);
        for (int ch = 0; ch < 3; ++ch)
        {
            // Guest order of the squares: rows 2, 3, 4, 5, 6, 7, 8, 0, 9, 1 for the first column, 0..9 for the others.
            static const int Order0[10] = { 2, 3, 4, 5, 6, 7, 8, 0, 9, 1 };
            double s = 0;
            for (int k = 0; k < 10; ++k)
            {
                const int row = ch == 0 ? Order0[k] : k;
                const double v = double(c[row * 4 + ch]);
                s = k == 0 ? v * v : s + v * v;
            }
            const float sf = float(s);
            if (double(sf) > (ch == 0 ? K(Zero) : largest))
                largest = double(sf);
        }
        const double length = std::sqrt(largest);
        if (length > K(Zero))
        {
            const double k = double(level) / length;
            for (int ch = 0; ch < 3; ++ch)
                for (int row = 0; row < 10; ++row)
                    c[row * 4 + ch] = float(k * double(c[row * 4 + ch]));
        }
    }
    if (alpha)
    {
        double s = 0;
        for (int row = 0; row < 10; ++row)
        {
            const double v = double(c[row * 4 + 3]);
            s = row == 0 ? v * v : s + v * v;
        }
        const float length = float(std::sqrt(s));
        if (double(length) > K(Zero))
        {
            const double k = double(alphaLevel) / double(length);
            for (int row = 0; row < 10; ++row)
                c[row * 4 + 3] = float(k * double(c[row * 4 + 3]));
        }
    }
}

// Guest 0x542dc0: 1 / p(x)^4 with p the polynomial of the constants at Poly.
double falloff(float x)
{
    const double a = double(x);
    const double p = a * K(Poly + 0x14) + K(One);
    const double a2 = a * a;
    const float t1 = float(K(Poly + 0x10) * a2 + p);
    const double a3 = a2 * a;
    const double q = K(Poly + 0xc) * a3 + double(t1);
    const double a4 = a3 * a;
    const float a4f = float(a4);
    const double q2 = q + a4 * K(Poly + 8);
    const double a5 = double(a4f) * a;
    const double r3 = K(Poly + 4) * a5 + q2;
    const double a6 = a5 * a;
    const double r4 = a6 * K(Poly) + r3;
    const double r2 = r4 * r4;
    const double r3p = r2 * r4;
    return K(One) / (r4 * r3p);
}

// Guest 0x541c90: the coefficients (10 rows of 4) rotated by the 3x3 part of the matrix t, column by column.
void rotateCoefficients(float* co, const float t[16])
{
    // The guest's stack frame: matrix elements at 0..0x28, temporaries above.
    float s[26];
    s[1] = t[0];
    s[0] = t[4];
    s[2] = t[8];
    s[5] = t[1];
    s[4] = t[5];
    s[3] = t[9];
    s[10] = t[2];
    s[9] = t[6];
    s[8] = t[10];
    for (int column = 0; column < 4; ++column)
    {
        float* const p = co + 8 + column;
        const float r_edx_1 = p[8];
        const double t1 = double(p[-4]);
        const double t2 = double(p[0]);
        s[7] = r_edx_1;
        const float r_edx_9 = p[12];
        const double t3 = double(p[4]);
        s[6] = r_edx_9;
        const float r_edx_15 = p[16];
        s[24] = r_edx_15;
        const double t4 = double(s[24]);
        const float r_edx_21 = p[20];
        const double t5 = t4 * double(s[0]);
        s[16] = r_edx_21;
        const float r_edx_28 = p[24];
        const double t6 = t3 * double(s[2]);
        s[15] = r_edx_28;
        const float r_edx_34 = p[28];
        s[14] = r_edx_34;
        const double t7 = t5 + t6;
        const float r_edx_40 = p[-8];
        const double t8 = double(s[6]);
        p[-8] = r_edx_40;
        const double t9 = t8 * double(s[1]);
        const double t10 = t7 + t9;
        const double t11 = t10 * double(s[2]);
        const double t12 = t2 * double(s[0]);
        const double t13 = double(s[7]);
        const double t14 = t13 * double(s[1]);
        const double t15 = t12 + t14;
        const double t16 = t15 * double(s[0]);
        const double t17 = t11 + t16;
        const double t18 = t1 * double(s[1]);
        const double t19 = t18 * double(s[1]);
        const double t20 = t17 + t19;
        p[-4] = (float(t20));
        const double t21 = double(s[24]);
        const double t22 = t21 * double(s[4]);
        s[22] = (float(t22));
        const double t23 = double(s[3]);
        const double t24 = t23 * t3;
        s[21] = (float(t24));
        const double t25 = double(s[6]);
        const double t26 = t25 * double(s[5]);
        s[20] = (float(t26));
        const double t27 = t2 * double(s[4]);
        s[19] = (float(t27));
        const double t28 = double(s[7]);
        const double t29 = t28 * double(s[5]);
        s[18] = (float(t29));
        const double t30 = t1 * double(s[5]);
        s[17] = (float(t30));
        const double t31 = double(s[20]);
        const double t32 = t31 + double(s[21]);
        const double t33 = t32 + double(s[22]);
        const double t34 = t33 * double(s[3]);
        const double t35 = double(s[18]);
        const double t36 = t35 + double(s[19]);
        const double t37 = t36 * double(s[4]);
        const double t38 = t34 + t37;
        const double t39 = double(s[17]);
        const double t40 = t39 * double(s[5]);
        const double t41 = t38 + t40;
        p[0] = (float(t41));
        const double t42 = double(s[24]);
        const double t43 = t42 * double(s[9]);
        s[13] = (float(t43));
        const double t44 = t3 * double(s[8]);
        s[12] = (float(t44));
        const double t45 = double(s[6]);
        const double t46 = t45 * double(s[10]);
        s[11] = (float(t46));
        const double t47 = t2 * double(s[9]);
        const double t48 = double(s[7]);
        const double t49 = t48 * double(s[10]);
        const double t50 = t1 * double(s[10]);
        const double t51 = double(s[11]);
        const double t52 = t51 + double(s[12]);
        const double t53 = t52 + double(s[13]);
        const double t54 = t53 * double(s[8]);
        const double t55 = t49 + t47;
        const double t56 = t55 * double(s[9]);
        const double t57 = t54 + t56;
        const double t58 = t50 * double(s[10]);
        const double t59 = t57 + t58;
        p[4] = (float(t59));
        const double t60 = double(s[24]);
        const double t61 = t60 * double(s[3]);
        const double t62 = t61 * double(s[0]);
        const double t63 = double(s[6]);
        const double t64 = t63 * double(s[3]);
        const double t65 = t64 * double(s[1]);
        const double t66 = t62 + t65;
        const double t67 = double(s[7]);
        const double t68 = t67 * double(s[4]);
        const double t69 = t68 * double(s[1]);
        const double t70 = t66 + t69;
        const double t71 = double(s[17]);
        const double t72 = t71 * double(s[1]);
        const double t73 = t72 + t72;
        const double t74 = t70 + t73;
        const double t75 = double(s[18]);
        const double t76 = t75 * double(s[0]);
        const double t77 = t74 + t76;
        const double t78 = double(s[19]);
        const double t79 = t78 * double(s[0]);
        const double t80 = t79 + t79;
        const double t81 = t77 + t80;
        const double t82 = double(s[20]);
        const double t83 = t82 * double(s[2]);
        const double t84 = t81 + t83;
        const double t85 = double(s[21]);
        const double t86 = t85 * double(s[2]);
        const double t87 = t86 + t86;
        const double t88 = t84 + t87;
        const double t89 = double(s[22]);
        const double t90 = t89 * double(s[2]);
        const double t91 = t88 + t90;
        p[8] = (float(t91));
        const double t92 = double(s[24]);
        const double t93 = t92 * double(s[8]);
        const double t94 = double(s[6]);
        const double t95 = t94 * double(s[8]);
        const double t96 = double(s[7]);
        const double t97 = t96 * double(s[9]);
        s[22] = (float(t97));
        const double t98 = t97 + t95;
        const double t99 = t98 * double(s[1]);
        const double t100 = t93 * double(s[0]);
        const double t101 = t99 + t100;
        const double t102 = t50 * double(s[1]);
        const double t103 = t102 + t102;
        const double t104 = t101 + t103;
        const double t105 = t49 * double(s[0]);
        const double t106 = t104 + t105;
        const double t107 = t47 * double(s[0]);
        const double t108 = t107 + t107;
        const double t109 = t106 + t108;
        const double t110 = double(s[11]);
        const double t111 = t110 * double(s[2]);
        const double t112 = t109 + t111;
        const double t113 = double(s[12]);
        const double t114 = t113 * double(s[2]);
        const double t115 = t114 + t114;
        const double t116 = t112 + t115;
        const double t117 = double(s[13]);
        const double t118 = t117 * double(s[2]);
        const double t119 = t116 + t118;
        p[12] = (float(t119));
        const double t120 = double(s[22]);
        const double t121 = t120 + t95;
        const double t122 = t121 * double(s[5]);
        const double t123 = t93 * double(s[4]);
        const double t124 = t122 + t123;
        const double t125 = t50 * double(s[5]);
        const double t126 = t125 + t125;
        const double t127 = t124 + t126;
        const double t128 = t49 * double(s[4]);
        const double t129 = t127 + t128;
        const double t130 = t47 * double(s[4]);
        const double t131 = t130 + t130;
        const double t132 = t129 + t131;
        const double t133 = double(s[11]);
        const double t134 = t133 * double(s[3]);
        const double t135 = t132 + t134;
        const double t136 = double(s[12]);
        const double t137 = t136 * double(s[3]);
        const double t138 = t137 + t137;
        const double t139 = t135 + t138;
        const double t140 = double(s[13]);
        const double t141 = t140 * double(s[3]);
        const double t142 = t139 + t141;
        p[16] = (float(t142));
        const double t143 = double(s[14]);
        const double t144 = t143 * double(s[2]);
        const double t145 = double(s[15]);
        const double t146 = t145 * double(s[0]);
        const double t147 = t144 + t146;
        const double t148 = double(s[16]);
        const double t149 = t148 * double(s[1]);
        const double t150 = t147 + t149;
        p[20] = (float(t150));
        const double t151 = double(s[14]);
        const double t152 = t151 * double(s[3]);
        const double t153 = double(s[15]);
        const double t154 = t153 * double(s[4]);
        const double t155 = t152 + t154;
        const double t156 = double(s[16]);
        const double t157 = t156 * double(s[5]);
        const double t158 = t155 + t157;
        p[24] = (float(t158));
        const double t159 = double(s[14]);
        const double t160 = t159 * double(s[8]);
        const double t161 = double(s[15]);
        const double t162 = t161 * double(s[9]);
        const double t163 = t160 + t162;
        const double t164 = double(s[16]);
        const double t165 = t164 * double(s[10]);
        const double t166 = t163 + t165;
        p[28] = (float(t166));
    }
}

// ---- elSetupEnvMap (0x542c20) ----

struct EnvExit
{
    bool target;            // the target path ran
    bool lengthNonZero;     // its normalisation had a length
    reg32 zBits;            // the z it normalised
    bool multiplied;
};

// The direction to the target in the matrix's frame (clamped length) at +0xc0 and, with an environment matrix,
// matrix * envMatrix at +0xe0 with that direction as its translation.
EnvExit envMap(Context& c, reg32 target, reg32 matrix, reg32 envMatrix)
{
    EnvExit e = {false, false, 0, false};
    float t[16];
    rotationTranspose(matrix, t);
    if (target != 0)
    {
        e.target = true;
        float v[4], u[4];
        v[0] = float(K(target) - K(matrix + 0x30));
        v[1] = float(K(target + 4) - K(matrix + 0x34));
        v[2] = float(K(target + 8) - K(matrix + 0x38));
        v[3] = bitsFloat(0x3f800000u);
        win32::d3dx9::transform(v, t, u);
        const double z = double(u[2]), y = double(u[1]), x = double(u[0]);
        double d = z * z + y * y;
        d += x * x;
        d += double(u[3]);
        d += double(u[3]);
        float df = float(std::sqrt(d));
        if (double(df) < K(EnvMin))
            df = rc::ldf32(EnvMin);
        else if (double(df) > K(EnvMax))
            df = rc::ldf32(EnvMax);
        u[3] = bitsFloat(0);
        e.zBits = floatBits(u[2]);
        float n[3];
        e.lengthNonZero = normalize4(u, n);
        c.at(0xcc) = bitsFloat(0x3f800000u);
        c.at(0xc8) = float(double(df) * double(n[2]));
        c.at(0xc0) = float(double(df) * double(n[0]));
        c.at(0xc4) = float(double(df) * double(n[1]));
    }
    if (envMatrix != 0 && target != 0)
    {
        e.multiplied = true;
        float a[16], b[16];
        loadMatrix(matrix, a);
        loadMatrix(envMatrix, b);
        win32::d3d9::multiplyFast(a, b, &c.at(0xe0));
        c.at(0xec) = c.at(0xfc) = c.at(0x10c) = bitsFloat(0);
        c.at(0x110) = c.at(0xc0);
        c.at(0x114) = c.at(0xc4);
        c.at(0x118) = c.at(0xc8);
        c.at(0x11c) = bitsFloat(0);
    }
    return e;
}


// ---- elSetupLights (0x542e40) ----

// The table lights, the query lights, the harmonic coefficients, the first three colours and directions and the
// three levels. False when the query returns more than 10 lights. The last float-to-integer conversion goes to *last.
bool setup(Context& c, reg32 position, reg32 matrix, reg32 envMatrix, reg32 view, reg64* last)
{
    Query q;
    query(q, position);
    if (q.count > MaxLights)
        return false;
    const int n = q.count;
    const int m = n + 8;

    reg32 camera = view != 0 ? rc::ld32(view + 0x40) : 0;
    if (camera == 0)
        camera = envMatrix;
    float t[16];
    rotationTranspose(camera, t);

    float dir[Directions][4], nrm[Directions][4], col[Directions][4];
    memset(dir, 0, sizeof(dir));
    memset(nrm, 0, sizeof(nrm));
    memset(col, 0, sizeof(col));

    const reg32 table = (rc::ld32(GameMode) == 3 ? LightTable : LightTableAlt) + 0xc;
    for (int i = 0; i < 7; ++i)
    {
        const reg32 e = table + 12 * reg32(i);
        const float azimuth = float(K(e) + K(OneEighty));
        const float elevation = float(K(Ninety) - K(e + 4));
        float d[3];
        sphere(azimuth, elevation, bitsFloat(0x3f800000u), d);
        const float v[4] = { float(-double(d[1])), float(-double(d[2])), roundTrip(d[0]), bitsFloat(0) };
        float r[4];
        win32::d3dx9::transform(v, t, r);
        dir[i][0] = r[0];
        dir[i][1] = r[1];
        dir[i][2] = r[2];
        const float intensity = bitsFloat(rc::ld32(e + 8));
        col[i][0] = col[i][1] = col[i][2] = intensity;
    }

    const float px = rc::ldf32(position), py = rc::ldf32(position + 4), pz = rc::ldf32(position + 8);
    for (int i = 0; i < n; ++i)
    {
        const reg32 light = q.light[i];
        const double w = double(q.weight[i]);
        float* const d = dir[7 + i];
        float* const k = col[7 + i];
        d[0] = float(K(light + 0x10) - double(px));
        d[1] = float(K(light + 0x14) - double(py));
        d[2] = float(K(light + 0x18) - double(pz));
        const reg32 colour = rc::ld32(light + 0xc);
        const float r = float(colour & 0xff), g = float((colour >> 8) & 0xff), b = float((colour >> 16) & 0xff);
        if (rc::ld8(light + 4) != 3)
        {
            k[0] = float(double(r) * K(Bright) * w);
            k[1] = float(double(g) * K(Bright) * w);
            k[2] = float(double(b) * K(Bright) * w);
            continue;
        }
        const reg32 angle = reg32(x86::truncInteger64(K(light + 0x34) * K(SpotScale) + K(SpotBias)));
        const double cosCut = double(float(mathfn::sin16(angle)));
        float v[3] = { float(double(px) - K(light + 0x10)), float(double(py) - K(light + 0x14)),
                       float(double(pz) - K(light + 0x18)) };
        normalize(v, v);
        const double dot = (double(v[2]) * K(light + 0x28) + double(v[1]) * K(light + 0x24))
                           + double(v[0]) * K(light + 0x20);
        double f = K(Zero);
        if (dot > cosCut)
            f = (dot - cosCut) / (K(One) - cosCut);
        f = f * w;
        k[0] = float(double(r) * f * K(Bright));
        k[1] = float(double(g) * f * K(Bright));
        k[2] = float(f * double(b) * K(Bright));
    }

    if (matrix != 0)
    {
        float r[16];
        rotationTranspose(matrix, r);
        for (int k = 0; k < m; ++k)
        {
            const float v[4] = { dir[k][0], dir[k][1], dir[k][2], bitsFloat(0x3f800000u) };
            win32::d3dx9::transform(v, r, dir[k]);
        }
    }

    for (int k = 0; k < m; ++k)
    {
        const double x = double(dir[k][0]), y = double(dir[k][1]), z = double(dir[k][2]);
        double len = y * y + x * x;
        len += z * z;
        len = std::sqrt(len);
        if (len == K(Zero))
        {
            nrm[k][0] = bitsFloat(0x3f800000u);
            nrm[k][1] = bitsFloat(0);
            nrm[k][2] = bitsFloat(0);
            continue;
        }
        const double inv = K(One) / len;
        nrm[k][0] = float(inv * x);
        nrm[k][1] = float(inv * y);
        nrm[k][2] = float(inv * z);
    }

    double sum = K(Zero);
    for (int k = 0; k < m; ++k)
    {
        float* const e = col[k];
        for (int i = 0; i < 4; ++i)
            e[i] = float(double(e[i]) * K(Inv255));
        const double luma = (double(e[1]) * K(LumaGreen) + double(e[0]) * K(LumaRed)) + double(e[2]) * K(LumaBlue);
        e[3] = float(luma);
        sum = sum + luma;
    }
    const float sumF = float(sum);
    double scale = K(One) - falloff(float(sum * K(Exposure)));
    if (double(sumF) > K(ExposureLimit))
        scale = K(ExposureLimit) / double(sumF);
    for (int k = 0; k < m; ++k)
        for (int i = 0; i < 4; ++i)
            col[k][i] = float(scale * double(col[k][i]));

    float co[40];
    memset(co, 0, sizeof(co));
    for (int k = 0; k < m; ++k)
        band(co, col[k], nrm[k]);
    normaliseCoefficients(co, rc::ld32(NormaliseColour) != 0, rc::ldf32(NormaliseLevel), rc::ld32(NormaliseAlpha) != 0,
                          rc::ldf32(NormaliseAlphaLevel));
    memcpy(&c.at(0x120), co, sizeof(co));

    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
            c.at(0x10 * i + 4 * j) = float(double(col[i][j]) * K(K255));
        c.at(0x40 + 4 * i) = nrm[i][0];
        c.at(0x50 + 4 * i) = nrm[i][1];
        c.at(0x60 + 4 * i) = nrm[i][2];
    }

    double level[3];
    reg64 v = 0;
    for (int j = 0; j < 3; ++j)
    {
        v = reg64(x86::truncInteger64(K(Scale50) * double(c.at(0x120 + 4 * j))));
        level[j] = double(sreg32(reg32(v)));
        if (sreg32(reg32(v)) < 0)
            level[j] += K(TwoPow32);
        c.at(0x30 + 4 * j) = float(level[j]);
    }
    *last = v;
    const float full = bitsFloat(0x437f0000u);
    if (level[0] > K(K255))
        c.at(0x30) = full;
    if (double(float(level[1])) > K(K255))
        c.at(0x34) = full;
    if (double(float(level[2])) > K(K255))
        c.at(0x38) = full;
    return true;
}

// ---- elSetupLightContext (0x5437f0) ----

// The context zeroed, the environment map and the lights. False as setup.
bool fill(Context& c, reg32 matrix, reg32 envMatrix, reg32 target, reg32 view, reg64* last)
{
    memset(c.f, 0, sizeof(c.f));
    envMap(c, target, matrix, envMatrix);
    return setup(c, matrix + 0x30, matrix, envMatrix, view, last);
}

// ---- the context transform (0x542400) ----

// The two contexts overlap without being the same.
inline bool partialOverlap(reg32 a, reg32 b)
{
    return a != b && (a - b < ContextSize || b - a < ContextSize);
}

// d = s (0x542940: the second and third float of each 16-byte row through the x87, the row at 0xd0 as dwords), its
// three directions and its coefficients rotated by the matrix's rotation. s may be d.
void transform(Context& d, const Context& source, bool same, reg32 matrix)
{
    for (reg32 row = 0; row < ContextSize; row += 0x10)
    {
        const float* const from = &source.f[row / 4];
        float* const to = &d.f[row / 4];
        if (row == 0xd0)
        {
            memcpy(to, from, 16);
            continue;
        }
        const float a = from[0], b = from[1], c = from[2], e = from[3];
        to[0] = a;
        to[1] = roundTrip(b);
        to[2] = roundTrip(c);
        to[3] = e;
    }
    const Context& s = same ? d : source;
    float t[16];
    rotationTranspose(matrix, t);
    for (int i = 0; i < 3; ++i)
    {
        const reg32 o = 4 * reg32(i);
        const float v[4] = { s.f[(0x40 + o) / 4], s.f[(0x50 + o) / 4], s.f[(0x60 + o) / 4], bitsFloat(0) };
        float r[4];
        win32::d3dx9::transform(v, t, r);
        d.at(0x40 + o) = r[0];
        d.at(0x50 + o) = r[1];
        d.at(0x60 + o) = r[2];
    }
    float co[40];
    memcpy(co, &s.f[0x120 / 4], sizeof(co));
    rotateCoefficients(co, t);
    memcpy(&d.at(0x120), co, sizeof(co));
}

// ---- guest-convention entries, dispatch and shadow compare ----

enum Id
{
    IdFill, IdSetup, IdEnvMap, IdTransform, IdClone, IdCount
};

const char* const Names[IdCount] = {
    "fill 0x5437f0", "setup 0x542e40", "env map 0x542c20", "transform 0x542400", "clone 0x544390",
};

const reg32 Addresses[IdCount] = { 0x5437f0, 0x542e40, 0x542c20, 0x542400, 0x544390 };

typedef void (*Guest)(win32::WinApplication*, x86::CPU&);
typedef bool (*Native)(x86::CPU&);

struct Stats
{
    uint32_t calls = 0, fallbacks = 0, sampled = 0;
    uint64_t sampledUs = 0;
};
Stats s_stats[IdCount];
uint32_t s_shadowCalls = 0, s_shadowDiffering = 0, s_shadowLogged = 0;
const uint32_t ShadowLogLimit = 20;
const uint32_t SampleMask = 15;     // one call in 16 is timed

// The frame base of 0x542c20 (esp after its prologue) for an entry esp.
inline reg32 envFrame(reg32 entry) { return ((entry - 4) & ~15u) - 0x60; }

// The registers 0x542c20 leaves (ecx and edx when it does not set them stay as the caller left them).
void envExit(x86::CPU& cpu, const EnvExit& e, reg32 entry, reg32 context, const Context& c)
{
    const reg32 frame = envFrame(entry);
    if (!e.target)
    {
        cpu.eax = frame + 0x20;
    }
    else
    {
        cpu.eax = frame + 0x10;
        cpu.ecx = e.lengthNonZero ? e.zBits : frame + 0x10;
        cpu.edx = frame + 0x10;
    }
    if (e.multiplied)
    {
        cpu.eax = context + 0xe0;
        cpu.ecx = floatBits(c.f[0xc8 / 4]);
        cpu.edx = floatBits(c.f[0xc4 / 4]);
    }
}

// 0x5437f0: cdecl (context, matrix, envMatrix, target, view).
bool fillNative(x86::CPU& cpu)
{
    const reg32 a = cpu.esp;
    const reg32 context = rc::ld32(a + 4), matrix = rc::ld32(a + 8), envMatrix = rc::ld32(a + 0xc),
                target = rc::ld32(a + 0x10), view = rc::ld32(a + 0x14);
    if (rc::ld32(FillPath) == 0)
        return false;
    if (context == 0 || matrix == 0)
    {
        cpu.eax = 0xffffffffu;
        cpu.esp = a + 4;
        return true;
    }
    Context c;
    reg64 last;
    if (!fill(c, matrix, envMatrix, target, view, &last))
        return false;
    c.store(context);
    cpu.eax = reg32(last);
    cpu.edx = reg32(last >> 32);
    cpu.ecx = 0x437f0000u;
    cpu.esp = a + 4;
    return true;
}

// 0x542e40: cdecl (context, position, matrix, envMatrix), eax = view.
bool setupNative(x86::CPU& cpu)
{
    const reg32 a = cpu.esp;
    const reg32 context = rc::ld32(a + 4), position = rc::ld32(a + 8), matrix = rc::ld32(a + 0xc),
                envMatrix = rc::ld32(a + 0x10);
    if (context == 0)
    {
        cpu.ecx = 0;
        cpu.esp = a + 4;
        return true;
    }
    Context c;
    c.load(context);
    reg64 last;
    if (!setup(c, position, matrix, envMatrix, cpu.eax, &last))
        return false;
    c.store(context);
    cpu.eax = reg32(last);
    cpu.edx = reg32(last >> 32);
    cpu.ecx = 0x437f0000u;
    cpu.esp = a + 4;
    return true;
}

// 0x542c20: eax = target, edi = context, cdecl (matrix, envMatrix).
bool envMapNative(x86::CPU& cpu)
{
    const reg32 a = cpu.esp;
    const reg32 context = cpu.edi, matrix = rc::ld32(a + 4), envMatrix = rc::ld32(a + 8);
    Context c;
    c.load(context);
    const EnvExit e = envMap(c, cpu.eax, matrix, envMatrix);
    c.store(context);
    envExit(cpu, e, a, context, c);
    cpu.esp = a + 4;
    return true;
}

// 0x542400: ecx = destination, eax = source, cdecl (matrix).
bool transformNative(x86::CPU& cpu)
{
    const reg32 a = cpu.esp;
    const reg32 destination = cpu.ecx, source = cpu.eax, matrix = rc::ld32(a + 4);
    if (partialOverlap(destination, source))
        return false;
    Context s, d;
    s.load(source);
    transform(d, s, destination == source, matrix);
    d.store(destination);
    cpu.ecx = floatBits(d.f[0x1bc / 4]);
    cpu.edx = floatBits(d.f[0x1b4 / 4]);
    cpu.eax = ((a - 4) & ~15u) - 0x120 + 0xe0;
    cpu.esp = a + 4;
    return true;
}

// 0x544390: eax = destination, esi = matrix, cdecl (envMatrix, target, view, source).
bool cloneNative(x86::CPU& cpu)
{
    const reg32 a = cpu.esp;
    const reg32 destination = cpu.eax, matrix = cpu.esi, envMatrix = rc::ld32(a + 4), target = rc::ld32(a + 8),
                view = rc::ld32(a + 0xc), source = rc::ld32(a + 0x10);
    if (rc::ld32(CloneFill) != 0)
    {
        if (rc::ld32(FillPath) == 0)
            return false;
        if (destination != 0 && matrix != 0)
        {
            Context c;
            reg64 last;
            if (!fill(c, matrix, envMatrix, target, view, &last))
                return false;
            c.store(destination);
            cpu.eax = reg32(last);
            cpu.edx = reg32(last >> 32);
            cpu.ecx = 0x437f0000u;
        }
        cpu.esp = a + 4;
        return true;
    }
    if (partialOverlap(destination, source))
        return false;
    Context s, d;
    s.load(source);
    transform(d, s, destination == source, matrix);
    cpu.ecx = floatBits(d.f[0x1bc / 4]);
    cpu.edx = floatBits(d.f[0x1b4 / 4]);
    const EnvExit e = envMap(d, target, matrix, envMatrix);
    d.store(destination);
    envExit(cpu, e, a - 0x1c, destination, d);
    cpu.esp = a + 4;
    return true;
}

bool sameCpu(Id id, const x86::CPU& g, const x86::CPU& n, char* text, size_t size)
{
    if (g.fpu.count != n.fpu.count)
    {
        snprintf(text, size, "x87 depth guest %u native %u", unsigned(g.fpu.count), unsigned(n.fpu.count));
        return false;
    }
    return retstate::sameRegisters(retstate::registers(Addresses[id]), g, n, text, size)
           && retstate::sameFlags(retstate::flags(Addresses[id]), g, n, text, size);
}

void shadow(Id id, win32::WinApplication* app, x86::CPU& cpu, Native native, Guest guest, reg32 context)
{
    reg8 before[ContextSize], after[ContextSize];
    if (context != 0)
        memcpy(before, rc::host(context), ContextSize);
    x86::CPU n = cpu;
    if (!native(n))
    {
        ++s_stats[id].fallbacks;
        guest(app, cpu);
        return;
    }
    if (context != 0)
    {
        memcpy(after, rc::host(context), ContextSize);
        memcpy(rc::host(context), before, ContextSize);
    }
    guest(app, cpu);
    ++s_shadowCalls;
    char text[160];
    bool same = sameCpu(id, cpu, n, text, sizeof(text));
    if (same && context != 0)
    {
        for (reg32 k = 0; k < ContextSize; k += 4)
        {
            reg32 g, v;
            memcpy(&g, rc::host(context + k), 4);
            memcpy(&v, after + k, 4);
            if (g != v)
            {
                snprintf(text, sizeof(text), "context +0x%03x guest 0x%08x native 0x%08x", unsigned(k), unsigned(g),
                         unsigned(v));
                same = false;
                break;
            }
        }
    }
    if (!same)
    {
        ++s_shadowDiffering;
        if (s_shadowLogged < ShadowLogLimit)
        {
            ++s_shadowLogged;
            pf::logInfo("shadow_lights: %s call %u: %s", Names[id], unsigned(s_shadowCalls), text);
        }
    }
}

void run(Id id, win32::WinApplication* app, x86::CPU& cpu, Native native, Guest guest, reg32 context)
{
    const Settings& s = settings();
    if (!s.native)
    {
        guest(app, cpu);
        return;
    }
    Stats& st = s_stats[id];
    ++st.calls;
    if (__builtin_expect(s.shadow, 0))
    {
        shadow(id, app, cpu, native, guest, context);
        return;
    }
    const bool timed = __builtin_expect(pf::perf::enabled(), 0) && (st.calls & SampleMask) == 0;
    const uint32_t start = timed ? pf::perf::now() : 0;
    if (!native(cpu))
    {
        ++st.fallbacks;
        guest(app, cpu);
        return;
    }
    if (timed)
    {
        st.sampledUs += pf::perf::now() - start;
        ++st.sampled;
    }
}

}

bool fillContext(reg32 context, reg32 matrix, reg32 envMatrix, reg32 target, reg32 view)
{
    if (!settings().native || rc::ld32(FillPath) == 0)
        return false;
    if (context == 0 || matrix == 0)
        return true;
    Context c;
    reg64 last;
    if (!fill(c, matrix, envMatrix, target, view, &last))
        return false;
    c.store(context);
    return true;
}

bool setupLights(reg32 context, reg32 position, reg32 matrix, reg32 envMatrix, reg32 view)
{
    if (!settings().native)
        return false;
    if (context == 0)
        return true;
    Context c;
    c.load(context);
    reg64 last;
    if (!setup(c, position, matrix, envMatrix, view, &last))
        return false;
    c.store(context);
    return true;
}

void setupEnvMap(reg32 context, reg32 target, reg32 matrix, reg32 envMatrix)
{
    Context c;
    c.load(context);
    envMap(c, target, matrix, envMatrix);
    c.store(context);
}

void transformContext(reg32 destination, reg32 source, reg32 matrix)
{
    Context s, d;
    s.load(source);
    transform(d, s, destination == source, matrix);
    d.store(destination);
}

bool cloneContext(reg32 destination, reg32 matrix, reg32 envMatrix, reg32 target, reg32 view, reg32 source)
{
    if (rc::ld32(CloneFill) != 0)
        return fillContext(destination, matrix, envMatrix, target, view);
    if (!settings().native || partialOverlap(destination, source))
        return false;
    Context s, d;
    s.load(source);
    transform(d, s, destination == source, matrix);
    envMap(d, target, matrix, envMatrix);
    d.store(destination);
    return true;
}

void fillEntry(win32::WinApplication* app, x86::CPU& cpu)
{
    run(IdFill, app, cpu, &fillNative, &sub_5437f0_guest, rc::ld32(cpu.esp + 4));
}

void cloneEntry(win32::WinApplication* app, x86::CPU& cpu)
{
    run(IdClone, app, cpu, &cloneNative, &sub_544390_guest, cpu.eax);
}

void report(uint32_t frames)
{
    if (!settings().native || frames == 0)
        return;
    char text[400];
    int used = 0;
    for (int id = 0; id < IdCount; ++id)
    {
        const Stats& st = s_stats[id];
        if (st.calls == 0)
            continue;
        const double us = st.sampled ? double(st.sampledUs) / st.sampled : 0.0;
        used += snprintf(text + used, sizeof(text) - size_t(used), "%s%s %u/frame (%.1f us, %u guest)",
                         used ? ", " : "", Names[id], unsigned(st.calls / frames), us, unsigned(st.fallbacks));
        if (used >= int(sizeof(text)))
            break;
    }
    if (used > 0)
        pf::logInfo("lights: %s", text);
    for (Stats& st : s_stats)
        st = Stats();
}

void logShadowTotal()
{
    if (settings().native && settings().shadow)
        pf::logInfo("shadow_lights: %u calls compared, %u differ", unsigned(s_shadowCalls),
                    unsigned(s_shadowDiffering));
}

}

namespace native
{

void sub_542e40(win32::WinApplication* app, x86::CPU& cpu)
{
    lights::run(lights::IdSetup, app, cpu, &lights::setupNative, &sub_542e40_guest, rc::ld32(cpu.esp + 4));
}

void sub_542c20(win32::WinApplication* app, x86::CPU& cpu)
{
    lights::run(lights::IdEnvMap, app, cpu, &lights::envMapNative, &sub_542c20_guest, cpu.edi);
}

void sub_542400(win32::WinApplication* app, x86::CPU& cpu)
{
    lights::run(lights::IdTransform, app, cpu, &lights::transformNative, &sub_542400_guest, cpu.ecx);
}

}
}
