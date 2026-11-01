// Native replacements of guest functions (tools/recompiler/generate.py NATIVE_OVERRIDES). Each one does what the
// guest function does, including its ret: on entry cpu.esp points at the return address slot.

#include "cull.h"
#include "movie_yuv.h"
#include "phases.h"

#include <lib/guest.h>
#include <lib/winapp.h>
#include <platform/config.h>
#include <platform/gxm.h>
#include <platform/log.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fpu.h>
#include <native.h>
#include <recomp.h>
#include <utility>
#include <vector>
#include <winapi/d3d9_native.h>
#include <winapi/d3dx9.h>

namespace nfsu
{

// The translated guest bodies of the overridden functions (generated).
void sub_647cb0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4086b0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4f5730_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_514b90_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_58e810_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

// Stack argument n (0 = the first) of a cdecl function.
inline x86::reg32 argument(x86::CPU& cpu, int n)
{
    return rc::ld32(cpu.esp + 4 + 4 * n);
}

// st0 to a 64-bit integer, truncating toward zero; NaN and values out of range give the integer indefinite.
void floatToInteger(x86::CPU& cpu)
{
    const double value = double(cpu.fpu.pop());
    cpu.edx_eax = x86::reg64(x86::truncInteger64(value));
    cpu.esp += 4;
}

// 2D mapping read by the patched 2D polygon code (generate.py PATCHES): clip x = x * [HudScale] - [HudOffset] for x
// of the 640x480 space.
const x86::reg32 HudScale = 0x6d3440, HudOffset = 0x6d3444;     // past the data of .rdata, inside its raw size
double s_hudWidth = 1.0;    // part of the back buffer width the 640x480 space spans, centred (sub_4086b0)

// Where a 2D element goes: the centred 640x480 space, against the left or the right screen edge at the same scale,
// or stretched to the full width.
enum class Place { Centre, Left, Right, Full };

void setMapping(double scale, double offset)
{
    rc::stf32(HudScale, float(scale));
    rc::stf32(HudOffset, float(offset));
}

void setPlace(Place place)
{
    const double k = s_hudWidth;
    double scale = k / 320.0, offset = k;
    if (place == Place::Left)
        offset = 1.0;
    else if (place == Place::Right)
        offset = 2.0 * k - 1.0;
    else if (place == Place::Full)
        scale = 1.0 / 320.0, offset = 1.0;
    setMapping(scale, offset);
}

// hud_widescreen=on (default): in the track game flow states (4 and up; 3 is the front end) the HUD packages'
// elements go to the screen edge on their side and full-width 2D is stretched; off keeps every 2D element centred.
bool anchoring()
{
    static const bool hudAnchor = pf::Config::getBool("hud_widescreen", true);
    const x86::reg32 GameFlowState = 0x77a920;
    return hudAnchor && s_hudWidth < 1.0 && rc::ld32(GameFlowState) >= 4;
}

// menu_widescreen=on (default): the packages other than the HUD's are laid out for the screen width in every game
// flow state (menuPlace below); off keeps them centred at 4:3.
bool menuWidescreen()
{
    static const bool menu = [] {
        const bool on = pf::Config::getBool("menu_widescreen", true);
        pf::logInfo("screen: menu_widescreen=%s", on ? "on" : "off");
        return on;
    }();
    return menu && s_hudWidth < 1.0;
}

// x extent in the 640x480 space.
struct Span
{
    float x0 = HUGE_VALF, x1 = -HUGE_VALF;
    bool empty() const { return !(x0 <= x1); }
    void add(float x)
    {
        if (std::isfinite(x))
            x0 = std::min(x0, x), x1 = std::max(x1, x);
    }
    void add(const Span& other)
    {
        if (!other.empty())
            x0 = std::min(x0, other.x0), x1 = std::max(x1, other.x1);
    }
};

// Centre more than CentreBand from x = 320: the side of the screen; otherwise centred.
Place sideOf(const Span& span)
{
    const float CentreBand = 100.0f;
    const float centre = 0.5f * (span.x0 + span.x1);
    return centre < 320.0f - CentreBand ? Place::Left : centre > 320.0f + CentreBand ? Place::Right : Place::Centre;
}

bool fullWidth(const Span& span)
{
    return span.x0 <= 4.0f && span.x1 >= 636.0f;
}

// Polygons of an FE render object (FERenderObject): list head at +0x18 (next at +0), each node holds the polygon at
// +0x10 with the four vertices' x at +0x20, +0x30, +0x40, +0x50 of the node, already in the 640x480 space.
Span renderSpan(x86::reg32 renderObject)
{
    Span span;
    const x86::reg32 head = renderObject + 0x18;
    x86::reg32 node = rc::ld32(head);
    for (int n = 0; node != head && node != 0 && n < 4096; node = rc::ld32(node), ++n)
        for (x86::reg32 v = 0; v < 4; ++v)
            span.add(rc::ldf32(node + 0x20 + 0x10 * v));
    return span;
}

// Package name (FEPackage name at +0xC) equal to one of names, ignoring case.
template <size_t N>
bool packageNamed(x86::reg32 package, const char* const (&names)[N])
{
    const x86::reg32 name = package ? rc::ld32(package + 0xc) : 0;
    if (!name)
        return false;
    const char* text = reinterpret_cast<const char*>(win32::g_guestBase + name);
    for (const char* wanted : names)
    {
        size_t i = 0;
        while (wanted[i] && std::tolower(static_cast<unsigned char>(text[i])) ==
                                std::tolower(static_cast<unsigned char>(wanted[i])))
            ++i;
        if (!wanted[i] && !text[i])
            return true;
    }
    return false;
}

bool isHudPackage(x86::reg32 package)
{
    static const char* const names[] = {"HUD_SingleRace.fng", "HUD_Drift.fng", "drag_HUD_test.fng", "2PHudTop.fng",
                                        "2PHudBot.fng", "Chyron.fng"};
    return packageNamed(package, names);
}

// FE objects drawn by the package flush in progress, with the place of each.
struct Element
{
    x86::reg32 object;      // FEObject
    x86::reg32 unit;        // its top-level group in the package, or the object itself
    Span span;
    Place place;
    bool decided;
};
std::vector<Element> s_elements;                            // by object while the flush runs
std::vector<std::pair<x86::reg32, x86::reg32>> s_renders;  // (FEObject, FERenderObject)
bool s_anchorFlush = false;

// Top-level group of an FE object: +0x20 holds the render context of the parent group (0 at the package level), the
// context at render + 0x20 + 0xD0 * index holds its group at +0x50.
x86::reg32 topGroup(x86::reg32 render, x86::reg32 object)
{
    x86::reg32 unit = object;
    x86::reg32 context = rc::ld16(object + 0x20);
    for (int depth = 0; context != 0 && context < 64 && depth < 64; ++depth)
    {
        const x86::reg32 group = rc::ld32(render + 0x20 + 0xd0 * context + 0x50);
        if (group == 0 || group == unit)
            break;
        unit = group;
        context = rc::ld16(group + 0x20);
    }
    return unit;
}

// Places the objects of the render list before the flush draws them: the 256 depth lists at 0x746118 (8 bytes each,
// next at +0, FEObject at +8) and the cached renders at render + 0x10 (FERenderObject: next at +0, FEObject at +8),
// whose polygons are those of the last time the object was drawn. A top-level group goes as one unit by the extent
// of all its objects; a group with objects on both sides places each object by its own extent.
void placeElements(x86::reg32 render)
{
    s_renders.clear();
    const x86::reg32 renders = render + 0x10;
    x86::reg32 node = rc::ld32(renders);
    for (int n = 0; node != renders && node != 0 && n < 65536; node = rc::ld32(node), ++n)
        s_renders.emplace_back(rc::ld32(node + 8), node);
    std::sort(s_renders.begin(), s_renders.end());

    s_elements.clear();
    const x86::reg32 RenderLists = 0x746118;
    for (x86::reg32 list = RenderLists; list < RenderLists + 256 * 8; list += 8)
    {
        x86::reg32 entry = rc::ld32(list);
        for (int n = 0; entry != list && entry != 0 && n < 65536; entry = rc::ld32(entry), ++n)
        {
            const x86::reg32 object = rc::ld32(entry + 8);
            const auto found = std::lower_bound(s_renders.begin(), s_renders.end(),
                                                std::make_pair(object, x86::reg32(0)));
            Element element = {object, topGroup(render, object), Span(), Place::Centre, false};
            if (found != s_renders.end() && found->first == object)
                element.span = renderSpan(found->second);
            s_elements.push_back(element);
        }
    }

    std::sort(s_elements.begin(), s_elements.end(),
              [](const Element& a, const Element& b) { return a.unit < b.unit; });
    for (size_t first = 0, last; first < s_elements.size(); first = last)
    {
        Span unit;
        bool left = false, right = false;
        for (last = first; last < s_elements.size() && s_elements[last].unit == s_elements[first].unit; ++last)
        {
            const Element& element = s_elements[last];
            if (element.span.empty())
                continue;
            unit.add(element.span);
            const Place side = sideOf(element.span);
            left |= side == Place::Left;
            right |= side == Place::Right;
        }
        for (size_t i = first; i < last; ++i)
        {
            Element& element = s_elements[i];
            const Span& span = left && right ? element.span : unit;
            element.decided = !span.empty();
            if (element.decided)
                element.place = sideOf(span);
        }
    }
    std::sort(s_elements.begin(), s_elements.end(),
              [](const Element& a, const Element& b) { return a.object < b.object; });
}

const Element* findElement(x86::reg32 object)
{
    const auto found = std::lower_bound(s_elements.begin(), s_elements.end(), object,
                                        [](const Element& element, x86::reg32 o) { return element.object < o; });
    return found != s_elements.end() && found->object == object ? &*found : nullptr;
}

// Menu layout (menu_widescreen), in the 640x480 space, per package flush, from the polygons of the frame before:
// - bands: an extent wholly left of x = 320 that starts at x <= 80 goes to the left screen edge at the 4:3 scale
//   (weight -1), with a linear blend to centred (0) at x = 112; wholly right of x = 320 and ending at x >= 560 it goes
//   to the right edge (+1), centred at 528. Other extents stay centred.
// - fills: images (types 1, 9) without texture or whose texture is magnified at least 4 times across or down. A fill
//   at least 160 wide or across x = 320 that is no box (below) is stretched: each of its ends moves with the object
//   it touches there (the nearest within 4, overlapping it vertically), else by the band of the end; a fill at least
//   320 wide with an end in a band reaches the edge of the display there.
// - clusters: the other drawn objects on the 640x480 screen (no movies, nothing reaching both edges at x <= 8 and
//   >= 632, no fill taller than 48 unless a box, no picture 320 or wider), joined when their boxes are less than 24
//   apart horizontally and 12 vertically, and the objects of a top-level group whose drawn objects span at most 320.
//   A box (a fill 160 to 560 wide and at least 64 tall, within x 24..616) is a cluster member, and the objects
//   outside the clusters that touch a box, directly or through each other, go with its cluster. A cluster takes the
//   band weight of its extent, any other object that of its own.
// - objects wholly left of x = 0 or right of x = 640 stay off the screen; movies stay centred at 4:3.
// PC_menubar.fng goes to the left edge as a whole; PreRaceStats.fng keeps its objects in place (its fills still reach
// the display edges), its bet markers belong to the racer boxes.
struct Box
{
    Span x;
    float y0 = HUGE_VALF, y1 = -HUGE_VALF;
};

// x and y extent of an FE render object's polygons (y at +0x24, +0x34, +0x44, +0x54 of the node).
Box renderBox(x86::reg32 renderObject)
{
    Box box;
    const x86::reg32 head = renderObject + 0x18;
    x86::reg32 node = rc::ld32(head);
    for (int n = 0; node != head && node != 0 && n < 4096; node = rc::ld32(node), ++n)
        for (x86::reg32 v = 0; v < 4; ++v)
        {
            const float x = rc::ldf32(node + 0x20 + 0x10 * v), y = rc::ldf32(node + 0x24 + 0x10 * v);
            if (!std::isfinite(x) || !std::isfinite(y))
                continue;
            box.x.add(x);
            box.y0 = std::min(box.y0, y), box.y1 = std::max(box.y1, y);
        }
    return box;
}

const x86::reg32 FEObjectType = 0x18, FEObjectData = 0x2c;
const x86::reg32 TypeImage = 1, TypeString = 2, TypeMovie = 7, TypeColoredImage = 9;

float rampDown(float x, float full, float none)
{
    return x <= full ? 1.0f : x >= none ? 0.0f : (none - x) / (none - full);
}

float rampUp(float x, float none, float full)
{
    return 1.0f - rampDown(x, none, full);
}

const float BandLeft = 80.0f, BandRight = 560.0f, BandBlend = 32.0f;

// Weight of an extent: -1 at the left screen edge, 0 centred, 1 at the right screen edge.
float bandWeight(float x0, float x1)
{
    if (x1 <= 320.0f)
        return -rampDown(x0, BandLeft, BandLeft + BandBlend);
    if (x0 >= 320.0f)
        return rampUp(x1, BandRight - BandBlend, BandRight);
    return 0.0f;
}

struct MenuElement
{
    x86::reg32 object;      // FEObject
    x86::reg32 unit;        // its top-level group, or the object itself
    Box box;
    bool onScreen;          // drawn before, on the 640x480 screen, no movie
    bool fill, box2D, stretch, member;
    size_t cluster;         // union-find parent while the clusters are built
    float weight;           // placement of a translated object
    float endLeft, endRight;    // weights of the ends of a stretched fill
};
std::vector<MenuElement> s_menu;    // by object while a menu flush runs
bool s_menuFlush = false, s_menuAllLeft = false, s_menuFixed = false;

size_t clusterOf(size_t i)
{
    while (s_menu[i].cluster != i)
        i = s_menu[i].cluster = s_menu[s_menu[i].cluster].cluster;
    return i;
}

void joinClusters(size_t a, size_t b)
{
    a = clusterOf(a), b = clusterOf(b);
    if (a != b)
        s_menu[std::max(a, b)].cluster = std::min(a, b);
}

// An image whose texture is magnified at least 4 times across or down (u, v at +0x44, +0x48 and +0x4C, +0x50 of its
// data, texture width and height at +0x44 and +0x46 of the TextureInfo at +0x14 of the render object), or an
// untextured one: a plain fill or gradient.
bool isFill(x86::reg32 renderObject, x86::reg32 object, const Box& box)
{
    const x86::reg32 type = rc::ld32(object + FEObjectType);
    if (type != TypeImage && type != TypeColoredImage)
        return false;
    const x86::reg32 texture = rc::ld32(renderObject + 0x14);
    if (!texture)
        return true;
    const x86::reg32 data = rc::ld32(object + FEObjectData);
    float u = 1.0f, v = 1.0f;
    if (data)
    {
        u = std::fabs(rc::ldf32(data + 0x4c) - rc::ldf32(data + 0x44));
        v = std::fabs(rc::ldf32(data + 0x50) - rc::ldf32(data + 0x48));
    }
    const float texelsX = float(rc::ld16(texture + 0x44)) * (std::isfinite(u) ? u : 1.0f);
    const float texelsY = float(rc::ld16(texture + 0x46)) * (std::isfinite(v) ? v : 1.0f);
    return (texelsX > 0.0f && box.x.x1 - box.x.x0 >= 4.0f * texelsX) ||
           (texelsY > 0.0f && box.y1 - box.y0 >= 4.0f * texelsY);
}

// A box: a fill 160 to 560 wide and at least 64 tall within x 24..616 (dialog and list frames).
bool isBox(const Box& box, bool fill)
{
    const float width = box.x.x1 - box.x.x0, height = box.y1 - box.y0;
    return fill && width >= 160.0f && width < 560.0f && height >= 64.0f && box.x.x0 >= 24.0f && box.x.x1 <= 616.0f;
}

bool isStretched(const Box& box, bool fill, bool box2D)
{
    return fill && !box2D && (box.x.x1 - box.x.x0 >= 160.0f || (box.x.x0 < 320.0f && box.x.x1 > 320.0f));
}

bool touches(const Box& a, const Box& b)
{
    return a.x.x0 - 4.0f <= b.x.x1 && b.x.x0 <= a.x.x1 + 4.0f && a.y0 < b.y1 && b.y0 < a.y1;
}

// Weight of the end x of a stretched fill: that of the object it touches there, else that of the band.
float endWeight(const MenuElement& fill, float x, float band)
{
    float best = band, distance = HUGE_VALF, strength = -1.0f;
    for (const MenuElement& other : s_menu)
    {
        if (!other.onScreen || other.stretch || &other == &fill)
            continue;
        const Box& o = other.box;
        if (x < o.x.x0 - 4.0f || x > o.x.x1 + 4.0f || !(o.y0 < fill.box.y1 && fill.box.y0 < o.y1))
            continue;
        const float d = x >= o.x.x0 && x <= o.x.x1 ? 0.0f : std::min(std::fabs(o.x.x0 - x), std::fabs(o.x.x1 - x));
        if (d < distance || (d == distance && std::fabs(other.weight) > strength))
            best = other.weight, distance = d, strength = std::fabs(other.weight);
    }
    return best;
}

// Builds the clusters of the render list (the objects placeElements reads) and the placement of every object.
void placeMenuElements(x86::reg32 render, x86::reg32 package)
{
    static const char* const leftPackages[] = {"PC_menubar.fng"};
    static const char* const fixedPackages[] = {"PreRaceStats.fng"};
    const bool allLeft = packageNamed(package, leftPackages), fixed = packageNamed(package, fixedPackages);
    s_menuAllLeft = allLeft, s_menuFixed = fixed;

    s_renders.clear();
    const x86::reg32 renders = render + 0x10;
    x86::reg32 node = rc::ld32(renders);
    for (int n = 0; node != renders && node != 0 && n < 65536; node = rc::ld32(node), ++n)
        s_renders.emplace_back(rc::ld32(node + 8), node);
    std::sort(s_renders.begin(), s_renders.end());

    s_menu.clear();
    const x86::reg32 RenderLists = 0x746118;
    for (x86::reg32 list = RenderLists; list < RenderLists + 256 * 8; list += 8)
    {
        x86::reg32 entry = rc::ld32(list);
        for (int n = 0; entry != list && entry != 0 && n < 65536; entry = rc::ld32(entry), ++n)
        {
            const x86::reg32 object = rc::ld32(entry + 8);
            MenuElement element = {object, topGroup(render, object), Box(), false, false, false, false, false,
                                   s_menu.size(), allLeft ? -1.0f : 0.0f, 0.0f, 0.0f};
            const auto found = std::lower_bound(s_renders.begin(), s_renders.end(),
                                                std::make_pair(object, x86::reg32(0)));
            if (found != s_renders.end() && found->first == object)
                element.box = renderBox(found->second);
            const Box& b = element.box;
            const float width = b.x.x1 - b.x.x0, height = b.y1 - b.y0;
            element.onScreen = !b.x.empty() && rc::ld32(object + FEObjectType) != TypeMovie && b.x.x1 > 0.0f &&
                               b.x.x0 < 640.0f;
            if (element.onScreen && !allLeft)
            {
                element.fill = isFill(found->second, object, b);
                element.box2D = isBox(b, element.fill);
                element.stretch = isStretched(b, element.fill, element.box2D);
                element.member = !fixed && b.y1 > 0.0f && b.y0 < 480.0f && !(b.x.x0 <= 8.0f && b.x.x1 >= 632.0f) &&
                                 !element.stretch && !(element.fill && !element.box2D && height > 48.0f) &&
                                 !(rc::ld32(object + FEObjectType) != TypeString && !element.box2D && width >= 320.0f);
            }
            s_menu.push_back(element);
        }
    }
    if (allLeft)
    {
        std::sort(s_menu.begin(), s_menu.end(),
                  [](const MenuElement& a, const MenuElement& b) { return a.object < b.object; });
        return;
    }

    // Objects of a top-level group whose drawn objects span at most 320.
    std::vector<size_t> order(s_menu.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [](size_t a, size_t b) { return s_menu[a].unit < s_menu[b].unit; });
    for (size_t first = 0, last; first < order.size(); first = last)
    {
        Span unit;
        for (last = first; last < order.size() && s_menu[order[last]].unit == s_menu[order[first]].unit; ++last)
            if (s_menu[order[last]].onScreen)
                unit.add(s_menu[order[last]].box.x);
        if (unit.empty() || unit.x1 - unit.x0 > 320.0f)
            continue;
        size_t head = order.size();
        for (size_t i = first; i < last; ++i)
            if (s_menu[order[i]].member)
            {
                if (head == order.size())
                    head = order[i];
                else
                    joinClusters(head, order[i]);
            }
    }

    // Boxes less than 24 apart horizontally and 12 vertically.
    const float GapX = 24.0f, GapY = 12.0f;
    order.erase(std::remove_if(order.begin(), order.end(), [](size_t i) { return !s_menu[i].member; }), order.end());
    std::sort(order.begin(), order.end(), [](size_t a, size_t b) { return s_menu[a].box.x.x0 < s_menu[b].box.x.x0; });
    for (size_t i = 0; i < order.size(); ++i)
    {
        const Box& a = s_menu[order[i]].box;
        for (size_t j = i + 1; j < order.size(); ++j)
        {
            const Box& b = s_menu[order[j]].box;
            if (b.x.x0 > a.x.x1 + GapX)
                break;
            if (b.y0 <= a.y1 + GapY && a.y0 <= b.y1 + GapY)
                joinClusters(order[i], order[j]);
        }
    }

    // Cluster extents.
    std::vector<Span> extents(s_menu.size());
    for (size_t i : order)
        extents[clusterOf(i)].add(s_menu[i].box.x);

    // Objects outside the clusters that touch a box, directly or through each other, go with the box's cluster.
    const size_t None = s_menu.size();
    std::vector<size_t> attached(s_menu.size(), None);
    for (size_t i : order)
        if (s_menu[i].box2D)
            attached[i] = clusterOf(i);
    for (int pass = 0; pass < 4; ++pass)
    {
        bool changed = false;
        for (size_t i = 0; i < s_menu.size(); ++i)
        {
            const MenuElement& element = s_menu[i];
            if (attached[i] != None || element.member || element.stretch || !element.onScreen)
                continue;
            for (size_t j = 0; j < s_menu.size(); ++j)
                if (attached[j] != None && touches(s_menu[j].box, element.box))
                {
                    attached[i] = attached[j];
                    changed = true;
                    break;
                }
        }
        if (!changed)
            break;
    }

    // Weights of the translated objects, then the ends of the stretched fills.
    for (size_t i = 0; i < s_menu.size(); ++i)
    {
        MenuElement& element = s_menu[i];
        if (fixed || element.stretch || element.box.x.empty())
            continue;
        const Span& extent = element.member         ? extents[clusterOf(i)]
                             : attached[i] != None ? extents[attached[i]]
                                                   : element.box.x;
        element.weight = bandWeight(extent.x0, extent.x1);
    }
    for (MenuElement& element : s_menu)
    {
        if (!element.stretch || fixed)
            continue;
        const Span& x = element.box.x;
        element.endLeft = endWeight(element, x.x0, -rampDown(x.x0, BandLeft, BandLeft + BandBlend));
        element.endRight = endWeight(element, x.x1, rampUp(x.x1, BandRight - BandBlend, BandRight));
    }

    std::sort(s_menu.begin(), s_menu.end(),
              [](const MenuElement& a, const MenuElement& b) { return a.object < b.object; });
}

const MenuElement* findMenuElement(x86::reg32 object)
{
    const auto found = std::lower_bound(s_menu.begin(), s_menu.end(), object,
                                        [](const MenuElement& element, x86::reg32 o) { return element.object < o; });
    return found != s_menu.end() && found->object == object ? &*found : nullptr;
}

// Sets the mapping of an FE render object of a menu flush; false keeps the centred one.
bool menuPlace(x86::reg32 renderObject)
{
    const x86::reg32 object = rc::ld32(renderObject + 8);
    const Box box = renderBox(renderObject);
    const Span& x = box.x;
    if (x.empty() || rc::ld32(object + FEObjectType) == TypeMovie)
        return false;
    if (x.x1 <= 0.0f || x.x0 >= 640.0f)
    {
        setMapping(0.0, 4.0);
        return true;
    }
    // An object first drawn this frame is placed by its own extent.
    const MenuElement* element = findMenuElement(object);
    const bool known = element && !element->box.x.empty();
    bool stretch;
    float weight = 0.0f, endLeft = 0.0f, endRight = 0.0f;
    if (s_menuAllLeft)
        stretch = false, weight = -1.0f;
    else if (known)
        stretch = element->stretch, weight = element->weight, endLeft = element->endLeft, endRight = element->endRight;
    else
    {
        const bool fill = isFill(renderObject, object, box);
        stretch = isStretched(box, fill, isBox(box, fill));
        if (!s_menuFixed)
        {
            weight = bandWeight(x.x0, x.x1);
            endLeft = -rampDown(x.x0, BandLeft, BandLeft + BandBlend);
            endRight = rampUp(x.x1, BandRight - BandBlend, BandRight);
        }
    }
    const double k = s_hudWidth, b = 1.0 - k, scale = k / 320.0;
    if (stretch && x.x1 - x.x0 > 0.5f)
    {
        double x0 = x.x0 * scale - k + endLeft * b, x1 = x.x1 * scale - k + endRight * b;
        if (x.x1 - x.x0 >= 320.0f && !s_menuFixed)
        {
            x0 += rampDown(x.x0, BandLeft, BandLeft + BandBlend) * (std::min(x0, -1.0) - x0);
            x1 += rampUp(x.x1, BandRight - BandBlend, BandRight) * (std::max(x1, 1.0) - x1);
        }
        else if (x.x1 - x.x0 >= 320.0f)
        {
            x0 += rampDown(x.x0, 8.0f, 16.0f) * (std::min(x0, -1.0) - x0);
            x1 += rampUp(x.x1, 624.0f, 632.0f) * (std::max(x1, 1.0) - x1);
        }
        const double fillScale = (x1 - x0) / (x.x1 - x.x0);
        setMapping(fillScale, x.x0 * fillScale - x0);
        return true;
    }
    if (weight == 0.0f)
        return false;
    setMapping(scale, k - weight * b);
    return true;
}

}

