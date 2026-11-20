// Native character animation (anim.h): the bone matrices of a pose (0x628B30 with 0x627D80) and the keyframe
// decoder of codec 7 (0x6315F0). Each override leaves the registers, x87 state and guest memory the translated
// code leaves.

#include "anim.h"
#include "retstate.h"

#include <lib/winapp.h>
#include <platform/config.h>
#include <platform/log.h>
#include <recomp.h>
#include <winapi/d3d9_internal.h>

#include <cmath>
#include <cstdio>
#include <fpu.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace nfsu
{

// The translated guest bodies of the overridden functions (generated).
void sub_628b30_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_6315f0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace anim
{

namespace
{

using x86::reg8;
using x86::reg32;
using x86::sreg32;

const reg32 One = 0x6cc7bc;                 // 1.0f
const reg32 ParentMultiply = 0x6e58dc;      // matrix product of a bone and its parent
const reg32 MatrixMultiply = 0x634160;      // D3DXMatrixMultiply(out, a, b) as cdecl

// Skeleton: the bone count at +8, bones of 0x70 bytes from +0x1c with the parent index first (-1: none).
const reg32 SkeletonBones = 0x8;
const reg32 SkeletonParents = 0x1c;
const reg32 BoneSize = 0x70;
// Pose record of a bone, 0x30 bytes: scale x, y, z, uniform scale, rotation quaternion x, y, z, w, translation.
const reg32 PoseSize = 0x30;

struct Settings
{
    bool native;
    bool shadow;
};

const Settings& settings()
{
    static const Settings s = []
    {
        const Settings r = { pf::Config::getBool("anim_native", true), pf::Config::getBool("shadow_anim", false) };
        pf::logInfo("anim: bone matrices and keyframe decoder %s%s", r.native ? "native" : "guest",
                    r.native && r.shadow ? ", shadow compare" : "");
        return r;
    }();
    return s;
}

const uint32_t ShadowLogLimit = 20;
// Shadow compare counts per function: 0 the bone matrices, 1 the keyframe decoder.
struct ShadowCounts
{
    uint32_t calls = 0, differing = 0, logged = 0;
};
ShadowCounts s_shadow[2];

inline double ld(reg32 address) { return double(rc::ldf32(address)); }

// 0x627D80: the matrix of a pose record (row-major, translation in the last row).
void boneMatrix(reg32 pose, float* m)
{
    const double sx = ld(pose), sy = ld(pose + 4), sz = ld(pose + 8);
    const double qx = ld(pose + 0x10), qy = ld(pose + 0x14), qz = ld(pose + 0x18), qw = ld(pose + 0x1c);
    const double one = ld(One);
    const double x2 = qx + qx, y2 = qy + qy, z2 = double(float(qz + qz));
    const double wx = float(x2 * qw), wy = float(qw * y2), wz = float(z2 * qw);
    const double xx = float(x2 * qx), xy = float(qx * y2), xz = float(z2 * qx);
    const double yy = y2 * qy, yz = z2 * qy, zz = z2 * qz;
    m[0] = float((one - (zz + yy)) * sx);
    m[4] = float((xy - wz) * sy);
    m[8] = float((xz + wy) * sz);
    m[1] = float((xy + wz) * sx);
    m[5] = float((one - (zz + xx)) * sy);
    m[9] = float((yz - wx) * sz);
    m[2] = float((xz - wy) * sx);
    m[6] = float((yz + wx) * sy);
    m[10] = float((one - (yy + xx)) * sz);
    m[3] = m[7] = m[11] = 0.0f;
    memcpy(&m[12], rc::host(pose + 0x20), 12);
    m[15] = 1.0f;
}

// 0x628B30 (ecx = skeleton; poses, matrices, bone mask or 0; ret 0xc): each bone (in the mask) gets the matrix of
// its pose with the first column scaled by the uniform scale, then that matrix times its parent's matrix.
void poseMatricesNative(x86::CPU& cpu)
{
    const reg32 skeleton = cpu.ecx, entry = cpu.esp;
    const reg32 poses = rc::ld32(entry + 4), matrices = rc::ld32(entry + 8), mask = rc::ld32(entry + 0xc);
    const sreg32 count = sreg32(rc::ld32(skeleton + SkeletonBones));
    reg32 eax = reg32(count), edx = cpu.edx;
    for (sreg32 i = 0; i < count; ++i)
    {
        if (mask)
        {
            edx = mask;
            if (!(rc::ld32(mask + 4 * (reg32(i) >> 5)) & (reg32(1) << (i & 31))))
                continue;
        }
        const reg32 pose = poses + reg32(i) * PoseSize, out = matrices + reg32(i) * 64;
        float m[16];
        boneMatrix(pose, m);
        const double uniform = ld(pose + 0xc);
        m[0] = float(double(m[0]) * uniform);
        m[4] = float(double(m[4]) * uniform);
        m[8] = float(double(m[8]) * uniform);
        memcpy(rc::host(out), m, 64);
        edx = rc::ld32(pose + 0x24);
        eax = rc::ld32(pose + 0x28);
        const sreg32 parent = sreg32(rc::ld32(skeleton + SkeletonParents + reg32(i) * BoneSize));
        eax = reg32(parent);
        if (parent >= 0)
        {
            float p[16], r[16];
            memcpy(p, rc::host(matrices + reg32(parent) * 64), 64);
            win32::d3d9::multiplyFast(m, p, r);
            memcpy(rc::host(out), r, 64);
            eax = out;
        }
    }
    if (mask)
    {
        cpu.eax = reg32(count);
        cpu.ecx = reg32(count);
    }
    else
    {
        cpu.eax = count > 0 ? eax : reg32(count);
        cpu.ecx = skeleton;
    }
    cpu.edx = edx;
    cpu.esp = entry + 0x10;
}

// ---- keyframe decoder of codec 7 (0x6315F0) ----

// Decoder: the stream header at +0xc, channel ranges at +0x10 (12 bytes per channel), the block data at +0x14 with
// the block size at +0x18, the decoded frame at +0x1c (-1: none), the channel rotations at +0x24 (16 bytes per
// channel), the extra channels' bones at +0x28 and their key rotations at +0x2c (6 bytes each).
const reg32 DecoderHeader = 0xc, DecoderParams = 0x10, DecoderBlocks = 0x14, DecoderBlockSize = 0x18,
            DecoderFrame = 0x1c, DecoderState = 0x24, DecoderExtraBones = 0x28, DecoderExtraKeys = 0x2c;
// Header: the frame count (16-bit) at +4, the channel count at +6, the extra channel count at +7, the key frame
// times (16-bit) at +8 or 0, the channels' bones at +0xc, the block shift at +0x10.
const reg32 HeaderFrames = 4, HeaderChannels = 6, HeaderExtra = 7, HeaderTimes = 8, HeaderBones = 0xc,
            HeaderShift = 0x10;
const reg32 FrameRounding = 0x6a7eb0;
const reg32 DeltaScaleX = 0x6a8374;
const reg32 DeltaScaleYZ = 0x6a8378;
const reg32 KeyScaleX = 0x6a837c;
const reg32 KeyScale = 0x6a8380;
const reg32 ForwardOnly = 0x6e58d8;
const reg32 Zero = 0x6cc7a4;

inline reg32 ld16(reg32 address) { return reg32(rc::ld16(address)); }
inline reg32 ld8(reg32 address) { return reg32(rc::ld8(address)); }
inline float* floats(reg32 address) { return reinterpret_cast<float*>(rc::host(address)); }

// 0x631210: the w of a unit rotation from x, y, z (negated with sign), or x, y, z scaled to unit length with w 0.
void normalize(float* q, bool sign, double one)
{
    const double x = q[0], y = q[1], z = q[2];
    const double sum = (x * x + y * y) + z * z;
    if (sum > one)
    {
        const double r = one / std::sqrt(sum);
        q[3] = 0.0f;
        q[0] = float(r * x);
        q[1] = float(r * y);
        q[2] = float(r * z);
        return;
    }
    const double w = std::sqrt(one - sum);
    q[3] = float(sign ? -w : w);
}

// 0x631280: a key rotation from three 16-bit values (bit 0 of the first: the sign of w).
void keyRotation(reg32 source, float* q, double one)
{
    const double s = ld(KeyScale) + ld(KeyScale);
    const double sx = ld(KeyScaleX) + ld(KeyScaleX);
    const reg32 first = ld16(source);
    q[0] = float(double(sreg32(first >> 1)) * sx - one);
    q[1] = float(double(sreg32(ld16(source + 2))) * s - one);
    q[2] = float(double(sreg32(ld16(source + 4))) * s - one);
    normalize(q, (first & 1) != 0, one);
}

// 0x631180: a channel's delta offsets and ranges, as floats.
struct Range
{
    float offset[3], range[3];
};

Range channelRange(reg32 source, double one)
{
    const double s = ld(KeyScale) + ld(KeyScale);
    Range r;
    for (reg32 k = 0; k < 3; ++k)
        r.offset[k] = float(double(sreg32(ld16(source + 2 * k))) * s - one);
    for (reg32 k = 0; k < 3; ++k)
        r.range[k] = float(double(sreg32(ld16(source + 6 + 2 * k))) * s);
    return r;
}

// One channel's delta in a frame record (3 bytes; bit 0 of the first: the sign of w): x and y in double, z rounded
// to float.
struct Delta
{
    double x, y;
    float z;
    bool sign;
};

Delta channelDelta(reg32 bytes, const Range& r)
{
    const reg32 b0 = ld8(bytes);
    Delta d;
    d.sign = (b0 & 1) != 0;
    d.x = double(sreg32(b0 >> 1)) * double(r.range[0]) * ld(DeltaScaleX) + double(r.offset[0]);
    d.y = double(sreg32(ld8(bytes + 1))) * double(r.range[1]) * ld(DeltaScaleYZ) + double(r.offset[1]);
    d.z = float(double(sreg32(ld8(bytes + 2))) * double(r.range[2]) * ld(DeltaScaleYZ) + double(r.offset[2]));
    return d;
}

// 0x631410: the normalised linear blend of rotations a and b by t into out.
void blend(const float* a, const float* b, float* out, double t, double one)
{
    const double n0 = (double(b[0]) - double(a[0])) * t + double(a[0]);
    const double n1 = (double(b[1]) - double(a[1])) * t + double(a[1]);
    const double n2 = double(float((double(b[2]) - double(a[2])) * t + double(a[2])));
    const double n3d = (double(b[3]) - double(a[3])) * t + double(a[3]);
    const double n3 = double(float(n3d));
    const double sum = ((n3d * n3 + n2 * n2) + n1 * n1) + n0 * n0;
    const double r = one / std::sqrt(sum);
    const double rf = double(float(r));
    out[0] = float(r * n0);
    out[1] = float(rf * n1);
    out[2] = float(rf * n2);
    out[3] = float(rf * n3);
}

// 0x629350: the frame of a time, rounded with the guest's rounding mode.
sreg32 frameOfTime(const x86::CPU& cpu, float time)
{
    const double t = double(time);
    const float shifted = !(t >= ld(Zero)) ? float(t + ld(FrameRounding)) : float(t - ld(FrameRounding));
    return x86::fistInteger<sreg32>(x86::Float(x86::roundIntegral(double(shifted), cpu.fpu.control.rc)));
}

// The rotation of channel ch's bone in the pose records.
inline reg32 outputAddress(reg32 header, reg32 poses, reg32 ch)
{
    return poses + ld8(rc::ld32(header + HeaderBones) + ch) * PoseSize + 0x10;
}

inline float* outputRotation(reg32 header, reg32 poses, reg32 ch)
{
    return floats(outputAddress(header, poses, ch));
}

// 0x6315F0 without a blend target (ecx = decoder; time, pose records, 0; ret 0xc): decodes the channels' rotations
// at the time into the pose records, from the decoded frame forward or backward inside a block or from the block's
// key, blended toward the next frame.
void decodeNative(x86::CPU& cpu)
{
    const reg32 self = cpu.ecx, entry = cpu.esp;
    const float time = rc::ldf32(entry + 4);
    const reg32 poses = rc::ld32(entry + 8);
    const double one = ld(One);
    const reg32 header = rc::ld32(self + DecoderHeader);
    const reg32 channels = ld8(header + HeaderChannels);
    const reg32 state = rc::ld32(self + DecoderState);
    reg32 eax = ((self + DecoderBlocks) & 0xffffff00u) | 1, ecx = self, edx = cpu.edx;
    if (channels)
    {
        eax = 1;
        edx = channels * 16;
        const sreg32 frameAtTime = frameOfTime(cpu, time);
        const reg32 times = rc::ld32(header + HeaderTimes);
        const sreg32 frames = sreg32(ld16(header + HeaderFrames));
        sreg32 frame;
        if (!times)
            frame = frameAtTime < 0 ? 0 : frameAtTime >= frames ? frames - 1 : frameAtTime;
        else if (frameAtTime < sreg32(ld16(times)))
            frame = 0;
        else
        {
            const sreg32 cached = sreg32(rc::ld32(self + DecoderFrame));
            sreg32 c = cached < 1 ? 0 : cached - 1;
            if (sreg32(ld16(times + 2 * reg32(c))) > frameAtTime)
            {
                while (c > 0 && sreg32(ld16(times + 2 * reg32(c))) > frameAtTime)
                    --c;
            }
            else
            {
                const sreg32 last = frames - 2;
                while (c < last && sreg32(ld16(times + 2 * reg32(c + 1))) <= frameAtTime)
                    ++c;
            }
            frame = c + 1;
        }

        const reg32 shift = ld8(header + HeaderShift);
        const sreg32 mask = sreg32(0x7fffffffu >> (31 - shift));
        const sreg32 block = frame >> shift, within = frame & mask;
        const sreg32 cached = sreg32(rc::ld32(self + DecoderFrame));
        const sreg32 cachedBlock = cached >> shift;
        const reg32 blockBase = rc::ld32(self + DecoderBlockSize) * reg32(block) + rc::ld32(self + DecoderBlocks);
        const reg32 params = rc::ld32(self + DecoderParams);
        const bool reset = frame < cached && ld8(ForwardOnly) == 0;
        sreg32 start;
        if (cached != -1 && block == cachedBlock && within != 0 && !reset)
            start = cached & mask;
        else
        {
            for (reg32 ch = 0; ch < channels; ++ch)
                keyRotation(blockBase + ch * 6, floats(state + ch * 16), one);
            start = 0;
        }
        if (start < within)
        {
            reg32 bytes = blockBase + 3 * channels * reg32(start + 2);
            for (sreg32 f = start; f < within; ++f)
            {
                for (reg32 ch = 0; ch < channels; ++ch, bytes += 3)
                {
                    const Delta d = channelDelta(bytes, channelRange(params + ch * 0xc, one));
                    float* q = floats(state + ch * 16);
                    q[0] = float(d.x + double(q[0]));
                    q[1] = float(d.y + double(q[1]));
                    q[2] = float(double(d.z) + double(q[2]));
                }
            }
        }
        else if (start > within)
        {
            reg32 bytes = blockBase + 3 * channels * reg32(start + 2) - 3;
            for (sreg32 f = start - 1; f >= within; --f)
            {
                for (reg32 n = channels; n-- > 0; bytes -= 3)
                {
                    const Delta d = channelDelta(bytes, channelRange(params + n * 0xc, one));
                    float* q = floats(state + n * 16);
                    q[0] = float(double(q[0]) - d.x);
                    q[1] = float(double(q[1]) - d.y);
                    q[2] = float(double(q[2]) - double(d.z));
                }
            }
        }
        if (start != within)
        {
            const reg32 signs = blockBase + 3 * channels * reg32(within + 1);
            for (reg32 ch = 0; ch < channels; ++ch)
                normalize(floats(state + ch * 16), (ld8(signs + 3 * ch) & 1) != 0, one);
        }
        rc::st32(self + DecoderFrame, reg32(frame));

        bool interpolate = true;
        double alpha = 0.0;
        if (!times)
        {
            const double f = double(frameAtTime);
            if (double(time) == f)
                interpolate = false;
            else
                alpha = double(time) - f;
        }
        else if (frame == 0)
        {
            if (double(time) == ld(Zero))
                interpolate = false;
            else
                alpha = double(time) / double(sreg32(ld16(times)));
        }
        else
        {
            const double k0 = double(sreg32(ld16(times + 2 * reg32(frame - 1))));
            if (double(time) == k0)
                interpolate = false;
            else
            {
                const double k1 = double(sreg32(ld16(times + 2 * reg32(frame))));
                alpha = (double(time) - k0) / (k1 - k0);
            }
        }
        if (interpolate && frame >= frames - 1)
            interpolate = false;
        if (!interpolate)
        {
            for (reg32 ch = 0; ch < channels; ++ch)
                memcpy(outputRotation(header, poses, ch), floats(state + ch * 16), 16);
            ecx = channels;
        }
        else
        {
            const double t = double(float(alpha));
            const sreg32 nextBlock = (frame + 1) >> shift;
            if (nextBlock != block)
            {
                const reg32 nextBase =
                    rc::ld32(self + DecoderBlockSize) * reg32(nextBlock) + rc::ld32(self + DecoderBlocks);
                for (reg32 ch = 0; ch < channels; ++ch)
                {
                    float next[4];
                    keyRotation(nextBase + ch * 6, next, one);
                    blend(floats(state + ch * 16), next, outputRotation(header, poses, ch), t, one);
                }
                ecx = outputAddress(header, poses, channels - 1);
            }
            else
            {
                const reg32 bytes = blockBase + 3 * channels * reg32(within + 2);
                for (reg32 ch = 0; ch < channels; ++ch)
                {
                    const Delta d = channelDelta(bytes + 3 * ch, channelRange(params + ch * 0xc, one));
                    const float* q = floats(state + ch * 16);
                    float next[4];
                    next[0] = float(d.x + double(q[0]));
                    next[1] = float(d.y + double(q[1]));
                    const double n2d = double(d.z) + double(q[2]);
                    next[2] = float(n2d);
                    const double sum = (n2d * double(next[2]) + double(next[1]) * double(next[1]))
                                     + double(next[0]) * double(next[0]);
                    if (sum > one)
                    {
                        const double r = one / std::sqrt(sum);
                        next[3] = 0.0f;
                        next[0] = float(double(next[0]) * r);
                        next[1] = float(double(next[1]) * r);
                        next[2] = float(r * double(next[2]));
                    }
                    else
                    {
                        const double w = std::sqrt(one - sum);
                        next[3] = float(d.sign ? -w : w);
                    }
                    blend(q, next, outputRotation(header, poses, ch), t, one);
                }
                ecx = bytes + 3 * channels;
            }
        }
    }
    const reg32 extra = ld8(header + HeaderExtra);
    for (reg32 i = 0; i < extra; ++i)
    {
        const reg32 bone = ld8(rc::ld32(self + DecoderExtraBones) + i);
        const reg32 source = rc::ld32(self + DecoderExtraKeys) + i * 6;
        keyRotation(source, floats(poses + bone * PoseSize + 0x10), one);
        eax = 1;
        ecx = poses + bone * PoseSize + 0x10;
        edx = ld16(source + 4);
    }
    cpu.eax = eax;
    cpu.ecx = ecx;
    cpu.edx = edx;
    cpu.esp = entry + 0x10;
}

bool sameCpu(reg32 function, const x86::CPU& g, const x86::CPU& n, char* text, size_t size)
{
    if (g.esp != n.esp)
    {
        snprintf(text, size, "esp guest 0x%08x native 0x%08x", unsigned(g.esp), unsigned(n.esp));
        return false;
    }
    if (g.fpu.count != n.fpu.count)
    {
        snprintf(text, size, "x87 depth guest %u native %u", unsigned(g.fpu.count), unsigned(n.fpu.count));
        return false;
    }
    return retstate::sameRegisters(retstate::registers(function), g, n, text, size)
        && retstate::sameFlags(retstate::flags(function), g, n, text, size);
}

void reportShadow(int id, const char* name, reg32 caller, bool same, const char* text)
{
    ShadowCounts& c = s_shadow[id];
    ++c.calls;
    if (!same)
    {
        ++c.differing;
        if (c.logged < ShadowLogLimit)
        {
            ++c.logged;
            pf::logInfo("shadow_anim: %s (ip 0x%08x): %s", name, unsigned(caller), text);
        }
    }
    if ((c.calls & 1023) == 0)
        pf::logInfo("shadow_anim: %s: %u calls compared, %u differ", name, unsigned(c.calls), unsigned(c.differing));
}

struct Span
{
    reg32 address, size;
};

// Runs the native code on a copy of the CPU, then the guest code on the restored memory, and compares the guest
// memory of the spans and the return state.
void shadow(int id, const char* name, reg32 function, win32::WinApplication* app, x86::CPU& cpu,
            void (*native)(x86::CPU&), void (*guest)(win32::WinApplication*, x86::CPU&), const std::vector<Span>& spans)
{
    static std::vector<reg8> before, after;
    before.clear();
    for (const Span& span : spans)
        before.insert(before.end(), rc::host(span.address), rc::host(span.address) + span.size);
    x86::CPU n = cpu;
    native(n);
    after.clear();
    for (const Span& span : spans)
        after.insert(after.end(), rc::host(span.address), rc::host(span.address) + span.size);
    size_t at = 0;
    for (const Span& span : spans)
    {
        memcpy(rc::host(span.address), before.data() + at, span.size);
        at += span.size;
    }
    const reg32 caller = cpu.ip;
    guest(app, cpu);
    char text[160] = {};
    bool same = sameCpu(function, cpu, n, text, sizeof(text));
    at = 0;
    for (const Span& span : spans)
    {
        for (reg32 k = 0; same && k + 4 <= span.size; k += 4)
        {
            reg32 v;
            memcpy(&v, &after[at + k], 4);
            const reg32 g = rc::ld32(span.address + k);
            if (g != v)
            {
                snprintf(text, sizeof(text), "dword at 0x%08x guest 0x%08x native 0x%08x",
                         unsigned(span.address + k), unsigned(g), unsigned(v));
                same = false;
            }
        }
        at += span.size;
    }
    reportShadow(id, name, caller, same, text);
}

}

}

namespace native
{

void sub_628b30(win32::WinApplication* app, x86::CPU& cpu)
{
    const anim::Settings& s = anim::settings();
    if (!s.native || rc::ld32(anim::ParentMultiply) != anim::MatrixMultiply)
    {
        sub_628b30_guest(app, cpu);
        return;
    }
    if (__builtin_expect(s.shadow, 0))
    {
        const x86::sreg32 count = x86::sreg32(rc::ld32(cpu.ecx + anim::SkeletonBones));
        const x86::reg32 matrices = rc::ld32(cpu.esp + 8);
        anim::shadow(0, "bone matrices 0x628b30", 0x628b30, app, cpu, &anim::poseMatricesNative, &sub_628b30_guest,
                     {{matrices, count > 0 ? x86::reg32(count) * 64 : 0}});
        return;
    }
    anim::poseMatricesNative(cpu);
}


void sub_6315f0(win32::WinApplication* app, x86::CPU& cpu)
{
    using namespace anim;
    const Settings& s = settings();
    const x86::reg32 self = cpu.ecx;
    if (!s.native || rc::ld32(cpu.esp + 0xc) != 0 || rc::ld32(self + DecoderBlocks) == 0)
    {
        sub_6315f0_guest(app, cpu);
        return;
    }
    if (__builtin_expect(s.shadow, 0))
    {
        const x86::reg32 header = rc::ld32(self + DecoderHeader);
        const x86::reg32 channels = ld8(header + HeaderChannels), extra = ld8(header + HeaderExtra);
        x86::reg32 bones = 0;
        for (x86::reg32 ch = 0; ch < channels; ++ch)
            bones = std::max(bones, ld8(rc::ld32(header + HeaderBones) + ch) + 1);
        for (x86::reg32 i = 0; i < extra; ++i)
            bones = std::max(bones, ld8(rc::ld32(self + DecoderExtraBones) + i) + 1);
        const std::vector<Span> spans = {
            {self + DecoderFrame, 4},
            {rc::ld32(self + DecoderState), channels * 16},
            {rc::ld32(cpu.esp + 8), bones * PoseSize},
        };
        shadow(1, "keyframe decoder 0x6315f0", 0x6315f0, app, cpu, &decodeNative, &sub_6315f0_guest, spans);
        return;
    }
    decodeNative(cpu);
}

}

}
