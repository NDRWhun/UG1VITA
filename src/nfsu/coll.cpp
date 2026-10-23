// Native track topology queries: the ground height under a point (guest 0x440a40), the distance of a point outside
// a track polygon (0x4400e0) and the rebuild of a topology coordinate's polygon cache around a point (0x4407b0 with
// its callees 0x567600, 0x441c10, 0x441500, 0x441530, 0x441660, 0x441b90, 0x441b20, 0x4400d0, 0x43ff30, 0x43fd20,
// 0x43fe70, 0x566ed0, 0x567370). Wheels, cameras, AI, effects and car rendering query the ground through these.
//
// Bit-exact with the translated guest code: every x87 chain is evaluated in double in the guest's order and
// rounded to float where the guest stores a dword, stack temporaries included. Guest memory is written in the
// guest's order; stores to the guest's own stack frames (dead after the return) are not made. The returned
// registers and flags, the x87 status word (C0, C2, C3 of the last compare the translated code makes) and the
// x87 slot above the stack are reproduced. Compares of 0x441660 and 0x441530 are not tracked: 0x441c10 always
// compares again after them.
//
// The queries are made one at a time from sequential game code and the cache rebuild changes shared state (the
// track polygon slot pool and list), so this runs on the calling thread.

#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>

#include <lib/winapp.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fpu.h>
#include <recomp.h>
#include <vector>
#include <x86.h>

