// Native scenery culler (guest 0x446620 and its callees 0x4465b0, 0x446400, 0x446fd0, 0x5679b0, 0x446140,
// 0x445ec0, 0x445520, 0x445e10, 0x445580, 0x401050, 0x40db20, 0x40da30).
//
// Bit-exact with the translated guest code: every x87 chain is evaluated in double in the guest's order and
// rounded to float where the guest stores a dword, stack temporaries included. Three phases:
//   A  game thread: the section queries of every view, their guest stores kept in an overlay;
//   B  job workers: per (view, section) item, the tree walk, frustum tests, LOD and model choice, into per-thread
//      candidate lists;
//   C  game thread: the overlay stores, then per view in the guest's order the list cursors, frame allocations,
//      matrices and list entries.
// Phase B reads guest memory only. Cases the guest handles by overrunning its stack arrays (more than 64 pending
// tree nodes, more than 0x80 sections), and a full candidate arena, are not reproduced: the guest code then runs
// instead, before any store it would not repeat. The returned registers, x87 status, the x87 slot above the stack
// and the flags the guest stores at calls are reproduced too.

#include "cull.h"

#include <platform/config.h>
#include <platform/jobs.h>
#include <platform/log.h>
#include <platform/perf.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fpu.h>
#include <recomp.h>
#include <vector>