// _ftol: fistp with the rounding control set to truncate, then the caller's control word again.
void sub_672fc8(win32::WinApplication*, x86::CPU& cpu)
{
    floatToInteger(cpu);
}

// _ftol2: rounds to nearest and corrects toward zero by the sign of the difference, which is truncation.
void sub_674898(win32::WinApplication*, x86::CPU& cpu)
{
    floatToInteger(cpu);
}

// _chkstk: moves esp down by eax bytes below the return address and returns there with eax = the dword in the
// return address slot (the guest pushes it again before its ret). The guest touches each page on the way; guest
// stacks here are committed memory.
void sub_6734a0(win32::WinApplication*, x86::CPU& cpu)
{
    const x86::reg32 slot = cpu.esp;
    const x86::reg32 size = cpu.eax;
    cpu.eax = rc::ld32(slot);
    cpu.esp = slot + 4 - size;
}

// Movie YUV420 -> 32-bit converter (Y, U, V, Y stride, width, height, dst, dst pitch, mode). The guest returns 0
// at once when the EA MMX flag [0x70FDD8] is clear and has only MMX kernels; mode 1 (A8R8G8B8, the only mode the
// game uses, 0x0041325D) runs natively whatever the flag says. Other modes keep the guest code.
void sub_647cb0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (argument(cpu, 8) != 1)
    {
        sub_647cb0_guest(app, cpu);
        return;
    }
    static const movie::Tables tables = {0x6e8130, 0x6e8930, 0x6e9130, 0x712018};
    cpu.eax = movie::convertFrame(win32::g_guestBase, tables, argument(cpu, 0), argument(cpu, 1), argument(cpu, 2),
                                  x86::sreg32(argument(cpu, 3)), x86::sreg32(argument(cpu, 4)),
                                  x86::sreg32(argument(cpu, 5)), argument(cpu, 6), x86::sreg32(argument(cpu, 7)));
    cpu.esp += 4;
}