namespace nfsu
{

// The translated guest bodies of the overridden functions (generated).
void sub_4400e0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_440a40_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4407b0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace coll
{

namespace
{

using x86::reg8;
using x86::reg16;
using x86::reg32;
using x86::sreg16;
using x86::sreg32;
using x86::sreg64;

// Float constants in .rdata.
const reg32 MinusOne = 0x6cc8a8;
const reg32 Zero = 0x6cc7a4;
const reg32 One = 0x6cc7bc;
const reg32 Half = 0x6cc7dc;
const reg32 SettleDistance = 0x6cccec;  // 28.8: the predicted box is small enough
const reg32 Shrink = 0x6cca08;          // 0.75
const reg32 GridMin = 0x6ccce8;         // -32768
const reg32 GridMax = 0x6cc8c8;         // 32767
const reg32 ToFixed = 0x6cc840;         // 65536
const reg32 FromFixed = 0x6cc83c;       // 1/65536
const reg32 Cell = 0x6cc8f4;            // 32: the grid square size
const reg32 Lift = 0x6cc908;            // 4

// Guest data.
const reg32 PolygonPool = 0x736184;     // slot pool of track polygons
const reg32 TopologyTree = 0x77b130;

const reg32 FloatMax = 0x7f7fffff;
const reg32 FloatLowest = 0xff7fffff;
const int MaxPolygons = 0xa0;           // a coordinate's polygon cache

inline double ld(reg32 address) { return double(rc::ldf32(address)); }
inline double asDouble(reg32 bits) { float f; memcpy(&f, &bits, 4); return double(f); }
inline reg32 bits(float value) { reg32 r; memcpy(&r, &value, 4); return r; }
inline bool parityFlag(reg32 value) { return !x86::parity8(reg8(value)); }
// A track polygon's int16 grid coordinate at p (scaled by 2^13, then to world units).
inline double fixedCoordinate(reg32 p, double scale)
{
    return double(sreg32(reg32(sreg32(sreg16(rc::ld16(p)))) << 13)) * scale;
}

struct Settings
{
    bool native;
    bool shadow;
};

const Settings& settings()
{
    static const Settings s = []
    {
        const Settings r = { pf::Config::getBool("coll_native", true), pf::Config::getBool("shadow_coll", false) };
        pf::logInfo("coll: topology queries %s%s", r.native ? "native" : "guest",
                    r.native && r.shadow ? ", shadow compare" : "");
        return r;
    }();
    return s;
}

// The x87 state guest code leaves behind: the operands of its last compare and the last value it left in the
// register slot above the stack.
struct X87
{
    bool compared = false;
    bool slotSet = false;
    double a = 0, b = 0;
    double slot = 0;

    void compare(double x, double y) { compared = true; a = x; b = y; }
    void setSlot(double x) { slotSet = true; slot = x; }
    void apply(x86::CPU& cpu) const
    {
        if (compared)
            cpu.fpu.compare(a, b);
        if (slotSet)
            cpu.fpu.regs[(cpu.fpu.count + 1) & 7] = slot;
    }
};

// Guest stores. Shadow mode records the old values to undo them.
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

    [[gnu::noinline, gnu::cold]] void log(reg32 address, reg32 size)
    {
        undo.push_back(Undo{address, size == 4 ? rc::ld32(address) : size == 2 ? rc::ld16(address)
                                                                                : rc::ld8(address), size});
    }
    void remember(reg32 address, reg32 size)
    {
        if (__builtin_expect(record, 0))
            log(address, size);
    }
    void st32(reg32 address, reg32 value) { remember(address, 4); rc::st32(address, value); }
    void st16(reg32 address, reg16 value) { remember(address, 2); rc::st16(address, value); }
    void st8(reg32 address, reg8 value) { remember(address, 1); rc::st8(address, value); }
    void stf32(reg32 address, float value) { st32(address, bits(value)); }
};

// ---- distance outside a track polygon (0x4400e0) ----

// Cross product of the edge from vertex offset a to b with the point q, rounded to float as the guest stores it
// in its stack slot (stored).
inline double edgeSide(reg32 p, reg32 q, reg32 a, reg32 b, float& stored)
{
    double t = (ld(p + b + 4) - ld(p + a + 4)) * (ld(q) - ld(p + a));
    t -= (ld(q + 4) - ld(p + a + 4)) * (ld(p + b) - ld(p + a));
    stored = float(t);
    return double(stored);
}

// Largest edge value of point q against polygon p (vertices at +0x30, three or four by [p+9]), -1 at least; it
// returns as soon as one is positive. stored: the last edge value in the guest's slot, which it pops into ecx.
double amountOutside(reg32 p, reg32 q, X87& x, float& stored)
{
    const double zero = ld(Zero);
    double v = ld(MinusOne);
    const double s0 = edgeSide(p, q, 0x30, 0x40, stored);
    x.compare(v, s0);
    if (!(v > s0))
    {
        v = s0;
        x.compare(v, zero);
        if (v > zero)
            return v;
    }
    static const reg32 edges[2][2] = {{0x40, 0x50}, {0x60, 0x30}};
    for (const auto& e : edges)
    {
        const double s = edgeSide(p, q, e[0], e[1], stored);
        x.compare(v, s);
        if (!(v > s))
            v = s;
        x.compare(v, zero);
        if (v > zero)
            return v;
    }
    if (rc::ld8(p + 9) != 4)
        return v;
    const double s = edgeSide(p, q, 0x50, 0x60, stored);
    x.compare(v, s);
    if (!(v > s))
        v = s;
    return v;
}

// Height of polygon p's plane (normal at +0x70, distance at +0x7c) under point q.
inline double planeHeight(reg32 p, reg32 q)
{
    double t = ld(p + 0x74) * ld(q + 4);
    t += ld(p + 0x70) * ld(q);
    t -= ld(p + 0x7c);
    t /= ld(p + 0x78);
    return -t;
}

// ---- interest box and polygon cache (0x4407b0 and callees) ----

// Grows the box lo..hi (float2 each) to hold the square of half size r around (px, py) (0x567600). Returns the
// guest's ecx: its stack slot, last holding hi.x.
reg32 expandBox(double px, double py, reg32 lo, reg32 hi, double r, X87& x, Writer& w)
{
    double a = px - r;
    const double lx = ld(lo);
    x.compare(lx, a);
    if (lx < a)
        a = lx;
    double b = py - r;
    const double ly = ld(lo + 4);
    x.compare(ly, b);
    if (ly < b)
        b = ly;
    double c = r + px;
    const double hx = ld(hi);
    x.compare(hx, c);
    if (hx > c)
        c = hx;
    double d = r + py;
    const double hy = ld(hi + 4);
    x.compare(hy, d);
    if (hy > d)
        d = hy;
    w.stf32(lo, float(a));
    w.stf32(lo + 4, float(b));
    w.stf32(hi, float(c));
    w.stf32(hi + 4, float(d));
    return bits(float(hx));
}

// A coordinate rounded down to the grid of 32 units, through _ftol2, which leaves its input in the slot.
double gridCorner(double value, X87& x)
{
    const double scaled = value * ld(ToFixed);
    x.setSlot(scaled);
    const reg32 fixed = reg32(x86::fistInteger<sreg64>(std::trunc(scaled))) & 0xffe00000u;
    return double(sreg32(fixed)) * ld(FromFixed);
}

// The grid square holding (px, py) from the tree's quadtree of ten levels, 0 if none (0x441530 with no nearest
// search).
reg32 findSquare(reg32 tree, double px, double py)
{
    const reg32 root = rc::ld32(tree);
    const double cell = ld(Cell), half = ld(Half);
    reg32 node = root;
    float cx = -16384.0f, cy = -16384.0f;
    for (reg32 level = 10;; --level)
    {
        const double size = double(sreg32(1u << level)) * cell * half;
        const float midX = float(double(cx) + size);
        const double midY = size + double(cy);
        reg32 quadrant = px >= double(midX) ? 1 : 0;
        if (py >= midY)
            quadrant += 2;
        if (quadrant & 1)
            cx = midX;
        if (quadrant & 2)
            cy = float(midY);
        const reg32 child = rc::ld16(node + 2 * quadrant);
        if (child == 0)
            return 0;
        if (level == 1)
            return (child << 4) + rc::ld32(tree + 8);
        node = root + child * 8;
    }
}

// Appends the unmarked topology polygons of a square whose bounds overlap lo..hi and marks them (0x441660).
sreg32 squarePolygons(reg32 square, reg32 lo, reg32 hi, reg32 tree, reg32* out, sreg32 capacity, Writer& w)
{
    const reg32 index = rc::ld32(tree + 0x18), table = rc::ld32(tree + 0x10);
    const reg32 start = rc::ld32(square + 8), end = start + rc::ld32(square + 0xc);
    const double maxX = ld(hi), maxY = ld(hi + 4), minX = ld(lo), minY = ld(lo + 4);
    const double scale = ld(FromFixed);
    sreg32 count = 0;
    for (reg32 i = start; sreg32(i) < sreg32(end); ++i)
    {
        const reg32 polygon = (reg32(rc::ld16(index + 2 * i)) << 5) + table;
        if (rc::ld8(polygon))
            continue;
        const reg32 corners = rc::ld8(polygon + 1);
        if (!(fixedCoordinate(polygon + 8 + (corners & 3) * 2, scale) <= maxX))
            continue;
        if (!(fixedCoordinate(polygon + 0x10 + ((corners >> 2) & 3) * 2, scale) <= maxY))
            continue;
        if (!(fixedCoordinate(polygon + 8 + ((corners >> 4) & 3) * 2, scale) >= minX))
            continue;
        if (!(fixedCoordinate(polygon + 0x10 + (corners >> 6) * 2, scale) >= minY))
            continue;
        if (count < capacity)
        {
            out[count++] = polygon;
            w.st8(polygon, 1);
        }
    }
    return count;
}

// Address of slot index of the track polygon pool, 0 past its blocks (0x43fe70).
reg32 slotAddress(sreg32 index)
{
    const reg32 first = rc::ld32(PolygonPool);
    for (reg32 pool = first; pool; pool = rc::ld32(pool + 8))
    {
        const sreg32 count = sreg32(rc::ld32(pool + 0x24));
        if (index >= 0 && index < count)
            return rc::ld32(first + 0x28) * reg32(index) + pool + 0x30;
        index -= count;
    }
    return 0;
}

// Slot index of address in the pool, -1 if outside (0x566ed0).
reg32 slotIndex(reg32 pool, reg32 address)
{
    if (!pool)
        return 0xffffffffu;
    const reg32 size = rc::ld32(pool + 0x28);
    reg32 base = 0;
    for (; pool; pool = rc::ld32(pool + 8))
    {
        const reg32 q = rc::divideUnsigned(0, address - pool - 0x30, size).quotient;
        if (sreg32(q) >= 0 && sreg32(q) < sreg32(rc::ld32(pool + 0x24)))
            return q + base;
        base += rc::ld32(pool + 0x24);
    }
    return 0xffffffffu;
}

// Frees up to 101 unused track polygons of the tree's list into the pool (0x441b20); the guest breaks when there
// is none.
void flushUnused(reg32 tree, Writer& w)
{
    const reg32 head = tree + 0x50;
    reg32 node = rc::ld32(head);
    if (node == head)
        x86::guestTrap(0x441b82, "int3");
    const reg32 pool = rc::ld32(PolygonPool);
    sreg32 freed = 0;
    do
    {
        if (rc::ld8(node + 8) == 0)
        {
            const reg32 next = rc::ld32(node), previous = rc::ld32(node + 4);
            w.st32(previous, next);
            w.st32(next + 4, previous);
            const reg32 topology = rc::ld32(node + 0xc);
            if (topology)
                w.st16(topology + 6, 0xffff);
            const reg32 used = rc::ld32(pool + 0x18) - 1;
            const reg32 freeHead = rc::ld32(pool + 0x10);
            ++freed;
            w.st32(node, freeHead);
            w.st32(pool + 0x18, used);
            w.st32(pool + 0x10, node);
            if (freed > 0x64)
                break;
            node = previous;
        }
        node = rc::ld32(node);
    } while (node != head);
    if (freed == 0)
        x86::guestTrap(0x441b82, "int3");
}

// A track polygon at p built from topology polygon t: bounds, vertices, normal, plane distance (0x43ff30 with
// 0x43fd20 and 0x567370).
void buildTrackPolygon(reg32 p, reg32 t, X87& x, Writer& w)
{
    w.st8(p + 0xb, rc::ld8(t + 2));
    w.st8(p + 0xa, rc::ld8(t + 3));
    w.st8(p + 8, 0);
    w.st32(p + 0x10, 0);
    const reg32 corners = rc::ld8(t + 1);
    const double scale = ld(FromFixed);
    w.stf32(p + 0x14, float(fixedCoordinate(t + 8 + (corners & 3) * 2, scale)));
    w.stf32(p + 0x18, float(fixedCoordinate(t + 0x10 + ((corners >> 2) & 3) * 2, scale)));
    w.stf32(p + 0x1c, float(fixedCoordinate(t + 8 + ((corners >> 4) & 3) * 2, scale)));
    w.stf32(p + 0x20, float(fixedCoordinate(t + 0x10 + (corners >> 6) * 2, scale)));
    const reg32 base = reg32(sreg32(sreg16(rc::ld16(t + 4)))) << 8;
    for (reg32 k = 0; k < 4; ++k)
    {
        const reg32 v = p + 0x30 + 0x10 * k;
        w.stf32(v, float(fixedCoordinate(t + 8 + 2 * k, scale)));
        w.stf32(v + 4, float(fixedCoordinate(t + 0x10 + 2 * k, scale)));
        const reg32 z = (base + reg32(sreg32(sreg16(rc::ld16(t + 0x18 + 2 * k))))) << 8;
        w.stf32(v + 8, float(double(sreg32(z)) * scale));
    }
    const reg8 flags = rc::ld8(p + 0xa);
    w.st8(p + 9, 4);
    if (!(flags & 0x10))
        w.st8(p + 9, 3);
    if (flags & 4)
    {
        for (reg32 offset = 0x38; offset <= 0x68; offset += 0x10)
            w.stf32(p + offset, float(ld(p + offset) * ld(Lift)));
    }
    // Normal: (v1 - v0) x (v2 - v0) with the guest's float temporaries, then normalised.
    const reg32 v0 = p + 0x30;
    const double e1x = ld(p + 0x40) - ld(v0), e1y = ld(p + 0x44) - ld(v0 + 4), e1z = ld(p + 0x48) - ld(v0 + 8);
    const double e2x = double(float(ld(p + 0x50) - ld(v0)));
    const double e2y = double(float(ld(p + 0x54) - ld(v0 + 4)));
    const double e2z = ld(p + 0x58) - ld(v0 + 8);
    const double nx = double(float(e2z * e1y - e2y * e1z));
    const double ny = double(float(e1z * e2x - e2z * e1x));
    const float nzf = float(e2y * e1x - e1y * e2x);
    const double nz = double(nzf);
    double length = nx * nx;
    length += ny * ny;
    length += nz * nz;
    length = std::sqrt(length);
    x.compare(length, ld(Zero));
    reg32 n[3];
    if (length == ld(Zero))
    {
        n[0] = 0x3f800000u;
        n[1] = 0;
        n[2] = 0;
    }
    else
    {
        const double inverse = ld(One) / length;
        n[0] = bits(float(inverse * nx));
        n[1] = bits(float(ny * inverse));
        n[2] = bits(float(double(nzf) * inverse));
    }
    w.st32(p + 0x74, n[1]);
    w.st32(p + 0x70, n[0]);
    w.st32(p + 0x78, n[2]);
    double d = ld(v0 + 8) * ld(p + 0x78);
    d += ld(v0 + 4) * ld(p + 0x74);
    d += ld(v0) * ld(p + 0x70);
    w.stf32(p + 0x7c, float(d));
    w.st16(t + 6, reg16(slotIndex(rc::ld32(PolygonPool), p)));
    w.st32(p + 0xc, t);
}

// The track polygon of topology polygon t, made from a free pool slot and appended to the tree's list if it has
// none; its use count goes up (0x441b90).
reg32 assignTrackPolygon(reg32 t, reg32 tree, X87& x, Writer& w)
{
    const sreg32 slot = sreg32(sreg16(rc::ld16(t + 6)));
    if (slot >= 0)
    {
        const reg32 p = slotAddress(slot);
        w.st8(p + 8, reg8(rc::ld8(p + 8) + 1));
        return p;
    }
    const reg32 pool = rc::ld32(PolygonPool);
    if (rc::ld32(pool + 0x18) == rc::ld32(pool + 0x2c))
        flushUnused(tree, w);
    const reg32 used = rc::ld32(pool + 0x18) + 1;
    const reg32 freeSlot = rc::ld32(pool + 0x10);
    reg32 p = 0;
    if (freeSlot)
    {
        const reg32 next = rc::ld32(freeSlot);
        if (sreg32(used) > sreg32(rc::ld32(pool + 0x1c)))
            w.st32(pool + 0x1c, used);
        w.st32(pool + 0x18, used);
        w.st32(pool + 0x10, next);
        buildTrackPolygon(freeSlot, t, x, w);
        p = freeSlot;
    }
    const reg32 head = tree + 0x50, tail = rc::ld32(tree + 0x54);
    w.st32(tail, p);
    w.st32(head + 4, p);
    w.st32(p, head);
    w.st32(p + 4, tail);
    w.st8(p + 8, reg8(rc::ld8(p + 8) + 1));
    return p;
}

// The track polygons of every grid square that the box lo..hi touches, at most capacity (0x441c10).
sreg32 buildTrackPolygons(reg32 lo, reg32 hi, reg32 tree, reg32* out, sreg32 capacity, X87& x, Writer& w)
{
    reg32 found[MaxPolygons];
    sreg32 n = 0;
    const double gridMin = ld(GridMin), gridMax = ld(GridMax), cell = ld(Cell);
    const double minX = ld(lo), maxX = ld(hi), minY = ld(lo + 4), maxY = ld(hi + 4);
    x.compare(minX, gridMin);
    if (!(minX > gridMin))
        return 0;
    x.compare(maxX, gridMax);
    if (!(maxX < gridMax))
        return 0;
    x.compare(minY, gridMin);
    if (!(minY > gridMin))
        return 0;
    x.compare(maxY, gridMax);
    if (!(maxY < gridMax))
        return 0;
    // 0x441500 returns the corner in the slot.
    const double firstY = gridCorner(minY, x);
    x.setSlot(firstY);
    float y = float(firstY);
    x.compare(firstY, maxY);
    if (firstY <= maxY)
    {
        double nextY;
        do
        {
            const double firstX = gridCorner(minX, x);
            float xf = float(firstX);
            x.compare(firstX, maxX);
            if (firstX <= maxX)
            {
                double nextX;
                do
                {
                    const reg32 square = findSquare(tree, double(xf), double(y));
                    if (square)
                        n += squarePolygons(square, lo, hi, tree, found + n, capacity - n, w);
                    nextX = double(xf) + cell;
                    xf = float(nextX);
                    x.compare(nextX, maxX);
                } while (nextX <= maxX);
            }
            nextY = double(y) + cell;
            y = float(nextY);
            x.compare(nextY, maxY);
        } while (nextY <= maxY);
    }
    for (sreg32 k = 0; k < n; ++k)
    {
        w.st8(found[k], 0);
        out[k] = assignTrackPolygon(found[k], tree, x, w);
    }
    return n;
}

// Registers the interest box rebuild returns. eaxStatus: eax holds the x87 status word in its low half.
struct BoxResult
{
    reg32 eax, ecx, edx;
    bool eaxStatus;
    bool cf, pf, zf;
};

// Sets the coordinate's interest box around pt (half size radius + 0.5, stretched along vel when given) and, if
// it is not inside the old box, refills its polygon cache: plain, then flag 2, then flag 8 polygons (0x4407b0).
BoxResult setInterestBox(reg32 coord, reg32 pt, reg32 radiusBits, reg32 vel, X87& x, Writer& w)
{
    w.st32(coord, FloatMax);
    w.st32(coord + 4, FloatMax);
    w.st32(coord + 8, FloatLowest);
    w.st32(coord + 0xc, FloatLowest);
    const double radius = asDouble(radiusBits);
    const reg32 slot = expandBox(ld(pt), ld(pt + 4), coord, coord + 8, radius, x, w);
    {
        const double minX = ld(coord), maxX = ld(coord + 8), minY = ld(coord + 4), maxY = ld(coord + 0xc);
        x.compare(minX, ld(coord + 0x10));
        if (minX >= ld(coord + 0x10))
        {
            x.compare(maxX, ld(coord + 0x18));
            if (maxX <= ld(coord + 0x18))
            {
                x.compare(minY, ld(coord + 0x14));
                if (minY >= ld(coord + 0x14))
                {
                    x.compare(maxY, ld(coord + 0x1c));
                    if (maxY <= ld(coord + 0x1c))
                        return BoxResult{radiusBits & 0xffff0000u, slot, coord + 0x18, true, false, false, false};
                }
            }
        }
    }
    const sreg32 previous = sreg32(rc::ld32(coord + 0x2a0));
    for (sreg32 k = 0; k < previous; ++k)
    {
        const reg32 p = rc::ld32(coord + 0x20 + 4 * reg32(k));
        w.st8(p + 8, reg8(rc::ld8(p + 8) - 1));
    }
    const double grown = radius + ld(Half);
    for (reg32 offset = 0x2a0; offset <= 0x2b0; offset += 4)
        w.st32(coord + offset, 0);
    const reg32 lo = coord + 0x10, hi = coord + 0x18;
    const auto resetBox = [&]
    {
        w.st32(lo, FloatMax);
        w.st32(lo + 4, FloatMax);
        w.st32(hi, FloatLowest);
        w.st32(hi + 4, FloatLowest);
    };
    const double r = double(float(grown));
    if (vel == 0)
    {
        resetBox();
        expandBox(ld(pt), ld(pt + 4), lo, hi, r, x, w);
    }
    else
    {
        // The box over the point and its predicted position, the time step shrinking until it is small enough.
        float t = asDouble(0x3e19999au);
        for (int step = 0;;)
        {
            resetBox();
            expandBox(ld(pt), ld(pt + 4), lo, hi, r, x, w);
            double fx = double(t) * ld(vel);
            fx += ld(pt);
            double fy = double(t) * ld(vel + 4);
            fy += ld(pt + 4);
            expandBox(double(float(fx)), double(float(fy)), lo, hi, r, x, w);
            const double dx = ld(lo) - ld(hi), dy = ld(lo + 4) - ld(hi + 4);
            double square = dx * dx;
            square += dy * dy;
            const double size = std::sqrt(square);
            x.compare(size, ld(SettleDistance));
            if (size < ld(SettleDistance))
                break;
            t = float(double(t) * ld(Shrink));
            if (step == 3)
                t = 0.0f;
            if (++step >= 5)
                break;
        }
    }
    reg32 polygons[MaxPolygons];
    const sreg32 n = buildTrackPolygons(lo, hi, TopologyTree, polygons, MaxPolygons, x, w);
    // pf follows the guest's last test: of n, or of a polygon's flags.
    bool pf = parityFlag(reg32(n));
    reg32 ecx = 0;
    const auto append = [&](reg8 mask, bool set)
    {
        for (sreg32 i = 0; i < n; ++i)
        {
            const reg32 p = polygons[i];
            const reg8 r = rc::ld8(p + 0xa) & mask;
            pf = parityFlag(r);
            if ((r != 0) == set)
            {
                w.st32(coord + 0x20 + 4 * ecx, p);
                ++ecx;
            }
        }
    };
    append(0xa, false);
    w.st32(coord + 0x2a4, ecx);
    if (ecx != reg32(n))
    {
        pf = parityFlag(reg32(n));
        append(2, true);
    }
    w.st32(coord + 0x2a8, ecx - rc::ld32(coord + 0x2a4));
    if (ecx != reg32(n))
    {
        pf = parityFlag(reg32(n));
        append(8, true);
    }
    const reg32 a = ecx - rc::ld32(coord + 0x2a4), b = rc::ld32(coord + 0x2a8);
    ecx = a - b;
    w.st32(coord + 0x2ac, ecx);
    w.st32(coord + 0x2a0, reg32(n));
    return BoxResult{reg32(n), ecx, b, false, a < b, pf, a == b};
}

// ---- entries ----

// 0x4400e0: ecx = polygon, edx = point; st0 = the distance.
void amountOutsideNative(x86::CPU& cpu)
{
    X87 x;
    float stored;
    const double v = amountOutside(cpu.ecx, cpu.edx, x, stored);
    x.apply(cpu);
    cpu.eax = (cpu.eax & 0xffff0000u) | cpu.fpu.status.word;
    cpu.ecx = bits(stored);
    cpu.fpu.regs[(cpu.fpu.count + 1) & 7] = v;
    cpu.fpu.count += 1;
    cpu.esp += 4;
}

// 0x4407b0: edi = coordinate; stack: point, radius, velocity or 0.
void interestBoxNative(x86::CPU& cpu, Writer& w)
{
    const reg32 esp = cpu.esp;
    X87 x;
    const BoxResult r = setInterestBox(cpu.edi, rc::ld32(esp + 4), rc::ld32(esp + 8), rc::ld32(esp + 0xc), x, w);
    x.apply(cpu);
    cpu.eax = r.eaxStatus ? r.eax | cpu.fpu.status.word : r.eax;
    cpu.ecx = r.ecx;
    cpu.edx = r.edx;
    cpu.esp = esp + 0x10;
    cpu.flags.cf = r.cf;
    cpu.flags.pf = r.pf;
    cpu.flags.zf = r.zf;
}

// 0x440a40: stack: coordinate, point, terrain out, normal out, on-track flag out; st0 = the ground height. The
// last polygon hit is tried first; outside the interest box the cache is rebuilt; then the highest polygon under
// the point below point z + 1 (any height in a second pass), or the point's z when there is none.
void elevationNative(x86::CPU& cpu, Writer& w)
{
    const reg32 esp = cpu.esp;
    const reg32 coord = rc::ld32(esp + 4), pt = rc::ld32(esp + 8);
    const reg32 terrain = rc::ld32(esp + 0xc), normal = rc::ld32(esp + 0x10), flag = rc::ld32(esp + 0x14);
    X87 x;
    const double zero = ld(Zero);
    reg32 eax = 0, ecx = 0, edx = cpu.edx;
    double height = 0;
    if (flag)
        w.st8(flag, 1);
    const reg32 last = rc::ld32(coord + 0x2b0);
    bool done = false;
    if (last)
    {
        edx = pt;
        float stored;
        const double outside = amountOutside(last, pt, x, stored);
        x.compare(outside, zero);
        if (outside < zero)
        {
            height = planeHeight(last, pt);
            if (terrain)
                w.st32(terrain, reg32(sreg32(x86::sreg8(rc::ld8(last + 0xb)))));
            if (!normal)
            {
                eax = terrain;
                ecx = 0;
            }
            else
            {
                const reg32 p = rc::ld32(coord + 0x2b0);
                const float ny = float(ld(p + 0x74)), nz = float(ld(p + 0x78));
                edx = rc::ld32(p + 0x70);
                w.stf32(normal + 4, ny);
                w.st32(normal, edx);
                w.stf32(normal + 8, nz);
                eax = p + 0x70;
                ecx = normal;
            }
            done = true;
        }
    }
    if (!done)
    {
        const double px = ld(pt), py = ld(pt + 4);
        bool rebuild = true;
        x.compare(px, ld(coord));
        if (!(px < ld(coord)))
        {
            x.compare(px, ld(coord + 8));
            if (!(px > ld(coord + 8)))
            {
                x.compare(py, ld(coord + 4));
                if (!(py < ld(coord + 4)))
                {
                    x.compare(py, ld(coord + 0xc));
                    rebuild = py > ld(coord + 0xc);
                }
            }
        }
        if (rebuild)
            edx = setInterestBox(coord, pt, 0x41000000u, 0, x, w).edx;
        // Pass 0 takes polygons below z + 1, pass 1 any.
        float limit = float(ld(pt + 8) + ld(One));
        sreg32 index = -1;
        reg32 hit = 0;
        for (int pass = 0; pass < 2 && index < 0; ++pass)
        {
            float best = asDouble(FloatLowest);
            for (sreg32 i = 0; i < sreg32(rc::ld32(coord + 0x2a4)); ++i)
            {
                const reg32 p = rc::ld32(coord + 0x20 + 4 * reg32(i));
                x.compare(px, ld(p + 0x14));
                if (!(px >= ld(p + 0x14)))
                    continue;
                x.compare(px, ld(p + 0x1c));
                if (!(px <= ld(p + 0x1c)))
                    continue;
                x.compare(py, ld(p + 0x18));
                if (!(py >= ld(p + 0x18)))
                    continue;
                x.compare(py, ld(p + 0x20));
                if (!(py <= ld(p + 0x20)))
                    continue;
                edx = pt;
                float stored;
                const double outside = amountOutside(p, pt, x, stored);
                const float outsideStored = float(outside);
                x.compare(outside, zero);
                if (!(outside <= zero))
                    continue;
                const double h = planeHeight(p, pt);
                x.compare(h, double(best));
                if (!(h > double(best)))
                    continue;
                x.compare(h, double(limit));
                if (!(h < double(limit)))
                    continue;
                if (!(rc::ld8(p + 0xa) & 1))
                {
                    x.compare(double(outsideStored), zero);
                    if (double(outsideStored) < zero)
                    {
                        // Strictly inside a plain polygon: taken at once and remembered.
                        w.st32(coord + 0x2b0, p);
                        index = i;
                        hit = p;
                        height = h;
                        break;
                    }
                }
                best = float(h);
                index = i;
            }
            if (index >= 0 && !hit)
                height = double(best);
            limit = asDouble(FloatMax);
        }
        if (index < 0)
        {
            height = ld(pt + 8);
            if (flag)
                w.st8(flag, 0);
            if (terrain)
                w.st32(terrain, 1);
            if (normal)
            {
                w.st32(normal, 0);
                w.st32(normal + 4, 0);
                w.st32(normal + 8, 0xbf800000u);
            }
            eax = normal;
            ecx = 2;
        }
        else
        {
            // Polygons found after the first four move to the front of the cache.
            ecx = rc::ld32(coord + 0x20 + 4 * reg32(index));
            if (index > 4)
            {
                for (reg32 k = reg32(index); k > 0; --k)
                {
                    edx = rc::ld32(coord + 0x1c + 4 * k);
                    w.st32(coord + 0x20 + 4 * k, edx);
                }
                w.st32(coord + 0x20, ecx);
            }
            if (terrain)
            {
                edx = reg32(sreg32(x86::sreg8(rc::ld8(ecx + 0xb))));
                w.st32(terrain, edx);
            }
            eax = normal;
            if (normal)
            {
                const float ny = float(ld(ecx + 0x74)), nz = float(ld(ecx + 0x78));
                const reg32 nx = rc::ld32(ecx + 0x70);
                w.stf32(normal + 4, ny);
                w.st32(normal, nx);
                w.stf32(normal + 8, nz);
                ecx = nx;
            }
        }
    }
    x.apply(cpu);
    cpu.eax = eax;
    cpu.ecx = ecx;
    cpu.edx = edx;
    // add esp, 0xc before the ret 0x14.
    const reg32 frame = esp - 0xc;
    cpu.flags.cf = frame + 0xc < frame;
    cpu.flags.zf = frame + 0xc == 0;
    cpu.flags.pf = parityFlag(normal);
    cpu.esp = esp + 0x18;
    cpu.fpu.regs[(cpu.fpu.count + 1) & 7] = height;
    cpu.fpu.count += 1;
}

// ---- shadow mode ----

// NaN against NaN with another sign or payload is counted apart: the compilers may swap the operands of a
// commutative operation, in the generated code as here, which picks the NaN that propagates.
struct Shadow
{
    uint32_t calls[3] = {}, differing[3] = {};
    uint32_t logged = 0;
    uint32_t nanOnly = 0;
};

Shadow s_shadow;

inline bool floatNaN(reg32 v) { return (v & 0x7f800000u) == 0x7f800000u && (v & 0x7fffffu) != 0; }
inline bool nanPair(reg32 g, reg32 n)
{
    if (floatNaN(g) && floatNaN(n))
    {
        ++s_shadow.nanOnly;
        return true;
    }
    return false;
}
const uint32_t ShadowLogLimit = 20;
const char* const EntryNames[3] = {"elevation", "outside", "interest box"};

bool compareRegisters(const x86::CPU& guest, const x86::CPU& native, char* text, size_t size)
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
        if (r.g != r.n && !nanPair(r.g, r.n))
        {
            snprintf(text, size, "%s guest 0x%08x native 0x%08x", r.name, unsigned(r.g), unsigned(r.n));
            return false;
        }
    }
    for (int i = 0; i < 8; ++i)
    {
        const double g = guest.fpu.regs[i], n = native.fpu.regs[i];
        if (memcmp(&g, &n, sizeof(g)) != 0)
        {
            if (std::isnan(g) && std::isnan(n))
            {
                ++s_shadow.nanOnly;
                continue;
            }
            snprintf(text, size, "x87 register slot %d guest %.17g native %.17g", i, g, n);
            return false;
        }
    }
    return true;
}