namespace nfsu
{

// The translated guest bodies of the overridden functions (generated).
void sub_446620_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_40db20_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace cull
{

namespace
{

using x86::reg8;
using x86::reg16;
using x86::reg32;
using x86::sreg16;
using x86::sreg32;
using x86::sreg64;

// Guest data.
const reg32 SkipCulling = 0x71db20;     // byte: culling is off
const reg32 ListStart = 0x71db28;       // the list of visible models, shared by all views
const reg32 ListEnd = 0x72a008;
const reg32 SectionTable = 0x72a008;    // section pointers by section number
const reg32 RegionList = 0x77a930;      // regions (polygon, section numbers), sentinel node at +0x10
const reg32 DefaultSections = 0x77a978; // sentinel of the section list used outside every region
const reg32 DetailLevel = 0x736128;
const reg32 FrameCursor = 0x7355b4;     // per-frame allocator: cursor, end, overflow flag, bytes refused
const reg32 FrameEnd = 0x7355b8;
const reg32 FrameOverflow = 0x7355c4;
const reg32 FrameRefused = 0x7355c8;
const reg32 HalfConstant = 0x6cc7dc;    // float constants in .rdata
const reg32 ZeroConstant = 0x6cc7a4;
const reg32 FourConstant = 0x6b780c;
const reg32 TwoConstant = 0x6b7810;
const reg32 ScaleConstant = 0x6cc994;   // int16 matrix entries to floats

const reg32 InfoSize = 0xd0;            // one view's record in the culler object
const int MaxSections = 0x80;           // section numbers per view
const int TreeStack = 64;               // the guest tree walk's stack entries
const int BatchItems = 4;               // (view, section) items per claim
const size_t InitialCandidates = 4096;
const size_t InitialMatrices = 1024;
const uint32_t ReportCalls = 300;

inline double ld(reg32 address) { return double(rc::ldf32(address)); }
inline sreg32 ld16s(reg32 address) { return sreg32(sreg16(rc::ld16(address))); }
inline reg32 bits(float value) { reg32 r; memcpy(&r, &value, 4); return r; }

struct Settings
{
    bool native;
    bool parallel;
    bool shadow;
};

const Settings& settings()
{
    static const Settings s = []
    {
        const Settings r = { pf::Config::getBool("cull_native", true), pf::Config::getBool("cull_parallel", true),
                             pf::Config::getBool("shadow_cull", false) };
        pf::logInfo("cull: scenery culler %s%s%s", r.native ? "native" : "guest",
                    r.native && r.parallel ? ", parallel" : "", r.native && r.shadow ? ", shadow compare" : "");
        return r;
    }();
    return s;
}

// The x87 state guest code leaves behind: the operands of its last compare (status C0, C2, C3) and the last value
// it passed to _ftol2 (kept in the register slot above the stack).
struct X87
{
    bool compared = false;
    bool converted = false;
    double a = 0, b = 0;
    double value = 0;

    void compare(double x, double y) { compared = true; a = x; b = y; }
    void convert(double x) { converted = true; value = x; }
    void follow(const X87& later)
    {
        if (later.compared)
        {
            compared = true;
            a = later.a;
            b = later.b;
        }
        if (later.converted)
        {
            converted = true;
            value = later.value;
        }
    }
    void apply(x86::CPU& cpu) const
    {
        if (compared)
            cpu.fpu.compare(a, b);
        if (converted)
            cpu.fpu.regs[(cpu.fpu.count + 1) & 7] = value;
    }
};

// Box (the guest's float temporaries) against the six planes (float4 normal and distance) at [view] + 0xc0:
// 0 outside, 1 partial, 2 inside. ecx and edx get the guest's registers at the return.
int visible(const float mn[3], const float mx[3], reg32 view, double half, double zero, X87& x87, reg32& ecx,
            reg32& edx)
{
    const float c0 = float((double(mx[0]) + double(mn[0])) * half);
    const float c1 = float((double(mx[1]) + double(mn[1])) * half);
    const float c2 = float((double(mx[2]) + double(mn[2])) * half);
    const float e0 = float(double(mx[0]) - double(c0));
    const float e1 = float(double(mx[1]) - double(c1));
    const float e2 = float(double(mx[2]) - double(c2));
    reg32 plane = rc::ld32(view) + 0xc4;
    bool partial = false;
    for (reg32 n = 1; n <= 6; ++n, plane += 0x10)
    {
        const double nx = ld(plane - 4), ny = ld(plane), nz = ld(plane + 4);
        double r = std::fabs(nz) * double(e2);
        r += std::fabs(nx) * double(e0);
        r += std::fabs(ny) * double(e1);
        double d = double(c2) * nz;
        d += double(c0) * nx;
        d += double(c1) * ny;
        d += ld(plane + 8);
        const double outer = d + r;
        x87.compare(outer, zero);
        if (outer < zero)
        {
            ecx = plane;
            edx = n;
            return 0;
        }
        const double inner = d - r;
        x87.compare(inner, zero);
        if (inner < zero)
            partial = true;
    }
    ecx = plane;
    edx = 7;
    return partial ? 1 : 2;
}

// One coordinate of a transformed corner pair: the smaller term goes to lo, the larger to hi (the larger one is
// the stored float when the terms compare unordered or equal).
inline void bound(double a, float b, double& lo, double& hi)
{
    if (a < double(b))
    {
        lo += a;
        hi += double(b);
    }
    else
    {
        lo += double(b);
        hi += a;
    }
}

// The box bounded again after the matrix at m (rows of float4, translation at +0x30), guest 0x40da30.
void transformBox(float mn[3], float mx[3], reg32 m)
{
    double maxX = ld(m + 0x30), maxY = ld(m + 0x34), maxZ = ld(m + 0x38);
    double minX = maxX, minY = maxY, minZ = maxZ;
    for (reg32 i = 0; i < 3; ++i)
    {
        const reg32 row = m + 0x10 * i;
        bound(double(mn[i]) * ld(row), float(double(mx[i]) * ld(row)), minX, maxX);
        bound(double(mn[i]) * ld(row + 4), float(double(mx[i]) * ld(row + 4)), minY, maxY);
        bound(double(mn[i]) * ld(row + 8), float(double(mx[i]) * ld(row + 8)), minZ, maxZ);
    }
    mn[0] = float(minX);
    mn[1] = float(minY);
    mn[2] = float(minZ);
    mx[0] = float(maxX);
    mx[1] = float(maxY);
    mx[2] = float(maxZ);
}

// int16 box at p: min at +0, max at +6.
inline void shortBox(reg32 p, float mn[3], float mx[3])
{
    for (reg32 i = 0; i < 3; ++i)
    {
        mn[i] = float(double(ld16s(p + 2 * i)));
        mx[i] = float(double(ld16s(p + 6 + 2 * i)));
    }
}

// Read-only inputs of the parallel phase.
struct Pass
{
    double half, zero, four, two, scale;
    reg32 detail;
};

// Projected size of a sphere at pos for a view (guest 0x445e10); 0 behind the view.
sreg32 pixelSize(reg32 pos, reg32 info, float radius, X87& x87)
{
    const double dx = ld(pos) - ld(info + 0xa0);
    const double dy = ld(pos + 4) - ld(info + 0xa4);
    const double dz = ld(pos + 8) - ld(info + 0xa8);
    double depth = dz * ld(info + 0xb8);
    depth += dy * ld(info + 0xb4);
    depth += dx * ld(info + 0xb0);
    const double back = -double(radius);
    x87.compare(back, depth);
    if (back > depth)
        return 0;
    double square = dx * dx;
    square += dz * dz;
    square += dy * dy;
    const float distance = float(std::sqrt(square) - double(radius));
    x87.compare(double(distance), double(radius));
    double value = ld(info + 0xc0);
    if (double(distance) > double(radius))
        value = value / double(distance) * double(radius);
    // _ftol2: truncation, the low dword.
    x87.convert(value);
    return sreg32(x86::fistInteger<sreg64>(std::trunc(value)));
}

struct Candidate
{
    reg32 instance;
    reg32 entry;        // model pointer plus the visibility state
    sreg32 matrix;      // index into the arena's matrices; -1 for an instance without a matrix
};

struct Matrix
{
    reg32 words[16];
};

// Results of one thread in phase B, with a capacity fixed during the phase.
struct Arena
{
    std::vector<Candidate> candidates;
    std::vector<Matrix> matrices;
};

// The matrix of an instance that is not axis aligned (guest 0x445580 and the stores after it in 0x445ec0).
void makeMatrix(const Pass& p, reg32 instance, reg32 flags, reg32 mode, Matrix& m)
{
    const auto entry = [&](reg32 offset) { return float(double(ld16s(instance + offset)) * p.scale); };
    m.words[0] = bits(entry(0x1c));
    m.words[1] = bits(entry(0x1e));
    m.words[2] = bits(entry(0x20));
    m.words[3] = 0;
    m.words[4] = bits(entry(0x22));
    m.words[5] = bits(entry(0x24));
    m.words[6] = bits(entry(0x26));
    m.words[7] = 0;
    m.words[8] = bits(entry(0x28));
    m.words[9] = bits(entry(0x2a));
    float zz = entry(0x2c);
    m.words[11] = 0;
    m.words[12] = rc::ld32(instance + 0x10);
    m.words[13] = bits(float(ld(instance + 0x14)));
    float z = float(ld(instance + 0x18));
    m.words[15] = 0x3f800000;
    if ((flags & mode) & 0x100)
        z = float(p.two + double(z));
    m.words[14] = bits(z);
    if (mode & 0x800)
        zz = float(-double(zz));
    m.words[10] = bits(zz);
}

// One instance of a section for a view (guest 0x445ec0): the model to draw, if any. False when the arena is full.
bool drawScenery(const Pass& p, Arena& out, X87& x87, reg32 section, reg32 info, reg32 index, reg32 state)
{
    const reg32 instance = rc::ld32(section + 0x20) + index * 48;
    const reg32 flags = rc::ld16(instance + 0xe);
    if (rc::ld8(info + 0x84) & reg8(flags ^ 0x60))
        return true;
    const reg32 desc = rc::ld32(section + 0x18) + reg32(ld16s(instance + 0xc)) * 72;
    if (state == 1)
    {
        float mn[3], mx[3];
        shortBox(instance, mn, mx);
        reg32 ecx, edx;
        state = reg32(visible(mn, mx, rc::ld32(info + 0x80), p.half, p.zero, x87, ecx, edx));
        if (state == 0)
            return true;
    }
    const float radius = float(p.four + ld(desc + 0x44));
    const sreg32 size = pixelSize(instance + 0x10, info, radius, x87);
    if (size <= 1)
        return true;
    const reg32 mode = rc::ld32(info + 0x84);
    reg32 model = 0;
    sreg32 slot = -1;
    if (mode & 0x800)
    {
        if (size >= ld16s(desc + 0x20) && rc::ld32(desc + 0x34))
        {
            model = rc::ld32(desc + 0x34);
            slot = 4;
        }
    }
    else if (mode & 0x1000)
    {
        if (size >= ld16s(desc + 0x22) && rc::ld32(desc + 0x38))
        {
            model = rc::ld32(desc + 0x38);
            slot = 5;
        }
    }
    else
    {
        for (reg32 k = 0; k < 4; ++k)
        {
            if (size >= ld16s(desc + 0x18 + 2 * k) && rc::ld32(desc + 0x24 + 4 * k))
            {
                model = rc::ld32(desc + 0x24 + 4 * k);
                slot = sreg32(k);
                break;
            }
        }
    }
    if (p.detail == 2)
    {
        if (slot == 0)
        {
            for (reg32 k = 1; k <= 3; ++k)
            {
                if (rc::ld32(desc + 0x24 + 4 * k))
                {
                    model = rc::ld32(desc + 0x24 + 4 * k);
                    break;
                }
            }
        }
    }
    else if (p.detail == 1)
    {
        if (rc::ld32(desc + 0x38))
            model = rc::ld32(desc + 0x38);
        else
        {
            for (sreg32 k = 3; k >= 0; --k)
            {
                if (rc::ld32(desc + 0x24 + 4 * reg32(k)))
                {
                    model = rc::ld32(desc + 0x24 + 4 * reg32(k));
                    break;
                }
            }
        }
    }
    if (!model)
        return true;
    if (out.candidates.size() == out.candidates.capacity())
        return false;
    Candidate c = { instance, model + state, -1 };
    if (!(flags & 0x200))
    {
        if (out.matrices.size() == out.matrices.capacity())
            return false;
        c.matrix = sreg32(out.matrices.size());
        out.matrices.emplace_back();
        makeMatrix(p, instance, flags, mode, out.matrices.back());
    }
    out.candidates.push_back(c);
    return true;
}

// One section's box tree for a view (guest 0x446140): depth first with the guest's stack (children pushed in
// order, popped last first). False when the guest's stack would overflow or the arena is full.
bool treeCull(const Pass& p, Arena& out, X87& x87, reg32 section, reg32 info)
{
    reg32 nodes[TreeStack];
    reg8 states[TreeStack];
    nodes[0] = rc::ld32(section + 0x28);
    states[0] = 1;
    int depth = 1;
    do
    {
        --depth;
        const reg32 node = nodes[depth];
        reg8 state = states[depth];
        if (state == 1)
        {
            float mn[3], mx[3];
            shortBox(node, mn, mx);
            reg32 ecx, edx;
            state = reg8(visible(mn, mx, rc::ld32(info + 0x80), p.half, p.zero, x87, ecx, edx));
        }
        if (state == 0 || ld16s(node + 0xc) <= 0)
            continue;
        for (sreg32 k = 0; k < ld16s(node + 0xc); ++k)
        {
            const sreg32 child = ld16s(node + 0xe + 2 * reg32(k));
            if (child >= 0)
            {
                if (!drawScenery(p, out, x87, section, info, reg32(child), state))
                    return false;
            }
            else
            {
                if (depth == TreeStack)
                    return false;
                nodes[depth] = rc::ld32(section + 0x28) - reg32(child) * 24;
                states[depth] = state;
                ++depth;
            }
        }
    } while (depth > 0);
    return true;
}

// Guest memory as phase A sees it: its stores stay here until the commit. A bit per hashed dword marks the dwords
// the stores touch; reads of unmarked dwords come from guest memory directly.
class Overlay
{
public:
    void clear()
    {
        m_writes.clear();
        memset(m_marks, 0, sizeof(m_marks));
    }
    reg32 ld32(reg32 address) const { return read(address, 4); }
    reg32 ld16(reg32 address) const { return read(address, 2); }
    void st32(reg32 address, reg32 value)
    {
        mark(address);
        mark(address + 3);
        m_writes.push_back(Write{address, value, 4});
    }
    void st8(reg32 address, reg8 value)
    {
        mark(address);
        m_writes.push_back(Write{address, value, 1});
    }
    size_t writes() const { return m_writes.size(); }

    template< typename W >
    void commit(W& writer) const
    {
        for (const Write& w : m_writes)
        {
            if (w.size == 4)
                writer.st32(w.address, w.value);
            else
                writer.st8(w.address, reg8(w.value));
        }
    }

private:
    struct Write
    {
        reg32 address;
        reg32 value;
        reg32 size;
    };

    static const int MarkBits = 10;

    static reg32 slot(reg32 address) { return ((address >> 2) * 0x9e3779b1u) >> (32 - MarkBits); }
    void mark(reg32 address)
    {
        const reg32 s = slot(address);
        m_marks[s >> 5] |= 1u << (s & 31);
    }
    bool marked(reg32 address) const
    {
        const reg32 s = slot(address);
        return (m_marks[s >> 5] >> (s & 31)) & 1;
    }

    reg32 read(reg32 address, reg32 size) const
    {
        if (!marked(address) && !marked(address + size - 1))
        {
            reg32 value = 0;
            memcpy(&value, rc::host(address), size);
            return value;
        }
        reg8 bytes[4] = {};
        memcpy(bytes, rc::host(address), size);
        for (const Write& w : m_writes)
        {
            if (w.address - address + w.size - 1 >= size + w.size - 1)
                continue;
            for (reg32 i = 0; i < w.size; ++i)
            {
                const reg32 at = w.address + i - address;
                if (at < size)
                    bytes[at] = reg8(w.value >> (8 * i));
            }
        }
        reg32 value = 0;
        memcpy(&value, bytes, size);
        return value;
    }

    std::vector<Write> m_writes;
    reg32 m_marks[(1 << MarkBits) / 32] = {};
};

// Crossing test of the point at pos (x, y floats) against count float2 vertices (guest 0x5679b0). The guest
// unrolls the edge loop four times; its second edge form skips one compare, so the forms are kept apart for the
// x87 status. ecx and edx get the guest's registers at the return.
bool pointInPolygon(reg32 pos, reg32 points, sreg32 count, X87& x87, reg32& ecx, reg32& edx)
{
    const double px = ld(pos), py = ld(pos + 4);
    const auto x = [&](sreg32 i) { return ld(points + 8 * reg32(i)); };
    const auto y = [&](sreg32 i) { return ld(points + 8 * reg32(i) + 4); };
    const auto le = [&](double a, double b) { x87.compare(a, b); return a <= b; };
    const auto lt = [&](double a, double b) { x87.compare(a, b); return a < b; };
    bool inside = false;
    // The edge from vertex p to vertex c crosses the point's y right of the point: toggle.
    const auto cross = [&](sreg32 c, sreg32 p)
    {
        const double v = (py - y(c)) * (x(p) - x(c)) / (y(p) - y(c)) + x(c);
        x87.compare(v, px);
        if (v > px)
            inside = !inside;
    };
    const auto edge = [&](sreg32 c, sreg32 p)
    {
        if (le(y(c), py) && lt(py, y(p)))
            cross(c, p);
        else if (le(y(p), py) && lt(py, y(c)))
            cross(c, p);
    };
    const auto edgeShort = [&](sreg32 c, sreg32 p)
    {
        if (le(y(c), py))
        {
            if (lt(py, y(p)) || lt(py, y(c)))
                cross(c, p);
        }
        else if (le(y(p), py) && lt(py, y(c)))
            cross(c, p);
    };
    sreg32 i = 0, previous = count - 1;
    if (count >= 4)
    {
        for (sreg32 last = 3;; last += 4)
        {
            edge(i, previous);
            edgeShort(i + 1, i);
            edge(i + 2, i + 1);
            edge(i + 3, i + 2);
            previous = i + 3;
            i += 4;
            if (!(last + 4 < count))
                break;
        }
    }
    for (; i < count; previous = i, ++i)
        edge(i, previous);
    if (count >= 1)
    {
        ecx = points + 8 * reg32(count) + 4;
        edx = 8 * reg32(count - 1);
    }
    else
    {
        ecx = rc::ld32(pos);
        edx = rc::ld32(pos + 4);
    }
    return inside;
}

// The region whose polygon contains pos, moved to the front of the region list (guest 0x446fd0), or 0.
reg32 findRegion(Overlay& o, reg32 pos, X87& x87, reg32& ecx, reg32& edx)
{
    const reg32 sentinel = RegionList + 0x10;
    for (reg32 node = o.ld32(sentinel); node != sentinel; node = o.ld32(node))
    {
        const reg32 polygon = o.ld32(node + 0xc);
        const sreg32 count = sreg32(sreg16(o.ld16(polygon + 0xa)));
        if (!pointInPolygon(pos, polygon + 0xc, count, x87, ecx, edx))
            continue;
        const reg32 next = o.ld32(node), previous = o.ld32(node + 4);
        o.st32(previous, next);
        o.st32(next + 4, previous);
        const reg32 first = o.ld32(sentinel);
        o.st32(sentinel, node);
        o.st32(first + 4, node);
        o.st32(node + 4, sentinel);
        o.st32(node, first);
        ecx = previous;
        return node;
    }
    return 0;
}

// The section numbers a view draws (guest 0x446400 with max = 0x80); ids holds one more for the guest's overrun.
// The guest pops ecx at the return from a slot that holds the culler object, or the default list when no region
// contains the view. flagsSet: the guest stored cf, pf and zf (0, 1, 1) at a call.
sreg32 whatSections(Overlay& o, reg32 object, reg32 info, sreg16* ids, X87& x87, reg32& ecxOut, reg32& edx,
                    bool& flagsSet)
{
    const sreg32 max = MaxSections;
    const reg32 view = o.ld32(info + 0x80);
    const reg32 pos = o.ld32(view + 0x40) + 0x40;
    reg32 ecx = view;
    edx = info;
    const reg32 region = findRegion(o, pos, x87, ecx, edx);
    ecxOut = region ? object : DefaultSections;
    sreg32 n = 0;
    if (region == 0)
    {
        flagsSet = true;
        for (reg32 node = o.ld32(DefaultSections); node != DefaultSections; node = o.ld32(node))
        {
            ecx = o.ld32(node + 0xc);
            edx = reg32(sreg32(ecx) % 100);
            if (sreg32(edx) < 10 && n < max)
                ids[n++] = sreg16(ecx);
        }
    }
    else
    {
        for (reg32 s = 0xa28; s < 0xa8c; ++s)
        {
            ecx = o.ld32(SectionTable + 4 * s);
            if (ecx && n < max)
                ids[n++] = sreg16(s);
        }
        edx = 0;
        if (sreg32(o.ld32(region + 0x10)) > 0)
        {
            ecx = region + 0x14;
            do
            {
                const sreg32 id = sreg32(sreg16(o.ld16(ecx)));
                if (id >= 0 && o.ld32(SectionTable + 4 * reg32(id)) && n < max)
                    ids[n++] = sreg16(id);
                ++edx;
                ecx += 2;
            } while (sreg32(edx) < sreg32(o.ld32(region + 0x10)));
        }
    }
    edx = info;
    if (o.ld32(o.ld32(info + 0x80) + 4) != 1)
        return n;
    const reg32 none = 0xffffffffu;
    const reg32 current = region ? reg32(sreg32(sreg16(o.ld16(region + 8)))) : none;
    if (current != o.ld32(0x700960))
        o.st32(0x700960, current);
    if (o.ld32(0x73612c) == 0)
        return n;
    ecx = o.ld32(0x736130);
    if (ecx != 0)
    {
        // [0x6f0888] / 5 as the guest's multiply and shifts compute it.
        sreg32 q = sreg32(sreg64(sreg32(o.ld32(0x6f0888))) * 0x66666667 >> 32) >> 1;
        q += sreg32(reg32(q) >> 31);
        edx = reg32(q);
        if (edx & 1)
        {
            sreg32 i = 0;
            for (; i < n; ++i)
            {
                edx = reg32(sreg32(ids[i]));
                if (edx == ecx)
                    break;
            }
            if (i < n)
                ids[i] = sreg16(none);
            else
            {
                edx = reg32(n % max);
                ids[edx] = sreg16(ecx);
                ++n;
            }
        }
    }
    if (o.ld32(0x736138) != 0)
    {
        o.st32(0x77aa6c, none);
        o.st32(0x77aab0, none);
        o.st8(0x77aaf9, 1);
        o.st32(0x736138, 0);
    }
    return n;
}

// Guest stores of the native culler. Shadow mode records the old values to undo them.
struct Writer
{
    struct Undo
    {
        reg32 address;
        reg32 value;
        reg32 size;
    };

    bool record = false;
    std::vector<Undo> undo;

    void st32(reg32 address, reg32 value)
    {
        if (record)
            undo.push_back(Undo{address, rc::ld32(address), 4});
        rc::st32(address, value);
    }
    void st8(reg32 address, reg8 value)
    {
        if (record)
            undo.push_back(Undo{address, rc::ld8(address), 1});
        rc::st8(address, value);
    }
    void stf32(reg32 address, float value) { st32(address, bits(value)); }
};

struct ViewPass
{
    reg32 info;
    sreg16 ids[MaxSections + 1];
    sreg32 count;
    X87 x87;
    reg32 ecx, edx;     // the guest's registers when its pass over the view returns
    bool flagsSet;
    size_t firstItem, itemCount;
    reg32 listStart, listEnd;
};

struct Item
{
    reg32 info;
    reg32 section;
    int thread;
    uint32_t first, count;
    X87 x87;
    bool failed;
};

struct Culler
{
    Pass pass;
    std::vector<ViewPass> views;
    std::vector<Item> items;
    std::vector<Arena> arenas;
    Overlay overlay;
};

Culler s_culler;

void cullBatch(void* context, int begin, int end, int thread)
{
    Culler& c = *static_cast<Culler*>(context);
    Arena& arena = c.arenas[thread];
    for (int i = begin; i < end; ++i)
    {
        Item& item = c.items[i];
        item.thread = thread;
        item.first = uint32_t(arena.candidates.size());
        item.x87 = X87();
        item.failed = !treeCull(c.pass, arena, item.x87, item.section, item.info);
        item.count = uint32_t(arena.candidates.size()) - item.first;
    }
}

// The per-frame allocation of one matrix (guest 0x401050 with one block), 0 when the frame buffer is full.
reg32 allocateMatrix(Writer& w)
{
    const reg32 cursor = rc::ld32(FrameCursor);
    const reg32 next = cursor + 0x40;
    if (next < rc::ld32(FrameEnd))
    {
        w.st32(FrameCursor, next);
        return cursor;
    }
    const reg32 refused = rc::ld32(FrameRefused) + 0x40;
    w.st32(FrameOverflow, 1);
    w.st32(FrameRefused, refused);
    return 0;
}

// flagsSet: the guest's matrix fill stored cf, pf and zf (0, 1, 1) at its return.
void commitCandidate(Writer& w, reg32 info, const Candidate& c, const Arena& arena, bool& flagsSet)
{
    reg32 matrix = 0;
    if (c.matrix >= 0)
    {
        matrix = allocateMatrix(w);
        if (!matrix)
            return;
        const Matrix& m = arena.matrices[size_t(c.matrix)];
        for (reg32 i = 0; i < 16; ++i)
            w.st32(matrix + 4 * i, m.words[i]);
        flagsSet = true;
    }
    const reg32 cursor = rc::ld32(info + 0x8c);
    if (cursor >= rc::ld32(info + 0x90))
        return;
    w.st32(info + 0x8c, cursor + 12);
    w.st32(cursor + 4, matrix);
    w.st32(cursor, c.entry);
    w.st32(cursor + 8, c.instance);
}

struct Counts
{
    int views = 0;
    int stores = 0;
    int items = 0;
    int batches = 0;
    bool forked = false;
    int candidates = 0;
    bool fallback = false;
};

// Doubles the arenas that ran full.
void growArenas()
{
    for (Arena& a : s_culler.arenas)
    {
        if (a.candidates.size() == a.candidates.capacity())
            a.candidates.reserve(a.candidates.capacity() * 2);
        if (a.matrices.size() == a.matrices.capacity())
            a.matrices.reserve(a.matrices.capacity() * 2);
    }
}

// The culler on cpu (esi = the culler object, esp at the return address). False: a case this code does not
// reproduce came up before any store the guest code would not repeat; the caller runs the guest code.
bool cullNative(x86::CPU& cpu, Writer& w, Counts& counts)
{
    const reg32 object = cpu.esi;
    const reg8 skip = rc::ld8(SkipCulling);
    if (skip != 0)
    {
        cpu.eax = (cpu.eax & 0xffffff00u) | skip;
        cpu.esp += 4;
        return true;
    }
    Culler& c = s_culler;
    // List bounds, each view's position and direction (stores the guest makes again with the same values).
    w.st32(object + 0x824, ListStart);
    w.st32(object + 0x828, ListStart);
    const sreg32 viewCount = sreg32(rc::ld32(object + 0x820));
    w.st32(object + 0x82c, ListEnd);
    reg32 ecx = cpu.ecx, edx = 0;
    for (sreg32 v = 0; v < viewCount; ++v)
    {
        const reg32 info = object + reg32(v) * InfoSize;
        // Rows 4 and 5 of the view matrix: x copied as bits, y and z through the FPU.
        reg32 m = rc::ld32(rc::ld32(info + 0x80) + 0x40);
        const double y4 = ld(m + 0x44), z4 = ld(m + 0x48);
        w.st32(info + 0xa0, rc::ld32(m + 0x40));
        w.stf32(info + 0xa4, float(y4));
        w.stf32(info + 0xa8, float(z4));
        m = rc::ld32(rc::ld32(info + 0x80) + 0x40);
        const double y5 = ld(m + 0x54), z5 = ld(m + 0x58);
        w.st32(info + 0xb0, rc::ld32(m + 0x50));
        w.stf32(info + 0xb4, float(y5));
        w.stf32(info + 0xb8, float(z5));
        ecx = rc::ld32(rc::ld32(info + 0x80) + 0xc);
        w.st32(info + 0xc0, ecx);
        edx = reg32(v + 1);
    }

    // Phase A: the sections of every view.
    c.pass = Pass{ld(HalfConstant), ld(ZeroConstant), ld(FourConstant), ld(TwoConstant), ld(ScaleConstant),
                  rc::ld32(DetailLevel)};
    c.overlay.clear();
    c.items.clear();
    c.views.resize(size_t(std::max(viewCount, 0)));
    // The guest's stack pointer in the section tree walk: its ecx and edx at the return point into that frame.
    const reg32 frame = ((cpu.esp - 0x12c) & ~15u) - 0x180;
    for (sreg32 v = 0; v < viewCount; ++v)
    {
        ViewPass& vp = c.views[size_t(v)];
        vp.info = object + reg32(v) * InfoSize;
        vp.x87 = X87();
        vp.flagsSet = false;
        vp.count = whatSections(c.overlay, object, vp.info, vp.ids, vp.x87, vp.ecx, vp.edx, vp.flagsSet);
        if (vp.count > MaxSections)
            return false;
        vp.firstItem = c.items.size();
        for (sreg32 i = 0; i < vp.count; ++i)
        {
            const sreg32 id = vp.ids[i];
            if (id < 0)
                continue;
            const reg32 section = rc::ld32(SectionTable + 4 * reg32(id));
            if (!section)
                continue;
            vp.ecx = rc::ld32(section + 0x28);
            if (!vp.ecx)
                continue;
            c.items.push_back(Item{vp.info, section, 0, 0, 0, X87(), false});
            vp.ecx = frame + 0x80;
            vp.edx = frame + 0x40;
        }
        vp.itemCount = c.items.size() - vp.firstItem;
    }
    counts.views = int(std::max(viewCount, 0));
    counts.stores = int(c.overlay.writes());

    // Phase B: the trees, on the workers.
    const int threads = settings().parallel ? pf::jobs::threads() : 1;
    if (c.arenas.size() < size_t(threads))
    {
        c.arenas.resize(size_t(threads));
        for (Arena& a : c.arenas)
        {
            a.candidates.reserve(InitialCandidates);
            a.matrices.reserve(InitialMatrices);
        }
    }
    for (Arena& a : c.arenas)
    {
        a.candidates.clear();
        a.matrices.clear();
    }
    counts.items = int(c.items.size());
    if (settings().parallel)
    {
        const pf::jobs::Stats stats = pf::jobs::run(int(c.items.size()), BatchItems, &cullBatch, &c);
        counts.batches = stats.batches;
        counts.forked = stats.forked;
    }
    else if (!c.items.empty())
    {
        cullBatch(&c, 0, int(c.items.size()), 0);
        counts.batches = 1;
    }
    for (const Item& item : c.items)
    {
        if (item.failed)
        {
            growArenas();
            return false;
        }
    }

    // Phase C: the guest's stores in its order.
    c.overlay.commit(w);
    X87 x87;
    bool flagsSet = false;
    for (ViewPass& vp : c.views)
    {
        const reg32 info = vp.info;
        w.st32(info + 0x88, rc::ld32(object + 0x828));
        w.st32(info + 0x8c, rc::ld32(object + 0x828));
        w.st32(info + 0x90, rc::ld32(object + 0x82c));
        vp.listStart = rc::ld32(info + 0x8c);
        x87.follow(vp.x87);
        flagsSet |= vp.flagsSet;
        for (size_t i = vp.firstItem; i < vp.firstItem + vp.itemCount; ++i)
        {
            const Item& item = c.items[i];
            const Arena& arena = c.arenas[size_t(item.thread)];
            for (uint32_t k = item.first; k < item.first + item.count; ++k)
                commitCandidate(w, info, arena.candidates[k], arena, flagsSet);
            counts.candidates += int(item.count);
            x87.follow(item.x87);
        }
        vp.listEnd = rc::ld32(info + 0x8c);
        w.st32(object + 0x828, vp.listEnd);
        ecx = vp.ecx;
        edx = vp.edx;
    }
    cpu.eax = rc::ld32(object + 0x820);
    cpu.ecx = ecx;
    cpu.edx = edx;
    cpu.esp += 4;
    if (flagsSet)
    {
        cpu.flags.cf = 0;
        cpu.flags.pf = 1;
        cpu.flags.zf = 1;
    }
    x87.apply(cpu);
    return true;
}

// Shadow mode: the guest's result is kept; differences from the native one are logged.
struct Shadow
{
    uint32_t frames = 0, differing = 0, fallbacks = 0, logged = 0;
    uint32_t boxes = 0, boxesDiffering = 0;
};

Shadow s_shadow;
const uint32_t ShadowLogLimit = 20;

struct Region
{
    reg32 address;
    reg32 size;
    std::vector<reg8> before;
    std::vector<bool> stored;   // bytes the native code stored
};

bool compareRegisters(const char* what, const x86::CPU& guest, const x86::CPU& native, char* text, size_t size)
{
    const struct { const char* name; reg32 g, n; } regs[] = {
        {"eax", guest.eax, native.eax}, {"ecx", guest.ecx, native.ecx}, {"edx", guest.edx, native.edx},
        {"ebx", guest.ebx, native.ebx}, {"esp", guest.esp, native.esp}, {"ebp", guest.ebp, native.ebp},
        {"esi", guest.esi, native.esi}, {"edi", guest.edi, native.edi},
        {"eflags", guest.flags.eflags, native.flags.eflags}, {"x87 depth", guest.fpu.count, native.fpu.count},
        {"x87 status", guest.fpu.status.word, native.fpu.status.word},
    };
    for (const auto& r : regs)
    {
        if (r.g != r.n)
        {
            snprintf(text, size, "%s: %s guest 0x%08x native 0x%08x", what, r.name, unsigned(r.g), unsigned(r.n));
            return false;
        }
    }
    for (int i = 0; i < 8; ++i)
    {
        const double g = guest.fpu.regs[i], n = native.fpu.regs[i];
        if (memcmp(&g, &n, sizeof(g)) != 0)
        {
            snprintf(text, size, "%s: x87 register slot %d guest %.17g native %.17g", what, i, g, n);
            return false;
        }
    }
    return true;
}

void shadowCulling(win32::WinApplication* app, x86::CPU& cpu, Counts& counts)
{
    Shadow& s = s_shadow;
    const x86::CPU entry = cpu;
    // Everything the guest may store besides what the native code stores: the culler object, the list, the frame
    // allocator, the section query globals and the region list head.
    static const reg32 fixed[][2] = {
        {ListStart, ListEnd - ListStart}, {FrameCursor, FrameRefused + 4 - FrameCursor}, {0x700960, 4},
        {0x77aa6c, 4}, {0x77aab0, 4}, {0x77aaf9, 1}, {0x736138, 4}, {RegionList, 0x18},
    };
    std::vector<Region> regions;
    regions.push_back(Region{cpu.esi, 0x830, {}, {}});
    for (const auto& f : fixed)
        regions.push_back(Region{f[0], f[1], {}, {}});
    for (Region& r : regions)
    {
        r.before.assign(rc::host(r.address), rc::host(r.address) + r.size);
        r.stored.assign(r.size, false);
    }

    Writer w;
    w.record = true;
    const bool done = cullNative(cpu, w, counts);
    struct Store { reg32 address, size, value; };
    std::vector<Store> stores;
    for (const Writer::Undo& u : w.undo)
        stores.push_back(Store{u.address, u.size, u.size == 4 ? rc::ld32(u.address) : rc::ld8(u.address)});
    for (auto u = w.undo.rbegin(); u != w.undo.rend(); ++u)
    {
        if (u->size == 4)
            rc::st32(u->address, u->value);
        else
            rc::st8(u->address, reg8(u->value));
    }
    const x86::CPU native = cpu;
    cpu = entry;
    sub_446620_guest(app, cpu);
    ++s.frames;
    if (!done)
    {
        counts.fallback = true;
        ++s.fallbacks;
        return;
    }

    char text[200] = {};
    bool same = compareRegisters("culler", cpu, native, text, sizeof(text));
    for (const Store& st : stores)
    {
        for (Region& r : regions)
        {
            for (reg32 i = 0; i < st.size; ++i)
            {
                if (st.address + i - r.address < r.size)
                    r.stored[st.address + i - r.address] = true;
            }
        }
        if (!same)
            continue;
        const reg32 guest = st.size == 4 ? rc::ld32(st.address) : rc::ld8(st.address);
        if (guest != st.value)
        {
            same = false;
            int view = -1;
            for (size_t v = 0; v < s_culler.views.size(); ++v)
            {
                if (st.address - s_culler.views[v].listStart < s_culler.views[v].listEnd - s_culler.views[v].listStart)
                    view = int(v);
            }
            if (view >= 0)
                snprintf(text, sizeof(text), "culler: view %d list entry %u: guest 0x%08x native 0x%08x at 0x%08x",
                         view, unsigned((st.address - s_culler.views[size_t(view)].listStart) / 12), unsigned(guest),
                         unsigned(st.value), unsigned(st.address));
            else
                snprintf(text, sizeof(text), "culler: guest 0x%08x native 0x%08x at 0x%08x", unsigned(guest),
                         unsigned(st.value), unsigned(st.address));
        }
    }
    for (const Region& r : regions)
    {
        for (reg32 i = 0; same && i < r.size; ++i)
        {
            const reg8 now = rc::ld8(r.address + i);
            if (!r.stored[i] && now != r.before[i])
            {
                same = false;
                snprintf(text, sizeof(text), "culler: guest stored 0x%02x at 0x%08x, native nothing", unsigned(now),
                         unsigned(r.address + i));
            }
        }
    }
    if (!same)
    {
        ++s.differing;
        if (s.logged < ShadowLogLimit)
        {
            ++s.logged;
            pf::logInfo("shadow_cull: call %u: %s", unsigned(s.frames), text);
        }
    }
}

// Perf builds: culler time and work, logged every ReportCalls calls (once per frame in the game).
struct PerfSums
{
    uint64_t us = 0;
    uint32_t maxUs = 0, calls = 0, views = 0, stores = 0, items = 0, batches = 0, forked = 0, candidates = 0;
    uint32_t fallbacks = 0;
};

PerfSums s_perf;

void report(uint32_t us, const Counts& counts)
{
    PerfSums& p = s_perf;
    p.us += us;
    p.maxUs = std::max(p.maxUs, us);
    p.views += uint32_t(counts.views);
    p.stores += uint32_t(counts.stores);
    p.items += uint32_t(counts.items);
    p.batches += uint32_t(counts.batches);
    p.forked += counts.forked ? 1 : 0;
    p.candidates += uint32_t(counts.candidates);
    p.fallbacks += counts.fallback ? 1 : 0;
    if (++p.calls < ReportCalls)
        return;
    const Settings& s = settings();
    pf::logInfo("cull: %s %.2f ms/frame (max %.2f), %u views/frame, %u section query stores/frame, %u items/frame in "
                "%u batches, forked %u%%, %u models/frame, %u guest fallbacks",
                !s.native ? "guest" : s.shadow ? "shadow" : s.parallel ? "parallel" : "native",
                double(p.us) / 1000.0 / p.calls, double(p.maxUs) / 1000.0, p.views / p.calls, p.stores / p.calls,
                p.items / p.calls, p.batches / p.calls,
                p.forked * 100 / p.calls, p.candidates / p.calls, p.fallbacks);
    if (s.native && s.shadow)
        pf::logInfo("shadow_cull: %u calls compared, %u differ, %u guest fallbacks; boxes %u compared, %u differ",
                    unsigned(s_shadow.frames), unsigned(s_shadow.differing), unsigned(s_shadow.fallbacks),
                    unsigned(s_shadow.boxes), unsigned(s_shadow.boxesDiffering));
    p = PerfSums();
}

void visibleStateNative(x86::CPU& cpu)
{
    const reg32 lo = cpu.edx, hi = cpu.ecx;
    // The guest copies x as bits and y, z through the FPU; both keep the value.
    float mn[3] = { rc::ldf32(lo), float(ld(lo + 4)), float(ld(lo + 8)) };
    float mx[3] = { rc::ldf32(hi), float(ld(hi + 4)), float(ld(hi + 8)) };
    if (cpu.eax)
        transformBox(mn, mx, cpu.eax);
    X87 x87;
    reg32 ecx, edx;
    const int state = visible(mn, mx, rc::ld32(cpu.esp + 4), ld(HalfConstant), ld(ZeroConstant), x87, ecx, edx);
    cpu.eax = reg32(state);
    cpu.ecx = ecx;
    cpu.edx = edx;
    cpu.esp += 8;
    x87.apply(cpu);
}

}

void doCulling(win32::WinApplication* app, x86::CPU& cpu)
{
    const Settings& s = settings();
    const uint32_t start = pf::perf::enabled() ? pf::perf::now() : 0;
    Counts counts;
    if (!s.native)
        sub_446620_guest(app, cpu);
    else if (s.shadow)
        shadowCulling(app, cpu, counts);
    else
    {
        Writer w;
        if (!cullNative(cpu, w, counts))
        {
            counts.fallback = true;
            sub_446620_guest(app, cpu);
        }
    }
    if (pf::perf::enabled())
        report(pf::perf::now() - start, counts);
}

int boxState(const float mn[3], const float mx[3], reg32 view, double& compareA, double& compareB)
{
    X87 x87;
    reg32 ecx, edx;
    const int state = visible(mn, mx, view, ld(HalfConstant), ld(ZeroConstant), x87, ecx, edx);
    compareA = x87.a;
    compareB = x87.b;
    return state;
}

void getVisibleState(win32::WinApplication* app, x86::CPU& cpu)
{
    const Settings& s = settings();
    if (!s.native)
    {
        sub_40db20_guest(app, cpu);
        return;
    }
    if (!s.shadow)
    {
        visibleStateNative(cpu);
        return;
    }
    x86::CPU native = cpu;
    visibleStateNative(native);
    const reg32 caller = rc::ld32(cpu.esp), lo = cpu.edx, hi = cpu.ecx, matrix = cpu.eax;
    sub_40db20_guest(app, cpu);
    ++s_shadow.boxes;
    char text[200];
    if (!compareRegisters("box", cpu, native, text, sizeof(text)))
    {
        ++s_shadow.boxesDiffering;
        if (s_shadow.logged < ShadowLogLimit)
        {
            ++s_shadow.logged;
            pf::logInfo("shadow_cull: %s (caller 0x%08x, min 0x%08x, max 0x%08x, matrix 0x%08x)", text,
                        unsigned(caller), unsigned(lo), unsigned(hi), unsigned(matrix));
        }
    }
}

}}
