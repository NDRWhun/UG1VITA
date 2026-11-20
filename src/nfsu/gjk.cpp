// Native GJK distance parts, bit-exact with the translated guest code: every x87 chain in double in the guest's
// order, floats where the guest stores floats, the guest's stores outside its own stack frame and the registers the
// callers read.
//
// 0x46EA50 (DistanceSqr of a simplex pair): esi = the pair, edx = the direction out. The pair holds up to four
// points of each shape (A at +0, B at +0x40, 16 bytes apart), their ids (+0xB0, +0xC4), counts (+0xC0, +0xD4) and
// the closest points (+0x80, +0x90). Every feature pair (point, edge, triangle of A against point, edge, triangle of
// B) is tried in the guest's order: its closest points and direction, then the support test (no point of B nearer
// along the direction than A's farthest). A pair that passes and whose support points are its own reduces the
// simplex to it and returns its squared distance; otherwise the nearest passing pair seen is written at the end,
// or 0 is returned when none passed. st0 = the squared distance; eax and edx as the guest leaves them.
//
// 0x472F30 (support point of a polytope, the function pointer at GJKPrimitive +8): the vertex of the polytope
// farthest along a direction, found by climbing from a start vertex over the vertex graph, or over all vertices when
// the climb ends without one, then transformed to world space into the output vertex. Each vertex caches its dot
// under a stamp; the stamp of a call comes from the tick counter and the game's random state, as in the guest.
//
// 0x473A90 (support point of a wafer, a track polygon with thickness): the vertex farthest along a world direction,
// copied into the output vertex.
//
// 0x471DE0 (GJK distance of a witness, edi): the simplex pair started from point 0 of both shapes or from the points
// it kept, grown by the support points along the pair's direction while its squared distance shrinks; the last
// pair that shrank it is kept. st0 = the distance, 0 when the shapes touch; edx as the guest leaves it. The guest's
// stack frame holds the vertices and directions the support functions read and write.

#include "gjk.h"
#include "phases.h"
#include "retstate.h"

#include <lib/guest.h>
#include <lib/winapp.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/time.h>
#include <winapi/d3dx9.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fpu.h>
#include <recomp.h>
#include <vector>