struct Region
{
    reg32 address;
    reg32 size;
    std::vector<reg8> before;
    std::vector<bool> stored;
};

// Runs the native entry with its stores recorded and undone, then the guest code (whose result stays), and logs
// the first difference: registers, x87 state, every native store, and guest stores the native code did not make
// in the coordinate, the outputs and (when the cache was rebuilt) the polygon pool and list head.
template< typename Native >
void shadowRun(int entry, win32::WinApplication* app, x86::CPU& cpu, Native native,
               void (*guest)(win32::WinApplication*, x86::CPU&), const std::vector<reg32>& outputs, reg32 coord)
{
    Shadow& s = s_shadow;
    const x86::CPU start = cpu;
    Writer w;
    w.record = true;
    native(cpu, w);
    struct Store { reg32 address, size, value; };
    std::vector<Store> stores;
    bool rebuilt = false;
    for (const Writer::Undo& u : w.undo)
    {
        stores.push_back(Store{u.address, u.size, u.size == 4 ? rc::ld32(u.address) : u.size == 2
                                                                    ? rc::ld16(u.address) : rc::ld8(u.address)});
        rebuilt |= coord && u.address == coord + 0x2a0;
    }
    for (auto u = w.undo.rbegin(); u != w.undo.rend(); ++u)
    {
        if (u->size == 4)
            rc::st32(u->address, u->value);
        else if (u->size == 2)
            rc::st16(u->address, reg16(u->value));
        else
            rc::st8(u->address, reg8(u->value));
    }
    const x86::CPU result = cpu;

    std::vector<Region> regions;
    if (coord)
        regions.push_back(Region{coord, 0x2b4, {}, {}});
    for (reg32 o : outputs)
    {
        if (o)
            regions.push_back(Region{o, 12, {}, {}});
    }
    if (rebuilt)
    {
        regions.push_back(Region{TopologyTree + 0x50, 8, {}, {}});
        for (reg32 pool = rc::ld32(PolygonPool); pool; pool = rc::ld32(pool + 8))
            regions.push_back(Region{pool, 0x30 + rc::ld32(pool + 0x24) * rc::ld32(pool + 0x28), {}, {}});
    }
    for (Region& r : regions)
    {
        r.before.assign(rc::host(r.address), rc::host(r.address) + r.size);
        r.stored.assign(r.size, false);
    }

    cpu = start;
    guest(app, cpu);
    ++s.calls[entry];
    char text[200] = {};
    bool same = compareRegisters(cpu, result, text, sizeof(text));
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
        const reg32 g = st.size == 4 ? rc::ld32(st.address) : st.size == 2 ? rc::ld16(st.address)
                                                                            : rc::ld8(st.address);
        if (g != st.value && !(st.size == 4 && nanPair(g, st.value)))
        {
            same = false;
            snprintf(text, sizeof(text), "guest 0x%08x native 0x%08x at 0x%08x", unsigned(g), unsigned(st.value),
                     unsigned(st.address));
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
                snprintf(text, sizeof(text), "guest stored 0x%02x at 0x%08x, native nothing", unsigned(now),
                         unsigned(r.address + i));
            }
        }
    }
    if (!same)
    {
        ++s.differing[entry];
        if (s.logged < ShadowLogLimit)
        {
            ++s.logged;
            pf::logInfo("shadow_coll: %s call %u (caller 0x%08x): %s", EntryNames[entry],
                        unsigned(s.calls[entry]), unsigned(rc::ld32(start.esp)), text);
        }
    }
}