// Back buffer size (0x4086B0): width in the argument, height in esi; the guest stores them in the present parameters
// and in [0x701034] / [0x701038]. Every device creation and Reset size passes here (0x408A38, 0x4087A0..0x408804,
// 0x4494CE). widescreen=on (default) replaces the size with the display's, off keeps the game's. Afterwards the 2D
// mapping the patched code reads (generate.py PATCHES) is written: the 640x480 space spans k of the back buffer
// width, centred, with k = (height / width) / 0.75 and at most 1.
void sub_4086b0(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 ScreenWidth = 0x701034, ScreenHeight = 0x701038;
    static const bool widescreen = pf::Config::getBool("widescreen", true);
    const x86::reg32 esi = cpu.esi;
    if (widescreen)
    {
        rc::st32(cpu.esp + 4, pf::gxm::DisplayWidth);
        cpu.esi = pf::gxm::DisplayHeight;
    }
    sub_4086b0_guest(app, cpu);
    if (cpu.terminate)
        return;
    cpu.esi = esi;
    const x86::reg32 width = rc::ld32(ScreenWidth), height = rc::ld32(ScreenHeight);
    const double k = width ? std::min(1.0, double(height) / double(width) / 0.75) : 1.0;
    s_hudWidth = k;
    setPlace(Place::Centre);
    pf::logInfo("screen: back buffer %ux%u%s, 2D width %.4f", unsigned(width), unsigned(height),
                widescreen ? " (widescreen)" : "", k);
}