namespace nfsu
{

// The translated guest bodies of the overridden functions (generated).
void sub_46ea50_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_471de0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_472f30_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_473a90_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace gjk
{

namespace
{

using x86::reg16;
using x86::reg32;

// Float constants in .rdata.
const reg32 Zero = 0x6cc7a4;
const reg32 One = 0x6cc7bc;
const reg32 Tiny = 0x6ccb7c;            // 1e-5: smallest determinant of two edges

const reg32 FloatMax = 0x7f7fffff;

// The simplex pair.
const reg32 PointsB = 0x40;
const reg32 ClosestA = 0x80;
const reg32 ClosestB = 0x90;
const reg32 IdsA = 0xb0;
const reg32 CountA = 0xc0;
const reg32 IdsB = 0xc4;
const reg32 CountB = 0xd4;
const reg32 PairBytes = 0xdc;
const int MaxPoints = 4;

inline double ld(reg32 address) { return double(rc::ldf32(address)); }
inline double dd(float value) { return double(value); }
inline reg32 bits(float value) { reg32 r; memcpy(&r, &value, 4); return r; }
inline float fromBits(reg32 value) { float r; memcpy(&r, &value, 4); return r; }

struct Settings
{
    bool native;
    bool shadow;
};

const Settings& settings()
{
    static const Settings s = []
    {
        const Settings r = { pf::Config::getBool("gjk_native", true), pf::Config::getBool("shadow_gjk", false) };
        pf::logInfo("gjk: distance, simplex distance and support points %s%s", r.native ? "native" : "guest",
                    r.native && r.shadow ? ", shadow compare" : "");
        return r;
    }();
    return s;
}

struct Point
{
    float x, y, z;
};

// Registers the guest leaves at its return.
struct Result
{
    double distance;
    reg32 eax, edx;
};

class SimplexPair
{
public:
    SimplexPair(reg32 pair, reg32 direction, reg16 status)
        : m_pair(pair), m_direction(direction), m_status(status), m_zero(ld(Zero)), m_one(ld(One))
    {
    }

    // False when a count is outside 1..4 (the guest's unrolled loops would run).
    bool load()
    {
        m_countA = int(rc::ld32(m_pair + CountA));
        m_countB = int(rc::ld32(m_pair + CountB));
        if (m_countA < 1 || m_countA > MaxPoints || m_countB < 1 || m_countB > MaxPoints)
            return false;
        for (int i = 0; i < m_countA; ++i)
            m_a[i] = loadPoint(m_pair + 0x10 * reg32(i));
        for (int i = 0; i < m_countB; ++i)
            m_b[i] = loadPoint(m_pair + PointsB + 0x10 * reg32(i));
        return true;
    }

    Result run()
    {
        Result r;
        if (m_countA == 1)
        {
            if (m_countB == 1)
                return pointPoint();
            if (pointEdges(r))
                return r;
        }
        else if (m_countB == 1)
        {
            if (edgesPoint(r))
                return r;
        }
        else if (edgesEdges(r))
            return r;
        if (trianglesPoints(r))
            return r;
        if (pointsTriangles(r))
            return r;
        return nearest();
    }

private:
    static Point loadPoint(reg32 address)
    {
        return Point{rc::ldf32(address), rc::ldf32(address + 4), rc::ldf32(address + 8)};
    }

    reg32 pointA(int i) const { return m_pair + 0x10 * reg32(i); }
    reg32 pointB(int i) const { return m_pair + PointsB + 0x10 * reg32(i); }

    void setDirection(float x, float y, float z)
    {
        m_d[0] = x;
        m_d[1] = y;
        m_d[2] = z;
        rc::stf32(m_direction, x);
        rc::stf32(m_direction + 4, y);
        rc::stf32(m_direction + 8, z);
    }

    void setClosest(reg32 offset, float* mirror, float x, float y, float z)
    {
        mirror[0] = x;
        mirror[1] = y;
        mirror[2] = z;
        rc::stf32(m_pair + offset, x);
        rc::stf32(m_pair + offset + 4, y);
        rc::stf32(m_pair + offset + 8, z);
    }

    void setClosestA(float x, float y, float z) { setClosest(ClosestA, m_cA, x, y, z); }
    void setClosestB(float x, float y, float z) { setClosest(ClosestB, m_cB, x, y, z); }

    // Squared distance of the closest points: (dz^2 + dy^2) + dx^2.
    double gap() const
    {
        const double dx = dd(m_cB[0]) - dd(m_cA[0]);
        const double dy = dd(m_cB[1]) - dd(m_cA[1]);
        const double dz = dd(m_cB[2]) - dd(m_cA[2]);
        double r = dz * dz;
        r += dy * dy;
        r += dx * dx;
        return r;
    }

    // The dot of p with the direction as the guest's support loops add it: (x + z) + y.
    double dotLoop(const Point& p) const
    {
        double r = dd(p.x) * dd(m_d[0]);
        r += dd(p.z) * dd(m_d[2]);
        r += dd(p.y) * dd(m_d[1]);
        return r;
    }

    // The largest (smallest) dot of points 1..n-1 against the start value: the unrounded dot is compared, the
    // rounded one kept, as the guest's loops do.
    double scanMax(const Point* p, int n, double value, int& index) const
    {
        for (int k = 1; k < n; ++k)
        {
            const double dot = dotLoop(p[k]);
            if (dot > value)
            {
                value = dd(float(dot));
                index = k;
            }
        }
        return value;
    }

    double scanMin(const Point* p, int n, double value, int& index) const
    {
        for (int k = 1; k < n; ++k)
        {
            const double dot = dotLoop(p[k]);
            if (dot < value)
            {
                value = dd(float(dot));
                index = k;
            }
        }
        return value;
    }

    // Keeps the pair as the nearest so far.
    void record(double distance, int countA, const int* idsA, int countB, const int* idsB)
    {
        m_found = true;
        m_best = float(distance);
        memcpy(m_bestA, m_cA, sizeof(m_bestA));
        memcpy(m_bestB, m_cB, sizeof(m_bestB));
        memcpy(m_bestD, m_d, sizeof(m_bestD));
        m_bestCountA = countA;
        m_bestCountB = countB;
        for (int i = 0; i < countA; ++i)
            m_bestIdsA[i] = idsA[i];
        for (int i = 0; i < countB; ++i)
            m_bestIdsB[i] = idsB[i];
    }

    // Point and id of slot `from` of a shape to slot `to` (x as a dword, y and z through the x87).
    void copyPoint(reg32 points, reg32 ids, int to, int from)
    {
        rc::st32(m_pair + ids + 4 * reg32(to), rc::ld32(m_pair + ids + 4 * reg32(from)));
        const reg32 src = m_pair + points + 0x10 * reg32(from), dst = m_pair + points + 0x10 * reg32(to);
        rc::st32(dst, rc::ld32(src));
        rc::stf32(dst + 4, float(ld(src + 4)));
        rc::stf32(dst + 8, float(ld(src + 8)));
    }

    void keepA(int count, int i, int j = 0, int k = 0)
    {
        rc::st32(m_pair + CountA, reg32(count));
        const int from[3] = {i, j, k};
        for (int e = 0; e < count; ++e)
            copyPoint(0, IdsA, e, from[e]);
    }

    void keepB(int count, int i, int j = 0, int k = 0)
    {
        rc::st32(m_pair + CountB, reg32(count));
        const int from[3] = {i, j, k};
        for (int e = 0; e < count; ++e)
            copyPoint(PointsB, IdsB, e, from[e]);
    }

    Result done(double distance, reg32 eax, reg32 edx) const { return Result{distance, eax, edx}; }

    // nA = nB = 1 (0x46EA97).
    Result pointPoint()
    {
        const Point& a = m_a[0];
        const Point& b = m_b[0];
        setClosestA(a.x, a.y, a.z);
        setClosestB(b.x, b.y, b.z);
        const double dx = dd(m_cB[0]) - dd(m_cA[0]);
        const double dy = dd(m_cB[1]) - dd(m_cA[1]);
        const double dz = dd(m_cB[2]) - dd(m_cA[2]);
        const float fz = float(dz), fx = float(dx);
        setDirection(fx, float(dy), fz);
        double r = dd(fz) * dd(fz);
        r += dy * dy;
        r += dd(fx) * dd(fx);
        return done(r, bits(a.x), m_direction);
    }

    // nA = 1: A's point against each edge of B (0x46EB2E).
    bool pointEdges(Result& out)
    {
        const Point& p = m_a[0];
        for (int i = 0; i + 1 < m_countB; ++i)
        {
            const Point& bi = m_b[i];
            const float wz = float(dd(bi.z) - dd(p.z));
            const float wx = float(dd(bi.x) - dd(p.x));
            const float wy = float(dd(bi.y) - dd(p.y));
            for (int j = i + 1; j < m_countB; ++j)
            {
                const Point& bj = m_b[j];
                const float ex = float(dd(bj.x) - dd(bi.x));
                const float ey = float(dd(bj.y) - dd(bi.y));
                const float ez = float(dd(bj.z) - dd(bi.z));
                double num = dd(ex) * dd(wx);
                num += dd(ez) * dd(wz);
                num += dd(ey) * dd(wy);
                double den = dd(ex) * dd(ex);
                den += dd(ez) * dd(ez);
                den += dd(ey) * dd(ey);
                const double t = -num / den;
                float ft = float(t);
                if (t < m_zero)
                {
                    ft = 0.0f;
                    setDirection(float(dd(bi.x) - dd(p.x)), float(dd(bi.y) - dd(p.y)), float(dd(bi.z) - dd(p.z)));
                }
                else if (dd(ft) > m_one)
                {
                    ft = 1.0f;
                    setDirection(float(dd(bj.x) - dd(p.x)), float(dd(bj.y) - dd(p.y)), float(dd(bj.z) - dd(p.z)));
                }
                else
                {
                    // The edge's perpendicular through the point: e x (w x e).
                    const float c1 = float(dd(wy) * dd(ez) - dd(ey) * dd(wz));
                    const double c2 = dd(ex) * dd(wz) - dd(wx) * dd(ez);
                    const float c3 = float(dd(wx) * dd(ey) - dd(ex) * dd(wy));
                    const double x = dd(c1) * dd(ez) - dd(ex) * dd(c3);
                    const double z = dd(ex) * c2 - dd(c1) * dd(ey);
                    const double y = dd(c3) * dd(ey) - c2 * dd(ez);
                    setDirection(float(y), float(x), float(z));
                }
                double dotA = dd(m_d[0]) * dd(p.x);
                dotA += dd(m_d[2]) * dd(p.z);
                dotA += dd(m_d[1]) * dd(p.y);
                double minB = dd(m_b[0].y) * dd(m_d[1]);
                minB += dd(m_d[0]) * dd(m_b[0].x);
                minB += dd(m_b[0].z) * dd(m_d[2]);
                int minIndex = 0;
                minB = scanMin(m_b, m_countB, minB, minIndex);
                if (!(minB > dotA))
                    continue;
                setClosestA(p.x, p.y, p.z);
                const float tx = float(dd(ex) * dd(ft)), ty = float(dd(ey) * dd(ft)), tz = float(dd(ez) * dd(ft));
                setClosestB(float(dd(tx) + dd(bi.x)), float(dd(bi.y) + dd(ty)), float(dd(bi.z) + dd(tz)));
                const double distance = gap();
                if (minIndex == i || minIndex == j)
                {
                    // 0x46F28E: B reduced to the edge or one of its ends.
                    if (dd(ft) == m_zero)
                    {
                        keepB(1, i);
                        out = done(distance, pointB(i), bits(bi.x));
                    }
                    else if (dd(ft) == m_one)
                    {
                        keepB(1, j);
                        out = done(distance, pointB(j), m_direction);
                    }
                    else
                    {
                        keepB(2, i, j);
                        out = done(distance, bits(bj.x), bits(bi.x));
                    }
                    return true;
                }
                if (!(distance < dd(m_best)))
                    continue;
                const int idsA[1] = {0};
                if (dd(ft) == m_zero)
                {
                    const int idsB[1] = {i};
                    record(distance, 1, idsA, 1, idsB);
                }
                else if (dd(ft) == m_one)
                {
                    const int idsB[1] = {j};
                    record(distance, 1, idsA, 1, idsB);
                }
                else
                {
                    const int idsB[2] = {i, j};
                    record(distance, 1, idsA, 2, idsB);
                }
            }
        }
        return false;
    }

    // nB = 1: each edge of A against B's point (0x46F354).
    bool edgesPoint(Result& out)
    {
        const Point& q = m_b[0];
        for (int i = 0; i + 1 < m_countA; ++i)
        {
            const Point& ai = m_a[i];
            const float wz = float(dd(q.z) - dd(ai.z));
            const float wx = float(dd(q.x) - dd(ai.x));
            const float wy = float(dd(q.y) - dd(ai.y));
            for (int j = i + 1; j < m_countA; ++j)
            {
                const Point& aj = m_a[j];
                const float ex = float(dd(aj.x) - dd(ai.x));
                const float ey = float(dd(aj.y) - dd(ai.y));
                const float ez = float(dd(aj.z) - dd(ai.z));
                double num = dd(wx) * dd(ex);
                num += dd(ez) * dd(wz);
                num += dd(ey) * dd(wy);
                double den = dd(ez) * dd(ez);
                den += dd(ey) * dd(ey);
                den += dd(ex) * dd(ex);
                const double t = num / den;
                float ft = float(t);
                if (t < m_zero)
                {
                    ft = 0.0f;
                    setDirection(float(dd(q.x) - dd(ai.x)), float(dd(q.y) - dd(ai.y)), float(dd(q.z) - dd(ai.z)));
                }
                else if (dd(ft) > m_one)
                {
                    ft = 1.0f;
                    setDirection(float(dd(q.x) - dd(aj.x)), float(dd(q.y) - dd(aj.y)), float(dd(q.z) - dd(aj.z)));
                }
                else
                {
                    const float c1 = float(dd(wy) * dd(ez) - dd(ey) * dd(wz));
                    const double c2 = dd(wz) * dd(ex) - dd(wx) * dd(ez);
                    const float c3 = float(dd(wx) * dd(ey) - dd(wy) * dd(ex));
                    const double x = dd(c1) * dd(ez) - dd(c3) * dd(ex);
                    const double z = c2 * dd(ex) - dd(c1) * dd(ey);
                    const double y = dd(c3) * dd(ey) - c2 * dd(ez);
                    setDirection(float(y), float(x), float(z));
                }
                double maxA = dd(m_d[0]) * dd(m_a[0].x);
                maxA += dd(m_d[2]) * dd(m_a[0].z);
                maxA += dd(m_d[1]) * dd(m_a[0].y);
                int maxIndex = 0;
                maxA = scanMax(m_a, m_countA, maxA, maxIndex);
                double dotB = dd(q.z) * dd(m_d[2]);
                dotB += dd(q.y) * dd(m_d[1]);
                dotB += dd(q.x) * dd(m_d[0]);
                if (!(dotB > maxA))
                    continue;
                const float tx = float(dd(ft) * dd(ex)), ty = float(dd(ft) * dd(ey)), tz = float(dd(ft) * dd(ez));
                setClosestA(float(dd(tx) + dd(ai.x)), float(dd(ai.y) + dd(ty)), float(dd(ai.z) + dd(tz)));
                setClosestB(q.x, q.y, q.z);
                const double distance = gap();
                if (maxIndex == i || maxIndex == j)
                {
                    // 0x46F8D5: A reduced to the edge or one of its ends.
                    if (dd(ft) == m_zero)
                    {
                        keepA(1, i);
                        // eax: the status word of the compare (equal), over the zero upper half of the index.
                        const reg32 status = reg32((m_status & ~retstate::ConditionCodes) | 0x4000u);
                        out = done(distance, status, bits(ai.x));
                    }
                    else if (dd(ft) == m_one)
                    {
                        const reg32 id = rc::ld32(m_pair + IdsA + 4 * reg32(j));
                        keepA(1, j);
                        out = done(distance, id, m_direction);
                    }
                    else
                    {
                        const reg32 id = rc::ld32(m_pair + IdsA + 4 * reg32(j));
                        keepA(2, i, j);
                        out = done(distance, bits(aj.x), id);
                    }
                    return true;
                }
                if (!(distance < dd(m_best)))
                    continue;
                const int idsB[1] = {0};
                if (dd(ft) == m_zero)
                {
                    const int idsA[1] = {i};
                    record(distance, 1, idsA, 1, idsB);
                }
                else if (dd(ft) == m_one)
                {
                    const int idsA[1] = {j};
                    record(distance, 1, idsA, 1, idsB);
                }
                else
                {
                    const int idsA[2] = {i, j};
                    record(distance, 2, idsA, 1, idsB);
                }
            }
        }
        return false;
    }

    // The normalised direction (0x567370 on [edx]): (1, 0, 0) for length 0.
    void normaliseDirection()
    {
        const double x = dd(m_d[0]), y = dd(m_d[1]), z = dd(m_d[2]);
        double length = x * x;
        length += y * y;
        length += z * z;
        length = std::sqrt(length);
        if (length == m_zero)
        {
            setDirection(1.0f, 0.0f, 0.0f);
            return;
        }
        const double inverse = m_one / length;
        setDirection(float(inverse * x), float(y * inverse), float(z * inverse));
    }

    // nA, nB > 1: each edge of A against each edge of B (0x46F9BE).
    bool edgesEdges(Result& out)
    {
        const double tiny = ld(Tiny);
        for (int i = 0; i + 1 < m_countA; ++i)
        {
            const Point& ai = m_a[i];
            for (int j = i + 1; j < m_countA; ++j)
            {
                const Point& aj = m_a[j];
                const float ex = float(dd(aj.x) - dd(ai.x));
                const float ey = float(dd(aj.y) - dd(ai.y));
                const float ez = float(dd(aj.z) - dd(ai.z));
                double eaa = dd(ez) * dd(ez);
                eaa += dd(ey) * dd(ey);
                eaa += dd(ex) * dd(ex);
                const float eAA = float(eaa);
                for (int k = 0; k + 1 < m_countB; ++k)
                {
                    const Point& bk = m_b[k];
                    const float wx = float(dd(bk.x) - dd(ai.x));
                    const float wy = float(dd(bk.y) - dd(ai.y));
                    const float wz = float(dd(bk.z) - dd(ai.z));
                    double eaw = dd(wx) * dd(ex);
                    eaw += dd(ez) * dd(wz);
                    eaw += dd(ey) * dd(wy);
                    const float eAw = float(eaw);
                    for (int l = k + 1; l < m_countB; ++l)
                    {
                        const Point& bl = m_b[l];
                        if (edgePair(out, ai, aj, bk, bl, ex, ey, ez, eAA, wx, wy, wz, eAw, tiny, i, j, k, l))
                            return true;
                    }
                }
            }
        }
        return false;
    }

    // One edge pair (0x46FB08 to 0x47094C): true when the simplex was reduced to it.
    bool edgePair(Result& out, const Point& ai, const Point& aj, const Point& bk, const Point& bl, float ex, float ey,
                  float ez, float eAA, float wx, float wy, float wz, float eAw, double tiny, int i, int j, int k, int l)
    {
        const float bx = float(dd(bl.x) - dd(bk.x));
        const float by = float(dd(bl.y) - dd(bk.y));
        const float bz = float(dd(bl.z) - dd(bk.z));
        double eBB = dd(bx) * dd(bx);
        eBB += dd(bz) * dd(bz);
        eBB += dd(by) * dd(by);
        double eab = dd(bx) * dd(ex);
        eab += dd(ez) * dd(bz);
        eab += dd(ey) * dd(by);
        const float eAB = float(eab);
        double eBw = dd(bx) * dd(wx);
        eBw += dd(bz) * dd(wz);
        eBw += dd(by) * dd(wy);
        const double det = dd(eAA) * eBB - dd(eAB) * dd(eAB);
        float fs = 0.0f;
        if (std::fabs(det) > tiny)
        {
            fs = float((dd(eAw) * eBB - dd(eAB) * eBw) / det);
            if (dd(fs) < m_zero)
                fs = 0.0f;
            else if (dd(fs) > m_one)
                fs = 1.0f;
        }
        float fu = float((dd(eAB) * dd(fs) - eBw) / eBB);
        if (dd(fu) > m_zero && dd(fu) < m_one)
        {
            if (dd(fs) > m_zero && dd(fs) < m_one)
                interiorDirection(ai, aj, bk, bl, ex, ey, ez, wx, wy, wz, bx, by, bz, fs, fu);
            else
            {
                // 0x4700D9: the perpendicular from A's end to edge B.
                const float tx = float(dd(fs) * dd(ex)), tz = float(dd(fs) * dd(ez));
                const float px = float(dd(tx) + dd(ai.x));
                const float py = float(dd(ai.y) + dd(fs) * dd(ey));
                const float pz = float(dd(ai.z) + dd(tz));
                const float qx = float(dd(bk.x) - dd(px));
                const double qy = dd(bk.y) - dd(py);
                const float qz = float(dd(bk.z) - dd(pz));
                const float r1 = float(qy * dd(bz) - dd(qz) * dd(by));
                const float r2 = float(dd(bx) * dd(qz) - dd(qx) * dd(bz));
                const double r3 = dd(qx) * dd(by) - dd(bx) * qy;
                const double y = dd(r1) * dd(bz) - r3 * dd(bx);
                const double z = dd(r2) * dd(bx) - dd(r1) * dd(by);
                const double x = r3 * dd(by) - dd(r2) * dd(bz);
                setDirection(float(x), float(y), float(z));
            }
        }
        else
        {
            // 0x4701E7: B at an end.
            fu = dd(fu) <= m_zero ? 0.0f : 1.0f;
            const double s = (dd(eAB) * dd(fu) + dd(eAw)) / dd(eAA);
            fs = float(s);
            if (s > m_zero && dd(fs) < m_one)
            {
                // 0x470248: the perpendicular from B's end to edge A.
                const double pbx = dd(bx) * dd(fu) + dd(bk.x);
                const double pby = dd(by) * dd(fu) + dd(bk.y);
                const float pbz = float(dd(bz) * dd(fu) + dd(bk.z));
                const float mx = float(pbx - dd(ai.x));
                const double my = pby - dd(ai.y);
                const double mz = dd(pbz) - dd(ai.z);
                const float s1 = float(my * dd(ez) - mz * dd(ey));
                const float s2 = float(mz * dd(ex) - dd(mx) * dd(ez));
                const double s3 = dd(mx) * dd(ey) - my * dd(ex);
                const double y = dd(ez) * dd(s1) - s3 * dd(ex);
                const double z = dd(s2) * dd(ex) - dd(ey) * dd(s1);
                const double x = dd(ey) * s3 - dd(ez) * dd(s2);
                setDirection(float(x), float(y), float(z));
            }
            else
            {
                // 0x470748: both at an end.
                fs = dd(fs) <= m_zero ? 0.0f : 1.0f;
                const double pax = dd(fs) * dd(ex) + dd(ai.x);
                const double pay = dd(fs) * dd(ey) + dd(ai.y);
                const float paz = float(dd(fs) * dd(ez) + dd(ai.z));
                const double pbx = dd(bx) * dd(fu) + dd(bk.x);
                const float pby = float(dd(by) * dd(fu) + dd(bk.y));
                const float pbz = float(dd(bz) * dd(fu) + dd(bk.z));
                setDirection(float(pbx - pax), float(dd(pby) - pay), float(dd(pbz) - dd(paz)));
            }
        }

        // 0x4702FF: the support test.
        double maxA = dd(m_d[0]) * dd(m_a[0].x);
        maxA += dd(m_d[2]) * dd(m_a[0].z);
        maxA += dd(m_d[1]) * dd(m_a[0].y);
        int maxIndex = 0;
        maxA = scanMax(m_a, m_countA, maxA, maxIndex);
        double minB = dd(m_b[0].y) * dd(m_d[1]);
        minB += dd(m_b[0].x) * dd(m_d[0]);
        minB += dd(m_b[0].z) * dd(m_d[2]);
        int minIndex = 0;
        minB = scanMin(m_b, m_countB, minB, minIndex);
        if (!(minB > maxA))
            return false;
        const float ax = float(dd(fs) * dd(ex)), ay = float(dd(fs) * dd(ey)), az = float(dd(fs) * dd(ez));
        setClosestA(float(dd(ax) + dd(ai.x)), float(dd(ai.y) + dd(ay)), float(dd(ai.z) + dd(az)));
        const float ux = float(dd(bx) * dd(fu)), uy = float(dd(by) * dd(fu)), uz = float(dd(bz) * dd(fu));
        setClosestB(float(dd(ux) + dd(bk.x)), float(dd(bk.y) + dd(uy)), float(dd(bk.z) + dd(uz)));
        const double distance = gap();
        if ((maxIndex == i || maxIndex == j) && (minIndex == k || minIndex == l))
        {
            // 0x4706FB: both reduced to their edges or ends.
            if (dd(fs) == m_zero)
                keepA(1, i);
            else if (dd(fs) == m_one)
                keepA(1, j);
            else
                keepA(2, i, j);
            if (dd(fu) == m_zero)
            {
                keepB(1, k);
                out = done(distance, pointB(k), reg32(k));
            }
            else if (dd(fu) == m_one)
            {
                const reg32 id = rc::ld32(m_pair + IdsB + 4 * reg32(l));
                keepB(1, l);
                out = done(distance, bits(bl.x), id);
            }
            else
            {
                keepB(2, k, l);
                out = done(distance, pointB(k), bits(bl.x));
            }
            return true;
        }
        if (!(distance < dd(m_best)))
            return false;
        int idsA[2], idsB[2];
        int countA = 1, countB = 1;
        if (dd(fs) == m_zero)
            idsA[0] = i;
        else if (dd(fs) == m_one)
            idsA[0] = j;
        else
        {
            countA = 2;
            idsA[0] = i;
            idsA[1] = j;
        }
        if (dd(fu) == m_zero)
            idsB[0] = k;
        else if (dd(fu) == m_one)
            idsB[0] = l;
        else
        {
            countB = 2;
            idsB[0] = k;
            idsB[1] = l;
        }
        record(distance, countA, idsA, countB, idsB);
        return false;
    }

    // 0x46FC88: both edges at inner points. The edges' common normal (normalised, towards B), then the line
    // between the points and the perpendicular from B's point to edge A where they separate the shapes better.
    void interiorDirection(const Point& ai, const Point& aj, const Point& bk, const Point& bl, float ex, float ey,
                           float ez, float wx, float wy, float wz, float bx, float by, float bz, float fs, float fu)
    {
        setDirection(float(dd(ey) * dd(bz) - dd(ez) * dd(by)), float(dd(bx) * dd(ez) - dd(bz) * dd(ex)),
                     float(dd(by) * dd(ex) - dd(bx) * dd(ey)));
        normaliseDirection();
        double side = dd(wy) * dd(m_d[1]);
        side += dd(wz) * dd(m_d[2]);
        side += dd(wx) * dd(m_d[0]);
        if (side < m_zero)
            setDirection(float(-dd(m_d[0])), float(-dd(m_d[1])), float(-dd(m_d[2])));

        // The normal of edge B in the plane of B and A's point: n = (q x eB) x eB.
        const float tx = float(dd(fs) * dd(ex)), tz = float(dd(fs) * dd(ez));
        const float px = float(dd(tx) + dd(ai.x));
        const float py = float(dd(ai.y) + dd(fs) * dd(ey));
        const float pz = float(dd(ai.z) + dd(tz));
        const float qx = float(dd(bk.x) - dd(px));
        const double qy = dd(bk.y) - dd(py);
        const float qz = float(dd(bk.z) - dd(pz));
        const float r1 = float(qy * dd(bz) - dd(qz) * dd(by));
        const double r2 = dd(qz) * dd(bx) - dd(qx) * dd(bz);
        const double r3 = dd(qx) * dd(by) - dd(bx) * qy;
        const float ny = float(dd(r1) * dd(bz) - dd(bx) * r3);
        const float nz = float(dd(bx) * r2 - dd(r1) * dd(by));
        const float nx = float(r3 * dd(by) - r2 * dd(bz));

        // The normal of edge A in the plane of A and B's point: k = (m x eA) x eA.
        const double pbx = dd(bx) * dd(fu) + dd(bk.x);
        const double pby = dd(by) * dd(fu) + dd(bk.y);
        const float pbz = float(dd(bz) * dd(fu) + dd(bk.z));
        const float mx = float(pbx - dd(ai.x));
        const double my = pby - dd(ai.y);
        const double mz = dd(pbz) - dd(ai.z);
        const float s1 = float(my * dd(ez) - mz * dd(ey));
        const double s2 = mz * dd(ex) - dd(mx) * dd(ez);
        const double s3 = dd(mx) * dd(ey) - my * dd(ex);
        const float ky = float(dd(s1) * dd(ez) - s3 * dd(ex));
        const float kz = float(s2 * dd(ex) - dd(s1) * dd(ey));
        const double kx = s3 * dd(ey) - s2 * dd(ez);

        // The separation along a direction: B's nearer end less A's nearer end (the second end's dot compared
        // unrounded, kept rounded).
        const auto nearer = [](double first, double second)
        {
            return second < first ? dd(float(second)) : first;
        };
        const auto dot = [](const Point& p, double x, double y, double z)
        {
            double r = dd(p.z) * z;
            r += dd(p.y) * y;
            r += dd(p.x) * x;
            return r;
        };
        const auto separation = [&](double x, double y, double z)
        {
            const double nearA = nearer(dot(aj, x, y, z), dot(ai, x, y, z));
            const double nearB = nearer(dot(bl, x, y, z), dot(bk, x, y, z));
            return nearB - nearA;
        };
        double best = separation(dd(m_d[0]), dd(m_d[1]), dd(m_d[2]));
        const double alongN = separation(dd(nx), dd(ny), dd(nz));
        if (alongN > best)
        {
            best = dd(float(alongN));
            setDirection(nx, ny, nz);
        }
        const double alongK = separation(kx, dd(ky), dd(kz));
        if (alongK > best)
            setDirection(float(kx), ky, kz);
    }

    // The triangle's unit normal (1, 0, 0 for a degenerate one): nx and ny rounded before the length, nz after it.
    void triangleNormal(const Point& p0, const Point& p1, const Point& p2, float* n) const
    {
        const float e1x = float(dd(p1.x) - dd(p0.x));
        const double e1y = dd(p1.y) - dd(p0.y);
        const float e1z = float(dd(p1.z) - dd(p0.z));
        const float e2x = float(dd(p2.x) - dd(p0.x));
        const float e2y = float(dd(p2.y) - dd(p0.y));
        const float e2z = float(dd(p2.z) - dd(p0.z));
        float nx = float(dd(e2z) * e1y - dd(e2y) * dd(e1z));
        float ny = float(dd(e2x) * dd(e1z) - dd(e1x) * dd(e2z));
        const double nz = dd(e1x) * dd(e2y) - dd(e2x) * e1y;
        double length = dd(nx) * dd(nx);
        length += nz * nz;
        length += dd(ny) * dd(ny);
        length = std::sqrt(length);
        if (length == m_zero)
        {
            n[0] = 1.0f;
            n[1] = 0.0f;
            n[2] = 0.0f;
            return;
        }
        const double inverse = m_one / length;
        n[0] = float(dd(nx) * inverse);
        n[1] = float(dd(ny) * inverse);
        n[2] = float(inverse * nz);
    }

    // Whether p lies on the inner side of the edge from-to of a triangle with normal n (0x470D9A).
    static bool insideEdge(const Point& from, const Point& to, const Point& p, const float* n)
    {
        const float vz = float(dd(p.z) - dd(from.z));
        const float vx = float(dd(p.x) - dd(from.x));
        const double vy = dd(p.y) - dd(from.y);
        const float uz = float(dd(to.z) - dd(from.z));
        const float ux = float(dd(to.x) - dd(from.x));
        const double uy = dd(to.y) - dd(from.y);
        const float w1 = float(dd(n[1]) * dd(uz) - uy * dd(n[2]));
        const double w2 = dd(ux) * dd(n[2]) - dd(n[0]) * dd(uz);
        const double w3 = dd(n[0]) * uy - dd(ux) * dd(n[1]);
        double c = dd(w1) * dd(vx);
        c += w3 * dd(vz);
        c += w2 * vy;
        return c > 0.0;
    }

    // nA >= 3: each triangle of A against B's points (0x46F0CF).
    bool trianglesPoints(Result& out)
    {
        for (int i = 0; i + 2 < m_countA; ++i)
        {
            for (int j = i + 1; j + 1 < m_countA; ++j)
            {
                for (int k = j + 1; k < m_countA; ++k)
                {
                    const Point& ai = m_a[i];
                    float n[3];
                    triangleNormal(ai, m_a[j], m_a[k], n);
                    const double sx = dd(m_b[0].x) - dd(ai.x);
                    const double sy = dd(m_b[0].y) - dd(ai.y);
                    const double sz = dd(m_b[0].z) - dd(ai.z);
                    double side = sx * dd(n[0]);
                    side += sz * dd(n[2]);
                    side += sy * dd(n[1]);
                    if (side < m_zero)
                        setDirection(float(-dd(n[0])), float(-dd(n[1])), float(-dd(n[2])));
                    else
                        setDirection(n[0], n[1], n[2]);
                    double planeA = dd(ai.z) * dd(m_d[2]);
                    planeA += dd(ai.y) * dd(m_d[1]);
                    planeA += dd(ai.x) * dd(m_d[0]);
                    const float plane = float(planeA);
                    float maxA = plane;
                    int maxIndex = i;
                    const int other = 6 - i - j - k;
                    if (other < m_countA)
                    {
                        const double dot = dotLoop(m_a[other]);
                        if (dot > dd(plane))
                        {
                            maxA = float(dot);
                            maxIndex = other;
                        }
                    }
                    double minB = dd(m_b[0].y) * dd(m_d[1]);
                    minB += dd(m_b[0].z) * dd(m_d[2]);
                    minB += dd(m_b[0].x) * dd(m_d[0]);
                    int minIndex = 0;
                    minB = scanMin(m_b, m_countB, minB, minIndex);
                    if (!(minB > dd(maxA)))
                        continue;
                    const Point& p = m_b[minIndex];
                    if (!insideEdge(ai, m_a[j], p, n) || !insideEdge(m_a[j], m_a[k], p, n)
                        || !insideEdge(m_a[k], ai, p, n))
                        continue;
                    // B's point and its projection on the triangle's plane.
                    const double g = minB - dd(plane);
                    const float gx = float(g * dd(m_d[0])), gy = float(g * dd(m_d[1])), gz = float(g * dd(m_d[2]));
                    setClosestA(float(dd(p.x) - dd(gx)), float(dd(p.y) - dd(gy)), float(dd(p.z) - dd(gz)));
                    setClosestB(p.x, p.y, p.z);
                    double distance = dd(gz) * dd(gz);
                    distance += dd(gy) * dd(gy);
                    distance += dd(gx) * dd(gx);
                    if (maxIndex == i)
                    {
                        // 0x4712BE: A reduced to the triangle, B to the point.
                        keepA(3, i, j, k);
                        keepB(1, minIndex);
                        out = done(distance, pointB(minIndex), bits(p.x));
                        return true;
                    }
                    if (!(distance < dd(m_best)))
                        continue;
                    const int idsA[3] = {i, j, k}, idsB[1] = {minIndex};
                    record(distance, 3, idsA, 1, idsB);
                }
            }
        }
        return false;
    }

    // nB >= 3: A's points against each triangle of B (0x47120A).
    bool pointsTriangles(Result& out)
    {
        for (int i = 0; i + 2 < m_countB; ++i)
        {
            for (int j = i + 1; j + 1 < m_countB; ++j)
            {
                for (int k = j + 1; k < m_countB; ++k)
                {
                    const Point& bi = m_b[i];
                    float n[3];
                    triangleNormal(bi, m_b[j], m_b[k], n);
                    const double sx = dd(bi.x) - dd(m_a[0].x);
                    const double sy = dd(bi.y) - dd(m_a[0].y);
                    const double sz = dd(bi.z) - dd(m_a[0].z);
                    double side = dd(n[2]) * sz;
                    side += sy * dd(n[1]);
                    side += sx * dd(n[0]);
                    if (side < m_zero)
                        setDirection(float(-dd(n[0])), float(-dd(n[1])), float(-dd(n[2])));
                    else
                        setDirection(n[0], n[1], n[2]);
                    double planeB = dd(bi.x) * dd(m_d[0]);
                    planeB += dd(bi.y) * dd(m_d[1]);
                    planeB += dd(m_d[2]) * dd(bi.z);
                    const float plane = float(planeB);
                    float minB = plane;
                    int minIndex = i;
                    const int other = 6 - i - j - k;
                    if (other < m_countB)
                    {
                        const double dot = dotLoop(m_b[other]);
                        if (dot < dd(plane))
                        {
                            minB = float(dot);
                            minIndex = other;
                        }
                    }
                    double maxA = dd(m_d[0]) * dd(m_a[0].x);
                    maxA += dd(m_d[2]) * dd(m_a[0].z);
                    maxA += dd(m_d[1]) * dd(m_a[0].y);
                    int maxIndex = 0;
                    maxA = scanMax(m_a, m_countA, maxA, maxIndex);
                    if (!(dd(minB) > maxA))
                        continue;
                    const Point& p = m_a[maxIndex];
                    if (!insideEdge(bi, m_b[j], p, n) || !insideEdge(m_b[j], m_b[k], p, n)
                        || !insideEdge(m_b[k], bi, p, n))
                        continue;
                    // A's point and its projection on the triangle's plane.
                    const double g = dd(plane) - maxA;
                    const float gx = float(g * dd(m_d[0])), gy = float(g * dd(m_d[1])), gz = float(g * dd(m_d[2]));
                    setClosestA(p.x, p.y, p.z);
                    setClosestB(float(dd(gx) + dd(p.x)), float(dd(p.y) + dd(gy)), float(dd(p.z) + dd(gz)));
                    double distance = dd(gz) * dd(gz);
                    distance += dd(gy) * dd(gy);
                    distance += dd(gx) * dd(gx);
                    if (minIndex == i)
                    {
                        // 0x471C7B: A reduced to the point, B to the triangle.
                        keepA(1, maxIndex);
                        keepB(3, i, j, k);
                        out = done(distance, pointB(k), bits(m_b[k].x));
                        return true;
                    }
                    if (!(distance < dd(m_best)))
                        continue;
                    const int idsA[1] = {maxIndex}, idsB[3] = {i, j, k};
                    record(distance, 1, idsA, 3, idsB);
                }
            }
        }
        return false;
    }

    // 0x471B2F: the nearest passing pair, or 0 when none passed.
    Result nearest()
    {
        if (!m_found)
            return done(m_zero, 0, m_direction);
        setClosestA(m_bestA[0], m_bestA[1], m_bestA[2]);
        setClosestB(m_bestB[0], m_bestB[1], m_bestB[2]);
        setDirection(m_bestD[0], m_bestD[1], m_bestD[2]);
        keepA(m_bestCountA, m_bestIdsA[0], m_bestIdsA[1], m_bestIdsA[2]);
        keepB(m_bestCountB, m_bestIdsB[0], m_bestIdsB[1], m_bestIdsB[2]);
        return done(dd(m_best), reg32(m_bestCountB), reg32(m_bestCountB));
    }

    const reg32 m_pair, m_direction;
    const reg16 m_status;
    const double m_zero, m_one;
    int m_countA = 0, m_countB = 0;
    Point m_a[MaxPoints], m_b[MaxPoints];
    float m_d[3] = {};
    float m_cA[3] = {}, m_cB[3] = {};
    bool m_found = false;
    float m_best = fromBits(FloatMax);
    float m_bestA[3] = {}, m_bestB[3] = {}, m_bestD[3] = {};
    int m_bestCountA = 0, m_bestCountB = 0;
    int m_bestIdsA[3] = {}, m_bestIdsB[3] = {};
};

// 0x46EA50: false when the guest body has to run.
bool distanceNative(x86::CPU& cpu)
{
    SimplexPair pair(cpu.esi, cpu.edx, cpu.fpu.status.word);
    if (!pair.load())
        return false;
    const Result r = pair.run();
    cpu.eax = r.eax;
    cpu.edx = r.edx;
    cpu.esp += 4;
    cpu.fpu.regs[(cpu.fpu.count + 1) & 7] = r.distance;
    cpu.fpu.count += 1;
    return true;
}

// ---- support point of a polytope (0x472F30) ----

// Guest data.
const reg32 RandomSeed = 0x6f227c;
const reg32 TickShift = 0x7349c8;       // the tick counter's shift to the stamp's unit
const reg32 QueueLater = 0x735fd4;      // nonzero: the climb queues larger neighbours of every vertex
const reg32 Lowest = 0x6ccc18;          // -9.16e36: start of the search over all vertices
const reg32 LowestBits = 0xfcdc84dcu;

// Polytope: vertex count, neighbour index list (int16), vertices (0x30 bytes each: queue links at +0 and +4,
// neighbour count +8 and first neighbour +0xA (int16), id +0xC, stamp +0xE, position +0x10, dot +0x20).
const reg32 PolyCount = 0x14;
const reg32 PolyNeighbours = 0x50;
const reg32 PolyVertices = 0x54;
const reg32 VertexBytes = 0x30;
const int ClimbQueueAbove = 16;         // the first vertex queues larger neighbours with more vertices than this

struct SupportResult
{
    reg32 ecx, edx;
};

class PolytopeSupport
{
public:
    PolytopeSupport(reg32 polytope, reg32 head) : m_polytope(polytope), m_head(head), m_next(head), m_previous(head)
    {
    }

    // The stamp of the call: the tick counter shifted, plus the random state before its step.
    static reg16 stamp(const reg16* forced)
    {
        const reg32 seed = rc::ld32(RandomSeed);
        const reg32 offset = seed % 0xffffu;
        const reg32 c = seed ^ 0x1d872b41u;
        const reg32 a = (c >> 5) ^ c;
        rc::st32(RandomSeed, (a << 27) ^ a ^ c);
        if (forced)
            return *forced;
        const int64_t ticks = int64_t(pf::ticksUs());
        const reg32 shift = rc::ld32(TickShift) & 0xff;
        const reg32 low = reg32(uint64_t(ticks >> (shift < 64 ? shift : 63)));
        return reg16(low + offset);
    }

    // The polytope's farthest vertex along d from start, 0 when there is none; ecx as the search leaves it.
    reg32 run(reg32 start, reg16 stamp, const float* d, reg32& ecx)
    {
        m_stamp = stamp;
        m_d[0] = dd(d[0]);
        m_d[1] = dd(d[1]);
        m_d[2] = dd(d[2]);
        m_vertices = rc::ld32(m_polytope + PolyVertices);
        m_neighbours = rc::ld32(m_polytope + PolyNeighbours);
        const x86::sreg32 count = x86::sreg32(rc::ld32(m_polytope + PolyCount));
        bool queue = count > ClimbQueueAbove;
        push(vertex(x86::sreg32(int16_t(rc::ld16(start + 0xc)))));
        while (m_next != m_head)
        {
            const reg32 v = pop();
            refresh(v);
            const float best = rc::ldf32(v + 0x20);
            const int n = int16_t(rc::ld16(v + 8));
            if (n == 0)
                continue;
            const int first = int16_t(rc::ld16(v + 0xa));
            bool peak = true;
            for (int k = first; k < first + n; ++k)
            {
                const reg32 w = vertex(int16_t(rc::ld16(m_neighbours + 2 * reg32(k))));
                const reg16 old = rc::ld16(w + 0xe);
                if (old != m_stamp)
                    rc::stf32(w + 0x20, float(dot(w)));
                if (dd(rc::ldf32(w + 0x20)) > dd(best))
                {
                    peak = false;
                    if (queue && old != m_stamp)
                        push(w);
                }
                rc::st16(w + 0xe, m_stamp);
            }
            queue = rc::ld32(QueueLater) != 0;
            if (peak)
            {
                ecx = 0;
                return v;
            }
        }
        // Every vertex, the first taken whatever its dot.
        ecx = m_head;
        float best = fromBits(LowestBits);
        reg32 found = 0;
        const double lowest = ld(Lowest);
        for (x86::sreg32 i = 0; i < count; ++i)
        {
            const reg32 v = rc::ld32(m_polytope + PolyVertices) + VertexBytes * reg32(i);
            ecx = v;
            refresh(v);
            const float value = rc::ldf32(v + 0x20);
            if (dd(value) > dd(best) || dd(best) == lowest)
            {
                best = value;
                found = v;
            }
        }
        return found;
    }

private:
    reg32 vertex(x86::sreg32 index) const { return m_vertices + VertexBytes * reg32(index); }

    // (dz z + dy y) + dx x.
    double dot(reg32 v) const
    {
        double r = m_d[2] * ld(v + 0x18);
        r += m_d[1] * ld(v + 0x14);
        r += m_d[0] * ld(v + 0x10);
        return r;
    }

    // The vertex's dot, computed and stamped unless it carries this call's stamp.
    void refresh(reg32 v)
    {
        if (rc::ld16(v + 0xe) == m_stamp)
            return;
        const double value = dot(v);
        rc::st16(v + 0xe, m_stamp);
        rc::stf32(v + 0x20, float(value));
    }

    // The search queue: links in the vertices, the head on the guest's stack (kept here).
    void push(reg32 v)
    {
        const reg32 tail = m_previous;
        if (tail == m_head)
            m_next = v;
        else
            rc::st32(tail, v);
        m_previous = v;
        rc::st32(v + 4, tail);
        rc::st32(v, m_head);
    }

    reg32 pop()
    {
        const reg32 v = m_next;
        const reg32 next = rc::ld32(v), previous = rc::ld32(v + 4);
        if (previous == m_head)
            m_next = next;
        else
            rc::st32(previous, next);
        if (next == m_head)
            m_previous = previous;
        else
            rc::st32(next + 4, previous);
        return v;
    }

    const reg32 m_polytope, m_head;
    reg32 m_next, m_previous;
    reg32 m_vertices = 0, m_neighbours = 0;
    reg16 m_stamp = 0;
    double m_d[3] = {};
};

// D3DXVec3Transform (0x5ABDAF): out (4 floats) = (v, 1) * m.
void transformPoint(reg32 out, reg32 v, reg32 m)
{
    float in[4] = {rc::ldf32(v), rc::ldf32(v + 4), rc::ldf32(v + 8), 1.0f};
    float mm[16], r[4];
    memcpy(mm, rc::host(m), sizeof(mm));
    win32::d3dx9::transform(in, mm, r);
    for (reg32 i = 0; i < 4; ++i)
        rc::stf32(out + 4 * i, r[i]);
}

// The arguments of a support function (GJKPrimitive +8): the primitive, the direction, the start vertex, the world
// matrix, its inverse and the output vertex (id +0xC, position +0x10).
struct SupportCall
{
    reg32 primitive, direction, start, world, inverse, out;
};

SupportCall supportArguments(reg32 e)
{
    return SupportCall{rc::ld32(e + 4), rc::ld32(e + 8), rc::ld32(e + 0xc), rc::ld32(e + 0x14), rc::ld32(e + 0x18),
                       rc::ld32(e + 0x1c)};
}

// 0x472F30 entered with esp = e: the output vertex filled; ecx and edx as the guest leaves them.
SupportResult polytopeSupport(reg32 e, const SupportCall& call, const reg16* forcedStamp)
{
    const reg32 polytope = call.primitive, direction = call.direction, start = call.start;
    const reg32 world = call.world, inverse = call.inverse, out = call.out;
    // The guest's frame: its search queue head at esp + 0x28 after the three pushes.
    const reg32 frame = ((e - 4) & ~0xfu) - 0x34 - 0xc;
    const reg16 stamp = PolytopeSupport::stamp(forcedStamp);
    // The direction in the polytope's space, where the guest keeps it (esp + 0x30).
    const reg32 local = frame + 0x30;
    transformPoint(local, direction, inverse);
    const float d[3] = {rc::ldf32(local), rc::ldf32(local + 4), rc::ldf32(local + 8)};
    PolytopeSupport search(polytope, frame + 0x28);
    reg32 ecx = 0, edx = polytope;
    const reg32 found = search.run(start, stamp, d, ecx);
    if (found)
    {
        transformPoint(out + 0x10, found + 0x10, world);
        const reg16 id = rc::ld16(found + 0xc);
        rc::st16(out + 0xc, id);
        ecx = (world & 0xffff0000u) | id;
        edx = found + 0x10;
    }
    return SupportResult{ecx, edx};
}

// 0x472F30: the arguments on the stack ([esp+4] on). eax = the output.
void supportNative(x86::CPU& cpu, const reg16* forcedStamp)
{
    const reg32 e = cpu.esp;
    const SupportCall call = supportArguments(e);
    const SupportResult r = polytopeSupport(e, call, forcedStamp);
    cpu.eax = call.out;
    cpu.ecx = r.ecx;
    cpu.edx = r.edx;
    cpu.esp = e + 4;
}

// ---- support point of a wafer (0x473A90) ----

// Wafer: vertex count +0x14, vertices from +0x20 (0x30 bytes each: id +0xC, position +0x10); with a thickness
// (+0x1D4) above 0 and the flag at +0x1D0 clear, only the first half of the vertices.
const reg32 WaferCount = 0x14;
const reg32 WaferVertices = 0x20;
const reg32 WaferThickness = 0x1d4;
const reg32 WaferFlat = 0x1d0;

// 0x473A90: [esp+4] the wafer, [esp+8] the direction (world space), [esp+0x1C] the output vertex. The vertex with
// the largest dot (the first taken whatever its dot) copied out with w = 0; eax = the output.
SupportResult waferSupport(reg32 wafer, reg32 direction, reg32 out)
{
    x86::sreg32 count = x86::sreg32(rc::ld32(wafer + WaferCount));
    if (ld(wafer + WaferThickness) > ld(Zero) && rc::ld8(wafer + WaferFlat) == 0)
        count /= 2;
    const double dx = ld(direction), dy = ld(direction + 4), dz = ld(direction + 8);
    const double lowest = ld(Lowest);
    float best = fromBits(LowestBits);
    reg32 found = 0;
    const auto consider = [&](reg32 v, double value)
    {
        if (value > dd(best) || dd(best) == lowest)
        {
            best = float(value);
            found = v;
        }
    };
    // (x + z) + y; the unrolled guest loop adds (y + z) + x for the last three of each four.
    const auto xzy = [&](reg32 v)
    {
        double r = ld(v + 0x10) * dx;
        r += ld(v + 0x18) * dz;
        r += dy * ld(v + 0x14);
        return r;
    };
    const auto yzx = [&](reg32 v)
    {
        double r = ld(v + 0x14) * dy;
        r += ld(v + 0x18) * dz;
        r += ld(v + 0x10) * dx;
        return r;
    };
    x86::sreg32 i = 0;
    reg32 ecx = wafer;
    if (count >= 4)
    {
        do
        {
            const reg32 v = wafer + WaferVertices + VertexBytes * reg32(i);
            consider(v, xzy(v));
            consider(v + VertexBytes, yzx(v + VertexBytes));
            consider(v + 2 * VertexBytes, yzx(v + 2 * VertexBytes));
            consider(v + 3 * VertexBytes, yzx(v + 3 * VertexBytes));
            i += 4;
        } while (i + 3 < count);
    }
    for (; i < count; ++i)
    {
        const reg32 v = wafer + WaferVertices + VertexBytes * reg32(i);
        consider(v, xzy(v));
        ecx = v + VertexBytes + 0x14;
    }
    reg32 edx = direction;
    if (found)
    {
        ecx = rc::ld32(found + 0x10);
        rc::st32(out + 0x10, ecx);
        rc::stf32(out + 0x14, float(ld(found + 0x14)));
        rc::stf32(out + 0x18, float(ld(found + 0x18)));
        rc::st32(out + 0x1c, 0);
        const reg16 id = rc::ld16(found + 0xc);
        rc::st16(out + 0xc, id);
        edx = (direction & 0xffff0000u) | id;
    }
    return SupportResult{ecx, edx};
}

void waferSupportNative(x86::CPU& cpu)
{
    const reg32 e = cpu.esp;
    const reg32 out = rc::ld32(e + 0x1c);
    const SupportResult r = waferSupport(rc::ld32(e + 4), rc::ld32(e + 8), out);
    cpu.eax = out;
    cpu.ecx = r.ecx;
    cpu.edx = r.edx;
    cpu.esp = e + 4;
}

// ---- GJK distance of a witness (0x471DE0) ----

// Witness: the simplex pair at +0x60 (the vector at pair +0xA0 normalised at the end, the warm start flag at pair
// +0xD8), the world matrix and its inverse of shape A (+0x140, +0x180) and of shape B (+0x1C0, +0x200), the
// primitives (+0x244, +0x248). Primitive: support function +8, GetPoint +0xC, type +0x10 (2: GetPoint gives world
// space).
const reg32 WitnessPair = 0x60;
const reg32 WitnessNormal = 0x100;
const reg32 WitnessWarm = 0x138;
const reg32 WitnessWorldA = 0x140;
const reg32 WitnessInverseA = 0x180;
const reg32 WitnessWorldB = 0x1c0;
const reg32 WitnessInverseB = 0x200;
const reg32 WitnessPrimitiveA = 0x244;
const reg32 WitnessPrimitiveB = 0x248;
const reg32 PrimitiveSupport = 8;
const reg32 PrimitivePoint = 0xc;
const reg32 PrimitiveType = 0x10;
const reg32 WorldSpace = 2;
const reg32 PairVectors = 11;           // four points of each shape, the closest points and the normal
const reg32 PairWarm = 0xd8;

const reg32 PolytopeSupportFunction = 0x472f30;
const reg32 WaferSupportFunction = 0x473a90;
const reg32 PolytopePointFunction = 0x472ee0;
const reg32 WaferPointFunction = 0x473a40;

const reg32 Epsilon = 0x6cc8bc;          // 0.001: squared distance of a touching start

// The guest's frame (esp after its prologue): the vertices of B (+0x20) and A (+0x50), the direction (+0x10), its
// negation (+0x80), the first direction (+0x90).
const reg32 FrameVertexB = 0x20;
const reg32 FrameVertexA = 0x50;
const reg32 FrameDirection = 0x10;
const reg32 FrameNegated = 0x80;
const reg32 FrameFirst = 0x90;
const reg32 VertexId = 0xc;
const reg32 VertexPosition = 0x10;

inline float hostFloat(const x86::reg8* p) { float r; memcpy(&r, p, 4); return r; }

// A float moved through the x87 (fld, fstp).
inline void throughX87(x86::reg8* to, const x86::reg8* from)
{
    const float value = float(dd(hostFloat(from)));
    memcpy(to, &value, 4);
}

// The simplex pair copies of the guest: 0x4723A0 (pointsX87) moves the eight points' coordinates through the x87,
// 0x4724B0 their x as a dword; both move x of the closest points and the normal as a dword, their y and z through
// the x87, then the ids and counts of both shapes and the warm start flag as dwords.
void copyPair(x86::reg8* to, const x86::reg8* from, bool pointsX87)
{
    for (reg32 v = 0; v < PairVectors; ++v)
    {
        const reg32 o = 0x10 * v;
        if (pointsX87 && v < 2 * MaxPoints)
            throughX87(to + o, from + o);
        else
            memcpy(to + o, from + o, 4);
        throughX87(to + o + 4, from + o + 4);
        throughX87(to + o + 8, from + o + 8);
    }
    memcpy(to + IdsA, from + IdsA, CountA + 4 - IdsA);
    memcpy(to + IdsB, from + IdsB, CountB + 4 - IdsB);
    memcpy(to + PairWarm, from + PairWarm, 4);
}

// 0x567370 with ecx = edx = v: v normalised in place, (1, 0, 0) for length 0; the x87 condition codes of its
// compare.
void normaliseInPlace(x86::CPU& cpu, reg32 v)
{
    const double x = ld(v), y = ld(v + 4), z = ld(v + 8);
    double length = x * x;
    length += y * y;
    length += z * z;
    length = std::sqrt(length);
    const double zero = ld(Zero);
    cpu.fpu.compare(length, zero);
    if (length == zero)
    {
        rc::st32(v + 4, 0);
        rc::st32(v + 8, 0);
        rc::st32(v, bits(1.0f));
        return;
    }
    const double inverse = ld(One) / length;
    rc::stf32(v, float(inverse * x));
    rc::stf32(v + 4, float(y * inverse));
    rc::stf32(v + 8, float(z * inverse));
}

inline reg32 vertexId(reg32 vertex) { return reg32(x86::sreg32(int16_t(rc::ld16(vertex + VertexId)))); }

// One call of 0x471DE0. Polytope and wafer supports and GetPoints run natively, other primitives' functions as
// guest code with the guest's arguments and esp.
class WitnessDistance
{
public:
    WitnessDistance(win32::WinApplication* app, x86::CPU& cpu, const reg16* stamps, size_t stampCount)
        : m_app(app), m_cpu(cpu), m_witness(cpu.edi), m_pair(cpu.edi + WitnessPair),
          m_frame(((cpu.esp - 4) & ~0xfu) - 0x190), m_stamps(stamps), m_stampCount(stampCount)
    {
    }

    // False when guest code it called terminated the thread.
    bool run()
    {
        const reg32 entry = m_cpu.esp, ebx = m_cpu.ebx, esi = m_cpu.esi;
        const reg32 vertexA = m_frame + FrameVertexA, vertexB = m_frame + FrameVertexB;
        const reg32 direction = m_frame + FrameDirection;
        clearVertex(vertexA);
        clearVertex(vertexB);
        float distance;
        bool zero = false;
        reg32 edx = 0;
        if (rc::ld32(m_witness + WitnessWarm) == 0)
        {
            // Cold start: point 0 of both shapes, the support points along their difference.
            reg32 p = point(primitiveA(), 0, vertexA);
            if (dead())
                return false;
            transformPoint(vertexA + VertexPosition, p + VertexPosition, m_witness + WitnessWorldA);
            p = point(primitiveB(), 0, vertexB);
            if (dead())
                return false;
            transformPoint(vertexB + VertexPosition, p + VertexPosition, m_witness + WitnessWorldB);
            const reg32 first = m_frame + FrameFirst;
            for (reg32 k = 0; k < 12; k += 4)
                rc::stf32(first + k, float(ld(vertexB + VertexPosition + k) - ld(vertexA + VertexPosition + k)));
            negate(first);
            if (!supports(first))
                return false;
            for (reg32 k = 0; k < 12; k += 4)
            {
                rc::st32(m_pair + k, rc::ld32(vertexA + VertexPosition + k));
                rc::st32(m_pair + PointsB + k, rc::ld32(vertexB + VertexPosition + k));
            }
            rc::st32(m_pair + IdsA, vertexId(vertexA));
            rc::st32(m_pair + IdsB, vertexId(vertexB));
            rc::st32(m_pair + CountB, 1);
            rc::st32(m_pair + CountA, 1);
            moveX87Point(m_pair + ClosestA, m_pair);
            moveX87Point(m_pair + ClosestB, m_pair + PointsB);
            const float dx = float(ld(m_pair + ClosestB) - ld(m_pair + ClosestA));
            const float dy = float(ld(m_pair + ClosestB + 4) - ld(m_pair + ClosestA + 4));
            const float dz = float(ld(m_pair + ClosestB + 8) - ld(m_pair + ClosestA + 8));
            rc::stf32(direction, dx);
            rc::stf32(direction + 4, dy);
            rc::stf32(direction + 8, dz);
            double squared = dd(dz) * dd(dz);
            squared += dd(dy) * dd(dy);
            squared += dd(dx) * dd(dx);
            distance = float(squared);
            edx = rc::ld32(m_pair + PointsB);
            const double epsilon = ld(Epsilon);
            m_cpu.fpu.compare(squared, epsilon);
            if (squared <= epsilon)
                zero = true;
            else
                rc::st32(m_pair + PairWarm, 1);
        }
        else
        {
            // Warm start: the kept simplex points again in world space.
            for (x86::sreg32 i = 0; i < x86::sreg32(rc::ld32(m_pair + CountA)); ++i)
            {
                if (!warmPoint(primitiveA(), rc::ld32(m_pair + IdsA + 4 * reg32(i)), vertexA,
                               m_pair + 0x10 * reg32(i), m_witness + WitnessWorldA))
                    return false;
            }
            for (x86::sreg32 i = 0; i < x86::sreg32(rc::ld32(m_pair + CountB)); ++i)
            {
                if (!warmPoint(primitiveB(), rc::ld32(m_pair + IdsB + 4 * reg32(i)), vertexB,
                               m_pair + PointsB + 0x10 * reg32(i), m_witness + WitnessWorldB))
                    return false;
            }
            double squared;
            if (!simplexDistance(squared, edx))
                return false;
            distance = float(squared);
            const double epsilon = ld(Epsilon);
            m_cpu.fpu.compare(squared, epsilon);
            zero = squared <= epsilon;
        }

        if (!zero)
        {
            // Grow the simplex by the support points along the direction while the distance shrinks.
            float best = distance;
            x86::reg8 backup[PairBytes];
            copyPair(backup, rc::host(m_pair), true);
            for (;;)
            {
                negate(direction);
                if (!supports(direction))
                    return false;
                const bool addedA = add(IdsA, CountA, 0, vertexA);
                const bool addedB = add(IdsB, CountB, PointsB, vertexB);
                if (!addedA && !addedB)
                    break;
                double squared;
                if (!simplexDistance(squared, edx))
                    return false;
                distance = float(squared);
                const double zeroValue = ld(Zero);
                m_cpu.fpu.compare(squared, zeroValue);
                if (squared == zeroValue)
                {
                    zero = true;
                    break;
                }
                m_cpu.fpu.compare(dd(distance), dd(best));
                if (!(distance < best))
                {
                    distance = best;
                    copyPair(rc::host(m_pair), backup, false);
                    break;
                }
                best = distance;
                copyPair(backup, rc::host(m_pair), false);
            }
        }

        double result;
        if (zero)
            result = ld(Zero);
        else
        {
            normaliseInPlace(m_cpu, m_witness + WitnessNormal);
            edx = m_witness + WitnessNormal;
            m_cpu.eax = edx;
            m_cpu.ecx = edx;
            result = std::sqrt(dd(distance));
        }
        m_cpu.edx = edx;
        m_cpu.ebx = ebx;
        m_cpu.esi = esi;
        m_cpu.esp = entry + 4;
        m_cpu.fpu.regs[(m_cpu.fpu.count + 1) & 7] = result;
        m_cpu.fpu.count += 1;
        return true;
    }

    // The replayed stamps used (shadow compare).
    size_t stampsUsed() const { return m_stampAt; }

private:
    bool dead() const { return m_cpu.terminate; }

    reg32 primitiveA() const { return rc::ld32(m_witness + WitnessPrimitiveA); }
    reg32 primitiveB() const { return rc::ld32(m_witness + WitnessPrimitiveB); }

    // 0x4665E0: the vertex's index words and position zeroed.
    static void clearVertex(reg32 v)
    {
        for (reg32 k = 8; k < 0x10; k += 2)
            rc::st16(v + k, 0);
        for (reg32 k = VertexPosition; k < VertexPosition + 12; k += 4)
            rc::st32(v + k, 0);
    }

    // x as a dword, y and z through the x87.
    static void moveX87Point(reg32 to, reg32 from)
    {
        rc::st32(to, rc::ld32(from));
        rc::stf32(to + 4, float(ld(from + 4)));
        rc::stf32(to + 8, float(ld(from + 8)));
    }

    // GetPoint (primitive +0xC) of vertex index into out (its index words and position); returns its eax.
    reg32 point(reg32 primitive, reg32 index, reg32 out)
    {
        const reg32 function = rc::ld32(primitive + PrimitivePoint);
        reg32 v;
        if (function == PolytopePointFunction)
            v = rc::ld32(primitive + PolyVertices) + VertexBytes * index;
        else if (function == WaferPointFunction)
            v = primitive + WaferVertices + VertexBytes * index;
        else
        {
            const reg32 e = m_frame - 0x10;
            rc::st32(e + 4, primitive);
            rc::st32(e + 8, index);
            rc::st32(e + 0xc, out);
            m_cpu.esp = e + 4;
            ::native::callGuest(m_app, m_cpu, function);
            return m_cpu.eax;
        }
        for (reg32 k = 8; k < 0x10; k += 2)
            rc::st16(out + k, rc::ld16(v + k));
        moveX87Point(out + VertexPosition, v + VertexPosition);
        return out;
    }

    // Point i of a warm start: GetPoint, then the position as it is (world space type) or transformed.
    bool warmPoint(reg32 primitive, reg32 index, reg32 vertex, reg32 slot, reg32 world)
    {
        point(primitive, index, vertex);
        if (dead())
            return false;
        if (rc::ld32(primitive + PrimitiveType) == WorldSpace)
        {
            for (reg32 k = 0; k < 12; k += 4)
                rc::st32(slot + k, rc::ld32(vertex + VertexPosition + k));
        }
        else
            transformPoint(slot, vertex + VertexPosition, world);
        return true;
    }

    // -d at frame +0x80.
    void negate(reg32 d)
    {
        const reg32 negated = m_frame + FrameNegated;
        for (reg32 k = 0; k < 12; k += 4)
            rc::stf32(negated + k, float(-ld(d + k)));
    }

    // The support point of A along d and of B along -d (frame +0x80).
    bool supports(reg32 d)
    {
        const reg32 vertexA = m_frame + FrameVertexA, vertexB = m_frame + FrameVertexB;
        support(SupportCall{primitiveA(), d, vertexA, m_witness + WitnessWorldA, m_witness + WitnessInverseA, vertexA},
                vertexB + VertexPosition, m_frame - 0x20);
        if (dead())
            return false;
        support(SupportCall{primitiveB(), m_frame + FrameNegated, vertexB, m_witness + WitnessWorldB,
                            m_witness + WitnessInverseB, vertexB}, vertexA + VertexPosition, m_frame - 0x3c);
        return !dead();
    }

    // A support call with esp = e at its entry: seven arguments pushed for A, seven more for B (the guest pops both
    // after B); other: the fourth argument.
    void support(const SupportCall& call, reg32 other, reg32 e)
    {
        const reg32 function = rc::ld32(call.primitive + PrimitiveSupport);
        if (function == PolytopeSupportFunction)
        {
            const reg16* forced = nullptr;
            if (m_stamps)
            {
                if (m_stampAt < m_stampCount)
                    forced = &m_stamps[m_stampAt];
                ++m_stampAt;
            }
            polytopeSupport(e, call, forced);
            return;
        }
        if (function == WaferSupportFunction)
        {
            waferSupport(call.primitive, call.direction, call.out);
            return;
        }
        const reg32 arguments[7] = {call.primitive, call.direction, call.start, other, call.world, call.inverse,
                                    call.out};
        for (reg32 i = 0; i < 7; ++i)
            rc::st32(e + 4 + 4 * i, arguments[i]);
        m_cpu.esp = e + 4;
        ::native::callGuest(m_app, m_cpu, function);
    }

    // The support point in vertex added to a shape of the pair unless its id is there (0x47222B, 0x472295).
    bool add(reg32 ids, reg32 countAt, reg32 points, reg32 vertex)
    {
        const reg32 id = vertexId(vertex);
        const reg32 count = rc::ld32(m_pair + countAt);
        for (x86::sreg32 k = 0; k < x86::sreg32(rc::ld32(m_pair + countAt)); ++k)
        {
            if (rc::ld32(m_pair + ids + 4 * reg32(k)) == id)
                return false;
        }
        rc::st32(m_pair + ids + 4 * count, id);
        const reg32 slot = m_pair + points + 0x10 * rc::ld32(m_pair + countAt);
        moveX87Point(slot, vertex + VertexPosition);
        rc::st32(m_pair + countAt, rc::ld32(m_pair + countAt) + 1);
        return true;
    }

    // DistanceSqr of the pair (esi) with the direction out at frame +0x10 (edx); edx as it leaves it.
    bool simplexDistance(double& squared, reg32& edx)
    {
        phases::Scope scope(phases::SimplexDistance);
        m_cpu.esi = m_pair;
        m_cpu.edx = m_frame + FrameDirection;
        m_cpu.esp = m_frame - 4;
        if (!distanceNative(m_cpu))
        {
            sub_46ea50_guest(m_app, m_cpu);
            if (dead())
                return false;
        }
        squared = m_cpu.fpu.regs[m_cpu.fpu.count & 7];
        m_cpu.fpu.count -= 1;
        edx = m_cpu.edx;
        return true;
    }

    win32::WinApplication* const m_app;
    x86::CPU& m_cpu;
    const reg32 m_witness, m_pair, m_frame;
    const reg16* const m_stamps;
    const size_t m_stampCount;
    size_t m_stampAt = 0;
};

// ---- shadow compare ----

enum Entry
{
    EntryDistance, EntryPolytope, EntryWafer, EntryWitness, EntryCount
};

const char* const EntryNames[EntryCount] = {"simplex distance 0x46ea50", "polytope support 0x472f30",
                                            "wafer support 0x473a90", "gjk distance 0x471de0"};
const reg32 EntryAddresses[EntryCount] = {0x46ea50, 0x472f30, 0x473a90, 0x471de0};
const uint32_t ShadowLogLimit = 20;
uint32_t s_shadowCalls[EntryCount] = {};
uint32_t s_shadowDiffering = 0, s_shadowLogged = 0;

struct Span
{
    reg32 address;
    reg32 size;
};

// Guest memory of a few spans, saved into and put back from one byte buffer.
struct Spans
{
    Span spans[4];
    int count = 0;

    void add(reg32 address, reg32 size) { spans[count++] = Span{address, size}; }

    void save(std::vector<x86::reg8>& bytes) const
    {
        bytes.clear();
        for (int i = 0; i < count; ++i)
            bytes.insert(bytes.end(), rc::host(spans[i].address), rc::host(spans[i].address) + spans[i].size);
    }

    void restore(const std::vector<x86::reg8>& bytes) const
    {
        size_t at = 0;
        for (int i = 0; i < count; ++i)
        {
            memcpy(rc::host(spans[i].address), bytes.data() + at, spans[i].size);
            at += spans[i].size;
        }
    }
};

// The memory of a shadow call before it and after the guest's and the native code's runs (game thread only). The
// GJK distance has its own: the guest's run of it shadows the parts it calls.
struct Buffers
{
    std::vector<x86::reg8> before, guestAfter, nativeAfter;
};

Buffers s_parts, s_witness;

void logDifference(Entry entry, reg32 caller, const char* text)
{
    ++s_shadowDiffering;
    if (s_shadowLogged < ShadowLogLimit)
    {
        ++s_shadowLogged;
        pf::logInfo("shadow_gjk: %s (caller 0x%08x): %s", EntryNames[entry], unsigned(caller), text);
    }
}

// Logs the first difference of the guest's and the native code's results: the x87 depth, st0 when the function
// returns one, the flags and registers the callers read and the span bytes (NaN against NaN counts as equal).
void compare(Entry entry, reg32 caller, const x86::CPU& g, const x86::CPU& n, bool st0, const Spans& spans,
             const Buffers& buffers)
{
    ++s_shadowCalls[entry];
    char text[160] = {};
    bool same = true;
    if (g.fpu.count != n.fpu.count)
    {
        snprintf(text, sizeof(text), "x87 depth guest %u native %u", unsigned(g.fpu.count), unsigned(n.fpu.count));
        same = false;
    }
    if (same && st0)
    {
        const double a = g.fpu.regs[g.fpu.count & 7], b = n.fpu.regs[n.fpu.count & 7];
        if (memcmp(&a, &b, sizeof(a)) != 0 && !(std::isnan(a) && std::isnan(b)))
        {
            snprintf(text, sizeof(text), "st0 guest %.17g native %.17g", a, b);
            same = false;
        }
    }
    const reg32 function = EntryAddresses[entry];
    if (same)
        same = retstate::sameFlags(retstate::flags(function), g, n, text, sizeof(text));
    if (same)
        same = retstate::sameRegisters(retstate::registers(function), g, n, text, sizeof(text));
    size_t at = 0;
    for (int i = 0; same && i < spans.count; ++i)
    {
        const Span& s = spans.spans[i];
        for (reg32 k = 0; same && k + 4 <= s.size; k += 4)
        {
            reg32 a, b;
            memcpy(&a, buffers.guestAfter.data() + at + k, 4);
            memcpy(&b, buffers.nativeAfter.data() + at + k, 4);
            if (a != b && !(std::isnan(fromBits(a)) && std::isnan(fromBits(b))))
            {
                snprintf(text, sizeof(text), "dword at 0x%08x guest 0x%08x native 0x%08x", unsigned(s.address + k),
                         unsigned(a), unsigned(b));
                same = false;
            }
        }
        at += s.size;
    }
    if (!same)
        logDifference(entry, caller, text);
}

// The guest runs first; the spans are put back and the native code runs from the same entry state, then the guest's
// memory is put back again. native returns false when the guest body has to run (nothing compared).
template< typename Guest, typename Native >
void shadow(Entry entry, x86::CPU& cpu, Guest guest, const Spans& spans, Buffers& buffers, bool st0, Native native)
{
    spans.save(buffers.before);
    const x86::CPU entryCpu = cpu;
    const reg32 caller = rc::ld32(cpu.esp);
    guest();
    if (cpu.terminate)
        return;
    spans.save(buffers.guestAfter);
    spans.restore(buffers.before);
    x86::CPU n = entryCpu;
    const bool ran = native(n);
    spans.save(buffers.nativeAfter);
    spans.restore(buffers.guestAfter);
    if (ran)
        compare(entry, caller, cpu, n, st0, spans, buffers);
}

void shadowDistance(win32::WinApplication* app, x86::CPU& cpu)
{
    Spans spans;
    spans.add(cpu.esi, PairBytes);
    spans.add(cpu.edx, 12);
    shadow(EntryDistance, cpu, [&] { sub_46ea50_guest(app, cpu); }, spans, s_parts, true,
           [](x86::CPU& n) { return distanceNative(n); });
}

// The polytope vertex a support search starts from (the start vertex's id).
reg32 firstVertex(reg32 e)
{
    const reg32 polytope = rc::ld32(e + 4), start = rc::ld32(e + 0xc);
    return rc::ld32(polytope + PolyVertices) + VertexBytes * reg32(x86::sreg32(int16_t(rc::ld16(start + 0xc))));
}

// The native search takes the guest's stamp (left on the first vertex). Compared besides the registers: the random
// state, the polytope's vertices and the output vertex.
void shadowSupport(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 e = cpu.esp;
    const reg32 polytope = rc::ld32(e + 4), out = rc::ld32(e + 0x1c);
    const x86::sreg32 count = x86::sreg32(rc::ld32(polytope + PolyCount));
    const reg32 vertices = rc::ld32(polytope + PolyVertices);
    const reg32 first = firstVertex(e);
    Spans spans;
    spans.add(RandomSeed, 4);
    spans.add(vertices, count > 0 ? VertexBytes * reg32(count) : 0);
    spans.add(out, 0x20);
    // The guest's stamp in the saved bytes: the random state's 4, then the vertices.
    const size_t at = 4 + size_t(first + 0xe - vertices);
    const auto native = [&](x86::CPU& n)
    {
        const std::vector<x86::reg8>& g = s_parts.guestAfter;
        const reg16 stamp = at + 1 < g.size() ? reg16(g[at] | g[at + 1] << 8) : 0;
        supportNative(n, &stamp);
        return true;
    };
    shadow(EntryPolytope, cpu, [&] { sub_472f30_guest(app, cpu); }, spans, s_parts, false, native);
}

void shadowWafer(win32::WinApplication* app, x86::CPU& cpu)
{
    Spans spans;
    spans.add(rc::ld32(cpu.esp + 0x1c), 0x20);
    shadow(EntryWafer, cpu, [&] { sub_473a90_guest(app, cpu); }, spans, s_parts, false, [](x86::CPU& n)
    {
        waferSupportNative(n);
        return true;
    });
}

// The stamps of the polytope searches in the guest's run of a shadowed GJK distance, replayed in the native run.
const size_t StampLimit = 256;
bool s_recording = false;
reg16 s_stamps[StampLimit];
size_t s_stampCount = 0;

// After a polytope search of the guest's run: its stamp, left on the first vertex.
void recordStamp(reg32 first)
{
    if (s_stampCount < StampLimit)
        s_stamps[s_stampCount] = rc::ld16(first + 0xe);
    ++s_stampCount;
}

// The vertices of a polytope primitive.
void addPolytope(Spans& spans, reg32 primitive)
{
    if (rc::ld32(primitive + PrimitiveSupport) != PolytopeSupportFunction)
        return;
    const x86::sreg32 count = x86::sreg32(rc::ld32(primitive + PolyCount));
    spans.add(rc::ld32(primitive + PolyVertices), count > 0 ? VertexBytes * reg32(count) : 0);
}

// Compared besides st0 and the registers: the simplex pair, the random state and the polytopes' vertices. The native
// run replays the guest's stamps; a different number of searches counts as a difference.
void shadowWitness(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 witness = cpu.edi;
    Spans spans;
    spans.add(witness + WitnessPair, PairBytes);
    spans.add(RandomSeed, 4);
    addPolytope(spans, rc::ld32(witness + WitnessPrimitiveA));
    addPolytope(spans, rc::ld32(witness + WitnessPrimitiveB));
    s_stampCount = 0;
    const auto guest = [&]
    {
        s_recording = true;
        sub_471de0_guest(app, cpu);
        s_recording = false;
    };
    size_t used = 0;
    const auto native = [&](x86::CPU& n)
    {
        WitnessDistance distance(app, n, s_stamps, s_stampCount < StampLimit ? s_stampCount : StampLimit);
        const bool ran = distance.run();
        used = distance.stampsUsed();
        return ran;
    };
    const uint32_t differing = s_shadowDiffering;
    const reg32 caller = rc::ld32(cpu.esp);
    shadow(EntryWitness, cpu, guest, spans, s_witness, true, native);
    if (s_shadowDiffering == differing && used != s_stampCount && !cpu.terminate)
    {
        char text[80];
        snprintf(text, sizeof(text), "polytope searches guest %u native %u", unsigned(s_stampCount), unsigned(used));
        logDifference(EntryWitness, caller, text);
    }
}

}

void logShadowTotal()
{
    if (settings().native && settings().shadow)
        pf::logInfo("shadow_gjk: %u calls compared (gjk distance %u, simplex distance %u, polytope support %u, wafer "
                    "support %u), %u differ", unsigned(s_shadowCalls[EntryWitness] + s_shadowCalls[EntryDistance]
                                                      + s_shadowCalls[EntryPolytope] + s_shadowCalls[EntryWafer]),
                    unsigned(s_shadowCalls[EntryWitness]), unsigned(s_shadowCalls[EntryDistance]),
                    unsigned(s_shadowCalls[EntryPolytope]), unsigned(s_shadowCalls[EntryWafer]),
                    unsigned(s_shadowDiffering));
}

}

namespace native
{

// GJK distance of a witness (guest 0x471DE0).
void sub_471de0(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::Gjk);
    const gjk::Settings& s = gjk::settings();
    if (!s.native)
    {
        sub_471de0_guest(app, cpu);
        return;
    }
    if (__builtin_expect(s.shadow, 0))
    {
        gjk::shadowWitness(app, cpu);
        return;
    }
    gjk::WitnessDistance distance(app, cpu, nullptr, 0);
    distance.run();
}

// DistanceSqr of a simplex pair (guest 0x46EA50).
void sub_46ea50(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::SimplexDistance);
    const gjk::Settings& s = gjk::settings();
    if (!s.native)
    {
        sub_46ea50_guest(app, cpu);
        return;
    }
    if (__builtin_expect(s.shadow, 0))
    {
        gjk::shadowDistance(app, cpu);
        return;
    }
    if (!gjk::distanceNative(cpu))
        sub_46ea50_guest(app, cpu);
}

// Support point of a polytope (guest 0x472F30).
void sub_472f30(win32::WinApplication* app, x86::CPU& cpu)
{
    const gjk::Settings& s = gjk::settings();
    if (!s.native)
    {
        sub_472f30_guest(app, cpu);
        return;
    }
    if (__builtin_expect(s.shadow, 0))
    {
        const x86::reg32 first = gjk::firstVertex(cpu.esp);
        gjk::shadowSupport(app, cpu);
        if (gjk::s_recording && !cpu.terminate)
            gjk::recordStamp(first);
        return;
    }
    gjk::supportNative(cpu, nullptr);
}

// Support point of a wafer (guest 0x473A90).
void sub_473a90(win32::WinApplication* app, x86::CPU& cpu)
{
    const gjk::Settings& s = gjk::settings();
    if (!s.native)
    {
        sub_473a90_guest(app, cpu);
        return;
    }
    if (__builtin_expect(s.shadow, 0))
    {
        gjk::shadowWafer(app, cpu);
        return;
    }
    gjk::waferSupportNative(cpu);
}

}
}