// ---- perf builds: time per second of game time, sampled on every 8th call ----

struct PerfSums
{
    uint32_t calls[3] = {};
    uint64_t sampledUs[3] = {};
    uint32_t sampled[3] = {};
    uint32_t start = 0;
    bool started = false;
};

PerfSums s_perf;
const uint32_t SampleEvery = 8;
const uint32_t ReportUs = 5000000;

void report(uint32_t now)
{
    PerfSums& p = s_perf;
    const uint32_t elapsed = now - p.start;
    if (elapsed < ReportUs)
        return;
    const Settings& s = settings();
    char line[400];
    int used = snprintf(line, sizeof(line), "coll: %s,", !s.native ? "guest" : s.shadow ? "shadow" : "native");
    double total = 0;
    for (int e = 0; e < 3; ++e)
    {
        // Mean time of the sampled calls times all calls.
        const double ms = p.sampled[e] ? double(p.sampledUs[e]) / p.sampled[e] * p.calls[e] / 1000.0 : 0.0;
        const double perSecond = 1000000.0 / elapsed;
        total += ms * perSecond;
        used += snprintf(line + used, sizeof(line) - size_t(used), " %s %.0f calls/s %.2f ms/s;", EntryNames[e],
                         p.calls[e] * perSecond, ms * perSecond);
    }
    snprintf(line + used, sizeof(line) - size_t(used), " total %.2f ms/s (ms/frame = this / fps)", total);
    pf::logInfo("%s", line);
    if (s.native && s.shadow)
        pf::logInfo("shadow_coll: elevation %u compared %u differ, outside %u/%u, interest box %u/%u; NaN payload "
                    "only %u", unsigned(s_shadow.calls[0]), unsigned(s_shadow.differing[0]),
                    unsigned(s_shadow.calls[1]), unsigned(s_shadow.differing[1]), unsigned(s_shadow.calls[2]),
                    unsigned(s_shadow.differing[2]), unsigned(s_shadow.nanOnly));
    p = PerfSums();
    p.start = now;
    p.started = true;
}