// debug_frame_dump=on: a frame's FE objects in the log, with their package, type, texture name (TextureInfo+0xC),
// extent and the mapping they are drawn with.
static x86::reg32 s_dumpPackage = 0;

static const char* guestText(x86::reg32 address, char* out, size_t size)
{
    size_t i = 0;
    for (; address && i + 1 < size; ++i)
    {
        const char c = char(rc::ld8(address + x86::reg32(i)));
        if (!c)
            break;
        out[i] = c >= 32 && c < 127 ? c : '?';
    }
    out[i] = 0;
    return out;
}

static void drawObject(win32::WinApplication* app, x86::CPU& cpu)
{
    if (win32::d3d9::bridge::dumpingFrame())
    {
        const x86::reg32 renderObject = argument(cpu, 0), object = rc::ld32(renderObject + 8);
        const x86::reg32 texture = rc::ld32(renderObject + 0x14);
        const Box box = renderBox(renderObject);
        const double scale = rc::ldf32(HudScale), offset = rc::ldf32(HudOffset);
        char package[40], name[25];
        pf::logInfo("dump: FE %s type %u texture %s %ux%u, x %.1f..%.1f y %.1f..%.1f, scale %.6f offset %.4f, "
                    "px %.0f..%.0f", guestText(s_dumpPackage ? rc::ld32(s_dumpPackage + 0xc) : 0, package, sizeof(package)),
                    unsigned(rc::ld32(object + FEObjectType)), guestText(texture ? texture + 0xc : 0, name, sizeof(name)),
                    texture ? unsigned(rc::ld16(texture + 0x44)) : 0u, texture ? unsigned(rc::ld16(texture + 0x46)) : 0u,
                    box.x.x0, box.x.x1, box.y0, box.y1, scale, offset, (box.x.x0 * scale - offset + 1.0) * 480.0,
                    (box.x.x1 * scale - offset + 1.0) * 480.0);
    }
    sub_514b90_guest(app, cpu);
}

