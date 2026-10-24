// Native replacements of guest functions (tools/recompiler/generate.py NATIVE_OVERRIDES). Each one does what the
// guest function does, including its ret: on entry cpu.esp points at the return address slot.

#include "cull.h"
#include "movie_yuv.h"

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
    cpu.edx_eax = x86::reg64(x86::fistInteger<x86::sreg64>(std::trunc(value)));
    cpu.esp += 4;
}

// 2D mapping read by the patched 2D polygon code (generate.py PATCHES): clip x = x * [HudScale] - [HudOffset] for x
// of the 640x480 space.
const x86::reg32 HudScale = 0x6d3440, HudOffset = 0x6d3444;     // past the data of .rdata, inside its raw size
double s_hudWidth = 1.0;    // part of the back buffer width the 640x480 space spans, centred (sub_4086b0)

// Where a 2D element goes: the centred 640x480 space, against the left or the right screen edge at the same scale,
// or stretched to the full width.
enum class Place { Centre, Left, Right, Full };

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
    rc::stf32(HudScale, float(scale));
    rc::stf32(HudOffset, float(offset));
}

// hud_anchor=1 (default): in the track game flow states (4 and up; 3 is the front end) the HUD packages' elements go
// to the screen edge on their side and full-width 2D is stretched; 0 keeps every 2D element centred.
bool anchoring()
{
    static const bool hudAnchor = pf::Config::getBool("hud_anchor", true);
    const x86::reg32 GameFlowState = 0x77a920;
    return hudAnchor && s_hudWidth < 1.0 && rc::ld32(GameFlowState) >= 4;
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

// HUD packages (FEPackage name at +0xC).
bool isHudPackage(x86::reg32 package)
{
    static const char* const names[] = {"HUD_SingleRace.fng", "HUD_Drift.fng", "drag_HUD_test.fng", "2PHudTop.fng",
                                        "2PHudBot.fng", "Chyron.fng"};
    const x86::reg32 name = package ? rc::ld32(package + 0xc) : 0;
    if (!name)
        return false;
    const char* text = reinterpret_cast<const char*>(win32::g_guestBase + name);
    for (const char* hud : names)
    {
        size_t i = 0;
        while (hud[i] && std::tolower(static_cast<unsigned char>(text[i])) ==
                             std::tolower(static_cast<unsigned char>(hud[i])))
            ++i;
        if (!hud[i] && !text[i])
            return true;
    }
    return false;
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
// 0x4494CE). widescreen=1 (default) replaces the size with the display's, 0 keeps the game's. Afterwards the 2D
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

// End of an FE package's rendering (cFEng, ecx; the package in the argument): the guest draws the package's render
// list (0x4F2EB0). In a HUD package the objects are placed first (placeElements) and FE render objects are drawn
// at their place (sub_514b90).
void sub_4f5730(win32::WinApplication* app, x86::CPU& cpu)
{
    s_anchorFlush = anchoring() && isHudPackage(argument(cpu, 0));
    if (s_anchorFlush)
        placeElements(rc::ld32(cpu.ecx + 0xc));
    sub_4f5730_guest(app, cpu);
    s_anchorFlush = false;
}

// Draws the polygons of an FE render object (FERenderObject in the argument, its FEObject at +8) through the 2D
// polygon routine 0x40BEF0. With anchoring: full-width objects are stretched, HUD objects go to their place.
void sub_514b90(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!anchoring())
    {
        sub_514b90_guest(app, cpu);
        return;
    }
    const x86::reg32 renderObject = argument(cpu, 0);
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
        sub_514b90_guest(app, cpu);
        return;
    }
    setPlace(place);
    sub_514b90_guest(app, cpu);
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