template< typename Body >
void timed(int entry, Body body)
{
    PerfSums& p = s_perf;
    if (++p.calls[entry] % SampleEvery != 0)
    {
        body();
        return;
    }
    const uint32_t start = pf::perf::now();
    body();
    const uint32_t end = pf::perf::now();
    p.sampledUs[entry] += end - start;
    ++p.sampled[entry];
    if (!p.started)
    {
        p.start = start;
        p.started = true;
    }
    report(end);
}

}

}

namespace native
{

using coll::settings;

// Ground height under a point (guest 0x440a40).
void sub_440a40(win32::WinApplication* app, x86::CPU& cpu)
{
    const auto body = [&]
    {
        const coll::Settings& s = settings();
        if (!s.native)
            sub_440a40_guest(app, cpu);
        else if (!s.shadow)
        {
            coll::Writer w;
            coll::elevationNative(cpu, w);
        }
        else
        {
            const x86::reg32 esp = cpu.esp;
            const std::vector<x86::reg32> outputs = {rc::ld32(esp + 0xc), rc::ld32(esp + 0x10), rc::ld32(esp + 0x14)};
            coll::shadowRun(0, app, cpu, coll::elevationNative, sub_440a40_guest, outputs, rc::ld32(esp + 4));
        }
    };
    if (pf::perf::enabled())
        coll::timed(0, body);
    else
        body();
}

// Distance of a point outside a track polygon (guest 0x4400e0).
void sub_4400e0(win32::WinApplication* app, x86::CPU& cpu)
{
    const auto body = [&]
    {
        const coll::Settings& s = settings();
        if (!s.native)
            sub_4400e0_guest(app, cpu);
        else if (!s.shadow)
            coll::amountOutsideNative(cpu);
        else
        {
            const auto native = [](x86::CPU& c, coll::Writer&) { coll::amountOutsideNative(c); };
            coll::shadowRun(1, app, cpu, native, sub_4400e0_guest, {}, 0);
        }
    };
    if (pf::perf::enabled())
        coll::timed(1, body);
    else
        body();
}

// Interest box and polygon cache of a topology coordinate (guest 0x4407b0).
void sub_4407b0(win32::WinApplication* app, x86::CPU& cpu)
{
    const auto body = [&]
    {
        const coll::Settings& s = settings();
        if (!s.native)
            sub_4407b0_guest(app, cpu);
        else if (!s.shadow)
        {
            coll::Writer w;
            coll::interestBoxNative(cpu, w);
        }
        else
            coll::shadowRun(2, app, cpu, coll::interestBoxNative, sub_4407b0_guest, {}, cpu.edi);
    };
    if (pf::perf::enabled())
        coll::timed(2, body);
    else
        body();
}

}
}