// A movie covering the 640x480 space (x 0..640) on a wider screen: the side areas are drawn black with the movie's
// own polygons through the 2D polygon routine 0x40BEF0 (eax = polygon at node + 0x10, ecx = texture, esi = the
// render object's draw count above 2, as FERenderObject::Render passes them), their vertex colours (polygon + 0x80,
// 4 bytes per vertex, alpha last) set to opaque black and mapped onto clip x -1..-k and k..1. The movie draws
// without blending, so the bars are black whatever the texture holds there.
static void drawMovieBars(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 renderObject)
{
    const double k = s_hudWidth;
    const x86::reg32 eax = cpu.eax, ebx = cpu.ebx, ecx = cpu.ecx, edx = cpu.edx, esi = cpu.esi, edi = cpu.edi,
                     ebp = cpu.ebp, esp = cpu.esp;
    const x86::reg32 head = renderObject + 0x18, drawn = rc::ld32(renderObject + 0x10);
    for (int side = 0; side < 2 && !cpu.terminate; ++side)
    {
        setMapping((1.0 - k) / 640.0, side == 0 ? 1.0 : -k);
        x86::reg32 node = rc::ld32(head);
        for (int n = 0; node != head && node != 0 && n < 64 && !cpu.terminate; node = rc::ld32(node), ++n)
        {
            const x86::reg32 polygon = node + 0x10, colours = polygon + 0x80;
            x86::reg32 saved[4];
            for (x86::reg32 v = 0; v < 4; ++v)
            {
                saved[v] = rc::ld32(colours + 4 * v);
                rc::st32(colours + 4 * v, 0xff000000);
            }
            const x86::reg32 texture = rc::ld32(node + 0xb0);
            cpu.eax = polygon;
            cpu.ecx = texture ? texture : rc::ld32(renderObject + 0x14);
            cpu.esi = drawn > 2 ? 1 : 0;
            ::native::callGuest(app, cpu, 0x40bef0);
            for (x86::reg32 v = 0; v < 4; ++v)
                rc::st32(colours + 4 * v, saved[v]);
        }
    }
    setPlace(Place::Centre);
    cpu.eax = eax, cpu.ebx = ebx, cpu.ecx = ecx, cpu.edx = edx, cpu.esi = esi, cpu.edi = edi, cpu.ebp = ebp;
    cpu.esp = esp;
}

// End of an FE package's rendering (cFEng, ecx; the package in the argument): the guest draws the package's render
// list (0x4F2EB0). In a HUD package the objects are placed first (placeElements), in the other packages the menu
// layout is built first (placeMenuElements), and FE render objects are drawn at their place (sub_514b90).
void sub_4f5730(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::FrontEnd);
    const x86::reg32 package = argument(cpu, 0);
    s_dumpPackage = package;
    const bool hud = isHudPackage(package);
    s_anchorFlush = hud && anchoring();
    s_menuFlush = !hud && menuWidescreen();
    if (s_anchorFlush)
        placeElements(rc::ld32(cpu.ecx + 0xc));
    else if (s_menuFlush)
        placeMenuElements(rc::ld32(cpu.ecx + 0xc), package);
    sub_4f5730_guest(app, cpu);
    s_anchorFlush = s_menuFlush = false;
}

// Draws the polygons of an FE render object (FERenderObject in the argument, its FEObject at +8) through the 2D
// polygon routine 0x40BEF0. In a menu flush the menu layout places it (menuPlace). With anchoring: movies stay
// centred, other full-width objects are stretched, HUD objects go to their place. A movie covering the 640x480 space gets black side bars
// (drawMovieBars): the full-screen effect pass the game draws before it copies a render target that holds an old
// picture there.
static void placeAndDraw(win32::WinApplication* app, x86::CPU& cpu);

void sub_514b90(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 renderObject = argument(cpu, 0);
    bool movie = false;
    if (s_hudWidth < 1.0 && rc::ld32(rc::ld32(renderObject + 8) + FEObjectType) == TypeMovie)
    {
        const Span x = renderSpan(renderObject);
        movie = !x.empty() && x.x0 <= 1.0f && x.x1 >= 639.0f;
    }
    placeAndDraw(app, cpu);
    if (movie && !cpu.terminate)
        drawMovieBars(app, cpu, renderObject);
}

static void placeAndDraw(win32::WinApplication* app, x86::CPU& cpu)
{
    if (s_menuFlush)
    {
        if (!menuPlace(argument(cpu, 0)))
        {
            drawObject(app, cpu);
            return;
        }
        drawObject(app, cpu);
        setPlace(Place::Centre);
        return;
    }
    const x86::reg32 renderObject = argument(cpu, 0);
    if (!anchoring() || rc::ld32(rc::ld32(renderObject + 8) + FEObjectType) == TypeMovie)
    {
        drawObject(app, cpu);
        return;
    }
    const Span span = renderSpan(renderObject);
    Place place = Place::Centre;
    if (!span.empty() && fullWidth(span))
        place = Place::Full;
    else if (s_anchorFlush)
    {
        const Element* element = findElement(rc::ld32(renderObject + 8));
        if (element && element->decided)
            place = element->place;
        else if (!span.empty())
            place = sideOf(span);
    }
    if (place == Place::Centre)
    {
        drawObject(app, cpu);
        return;
    }
    setPlace(place);
    drawObject(app, cpu);
    setPlace(Place::Centre);
}

// Track map of a player (TrackMap in eax), drawn after the FE packages: map, markers and overlay through the 2D
// polygon routine. Centre x at +0x68 plus the x offset of the map mode (+0xC0, table of 0x3C bytes at 0x737850,
// x offset at +8), half width at +0x70; with anchoring the whole map goes to its side like the HUD objects.
void sub_58e810(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!anchoring())
    {
        sub_58e810_guest(app, cpu);
        return;
    }
    const x86::reg32 map = cpu.eax;
    const x86::reg32 mode = rc::ld32(map + 0xc0);
    const float centre = rc::ldf32(map + 0x68) + (mode < 16 ? rc::ldf32(0x737858 + 0x3c * mode) : 0.0f);
    const float half = rc::ldf32(map + 0x70);
    Span span;
    span.add(centre - half);
    span.add(centre + half);
    const Place place = span.empty() ? Place::Centre : sideOf(span);
    setPlace(place);
    sub_58e810_guest(app, cpu);
    setPlace(Place::Centre);
}

// FE mouse pointer (x, y in the arguments, cdecl), drawn after the FE packages. The Vita has no pointer, so nothing is
// drawn; the pointer state the guest keeps is kept: [0x7357A8] = moved since the last call (read by the main menu),
// [0x746110] = time of the last move ([0x73AD3C]), [0x7357A0] / [0x7357A4] = the position.
void sub_4f3030(win32::WinApplication*, x86::CPU& cpu)
{
    const x86::reg32 LastX = 0x7357a0, LastY = 0x7357a4, Moved = 0x7357a8, MoveTime = 0x746110, Now = 0x73ad3c;
    const x86::reg32 x = argument(cpu, 0), y = argument(cpu, 1);
    const bool moved = x != rc::ld32(LastX) || y != rc::ld32(LastY);
    rc::st8(Moved, moved ? 1 : 0);
    if (moved)
        rc::st32(MoveTime, rc::ld32(Now));
    rc::st32(LastX, x);
    rc::st32(LastY, y);
    cpu.esp += 4;
}

// D3DX: the port's effect layer and matrix helpers (src/lib/winapi/d3dx9), stdcall like the guest's.
void sub_5af9e5(win32::WinApplication* app, x86::CPU& cpu)
{
    win32::d3dx9::CreateEffectPool(app, cpu);
}

void sub_5afb2f(win32::WinApplication* app, x86::CPU& cpu)
{
    win32::d3dx9::CreateEffect(app, cpu);
}

void sub_5ac69c(win32::WinApplication* app, x86::CPU& cpu)
{
    win32::d3dx9::MatrixMultiply(app, cpu);
}

void sub_5abdaf(win32::WinApplication* app, x86::CPU& cpu)
{
    win32::d3dx9::Vec3Transform(app, cpu);
}

void sub_5ac443(win32::WinApplication* app, x86::CPU& cpu)
{
    win32::d3dx9::Vec4Transform(app, cpu);
}

void sub_5ac8e5(win32::WinApplication* app, x86::CPU& cpu)
{
    win32::d3dx9::MatrixTranspose(app, cpu);
}

// Scenery culler and box against frustum test (cull.cpp).
void sub_446620(win32::WinApplication* app, x86::CPU& cpu)
{
    cull::doCulling(app, cpu);
}

void sub_40db20(win32::WinApplication* app, x86::CPU& cpu)
{
    cull::getVisibleState(app, cpu);
}

}
}
