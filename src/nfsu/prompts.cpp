#include "prompts.h"

#include <lib/controls.h>
#include <lib/memmap.h>
#include <platform/config.h>
#include <platform/log.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace nfsu { namespace prompts
{

namespace
{

using x86::reg8;
using x86::reg16;
using x86::reg32;

// ---- guest memory ----

inline reg8 ld8(reg32 a) { return win32::g_guestBase[a]; }
inline reg16 ld16(reg32 a) { reg16 v; memcpy(&v, win32::g_guestBase + a, 2); return v; }
inline reg32 ld32(reg32 a) { reg32 v; memcpy(&v, win32::g_guestBase + a, 4); return v; }
inline void st16(reg32 a, reg16 v) { memcpy(win32::g_guestBase + a, &v, 2); }
inline void st32(reg32 a, reg32 v) { memcpy(win32::g_guestBase + a, &v, 4); }
inline bool valid(reg32 a, reg32 size) { return a && win32::MemMap::isGuestRange(a, size); }

// ---- glyph art (app0:glyphs.bin) ----

// Atlas sets: the menu bar key caps (32x32 and their mip levels) and the glyph characters of the two fonts.
enum Set : reg8 { Key32, Key16, Key8, Tt21, Tt14 };

// Character codes of the glyphs (bytes the language tables pass through unchanged).
const reg8 Cross = 0x81, Circle = 0x82, Square = 0x83, Triangle = 0x84, ShoulderL = 0x86, ShoulderR = 0x87,
           Start = 0x88, Select = 0x89, DpadUp = 0x8B, DpadDown = 0x8C, DpadLeft = 0x8D, DpadRight = 0x8E,
           LeftStick = 0x8F, RightStick = 0x90, RearLeft = 0x91, RearRight = 0x93, DpadLeftRight = 0x94;

struct Glyph
{
    reg8        code;
    reg8        set;
    reg8        w, h;
    reg16       x, y;           // font sets: position in the font texture
    signed char xoff, yoff;     // font sets: offset of the glyph from the pen position
    reg8        advance;        // font sets
    const reg8* alpha;          // w * h, row by row
};

std::vector<reg8>  s_atlas;
std::vector<Glyph> s_glyphs;
bool               s_on = false;

bool loadAtlas(const char* path)
{
    FILE* file = fopen(path, "rb");
    if (!file)
        return false;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    s_atlas.resize(size > 0 ? size_t(size) : 0);
    const bool read = !s_atlas.empty() && fread(s_atlas.data(), 1, s_atlas.size(), file) == s_atlas.size();
    fclose(file);
    if (!read || s_atlas.size() < 8 || memcmp(s_atlas.data(), "NUGL", 4) != 0)
        return false;
    reg16 version, count;
    memcpy(&version, &s_atlas[4], 2);
    memcpy(&count, &s_atlas[6], 2);
    if (version != 1 || 8 + size_t(count) * 16 > s_atlas.size())
        return false;
    for (reg16 i = 0; i < count; ++i)
    {
        const reg8* e = &s_atlas[8 + i * 16];
        Glyph g;
        g.code = e[0];
        g.set = e[1];
        g.w = e[2];
        g.h = e[3];
        memcpy(&g.x, e + 4, 2);
        memcpy(&g.y, e + 6, 2);
        g.xoff = static_cast<signed char>(e[8]);
        g.yoff = static_cast<signed char>(e[9]);
        g.advance = e[10];
        reg32 offset;
        memcpy(&offset, e + 12, 4);
        if (!g.w || !g.h || offset + size_t(g.w) * g.h > s_atlas.size())
            return false;
        g.alpha = &s_atlas[offset];
        s_glyphs.push_back(g);
    }
    return true;
}

const Glyph* findGlyph(reg8 set, reg8 code)
{
    for (const Glyph& g : s_glyphs)
    {
        if (g.set == set && g.code == code)
            return &g;
    }
    return nullptr;
}

// ---- DXT3 ----

const reg32 FourccDxt3 = 0x33545844;

// One DXT3 block: 4-bit alpha per pixel from a (16 values 0..255, row by row), white colour.
void encodeBlock(const reg8 a[16], reg8 out[16])
{
    for (int row = 0; row < 4; ++row)
    {
        reg16 bits = 0;
        for (int i = 0; i < 4; ++i)
            bits |= reg16(((a[row * 4 + i] * 15 + 127) / 255) << (4 * i));
        out[row * 2] = reg8(bits);
        out[row * 2 + 1] = reg8(bits >> 8);
    }
    memset(out + 8, 0xFF, 4);
    memset(out + 12, 0, 4);
}

// Each pixel of DXT3 block old whose alpha in block is larger takes that alpha and the brighter of old's two
// colours.
void mergeBlock(reg8 old[16], const reg8 block[16])
{
    const auto brightness = [](reg16 c) { return ((c >> 11) & 31) * 2 + ((c >> 5) & 63) + (c & 31) * 2; };
    reg16 c0, c1;
    reg32 indices;
    memcpy(&c0, old + 8, 2);
    memcpy(&c1, old + 10, 2);
    memcpy(&indices, old + 12, 4);
    const reg32 white = brightness(c0) >= brightness(c1) ? 0 : 1;
    for (int i = 0; i < 16; ++i)
    {
        const int shift = (i & 1) * 4;
        const int a = (old[i / 2] >> shift) & 15, b = (block[i / 2] >> shift) & 15;
        if (b <= a)
            continue;
        old[i / 2] = reg8((old[i / 2] & ~(15 << shift)) | (b << shift));
        indices = (indices & ~(3u << (2 * i))) | (white << (2 * i));
    }
    memcpy(old + 12, &indices, 4);
}

reg32 dxtLevelSize(reg32 w, reg32 h)
{
    return std::max<reg32>(1, w / 4) * std::max<reg32>(1, h / 4) * 16;
}

// Texture record: +0x24 name hash, +0x38 pixel bytes, +0x44/+0x46 width and height, +0x4e level count, +0x70 the
// pixels (levels in order); [record] + 0x14 the format.
struct Record
{
    reg32 hash, bytes, width, height, levels, pixels, format;
};

bool readRecord(reg32 t, Record* r)
{
    if (!valid(t, 0x78))
        return false;
    const reg32 platform = ld32(t);
    if (!valid(platform, 0x18))
        return false;
    r->hash = ld32(t + 0x24);
    r->bytes = ld32(t + 0x38);
    r->width = ld16(t + 0x44);
    r->height = ld16(t + 0x46);
    r->levels = ld8(t + 0x4e);
    r->pixels = ld32(t + 0x70);
    r->format = ld32(platform + 0x14);
    return r->pixels && valid(r->pixels, r->bytes);
}

// ---- menu bar key caps ----

struct KeyCap
{
    reg32       hash;
    reg8        code;   // 0: empty
    const char* name;
};

const KeyCap s_keyCaps[] =
{
    { 0x001AEF69, Cross,  "PC_X" },
    { 0xC5172FE2, Circle, "PC_BACK" },
    { 0xC51FBF74, 0,        "PC_QUIT" },
    { 0x6B6AB9EC, Triangle, "PC_CUSTOM" },
    { 0x8CF52B6B, Triangle, "PC_RACEOP" },
    { 0x04BDFDA3, Triangle, "PC_PERFORMANCE" },
    { 0x6C9A44A4, Triangle, "PC_DELETE" },
    { 0x4073AF02, Triangle, "PC_CREATEROOM" },
    { 0xCBDF98C9, Triangle, "PC_AGREEMENT" },
    { 0xA35C52C5, Triangle, "PC_TUTORIAL" },
    { 0xC51A8C5A, 0,        "PC_HELP" },
};

void replaceKeyCap(const KeyCap& cap, const Record& r)
{
    if (r.format != FourccDxt3 || r.width != 32 || r.height != 32 || r.levels < 1)
    {
        pf::logError("button prompts: %s is not a 32x32 DXT3 texture, kept", cap.name);
        return;
    }
    reg32 offset = 0;
    for (reg32 level = 0; level < r.levels; ++level)
    {
        const reg32 size = 32 >> level;
        const reg32 bytes = dxtLevelSize(size, size);
        if (offset + bytes > r.bytes)
            break;
        const Glyph* g = cap.code && level < 3 ? findGlyph(reg8(Key32 + level), cap.code) : nullptr;
        if (g && (g->w != size || g->h != size))
            g = nullptr;
        const reg32 blocks = std::max<reg32>(1, size / 4);
        for (reg32 by = 0; by < blocks; ++by)
        {
            for (reg32 bx = 0; bx < blocks; ++bx)
            {
                reg8 a[16] = {};
                for (reg32 y = 0; y < 4 && g; ++y)
                    for (reg32 x = 0; x < 4; ++x)
                        if (by * 4 + y < size && bx * 4 + x < size)
                            a[y * 4 + x] = g->alpha[(by * 4 + y) * size + bx * 4 + x];
                encodeBlock(a, win32::g_guestBase + r.pixels + offset + (by * blocks + bx) * 16);
            }
        }
        offset += bytes;
    }
    static std::vector<reg32> s_logged;
    if (std::find(s_logged.begin(), s_logged.end(), cap.hash) == s_logged.end())
    {
        s_logged.push_back(cap.hash);
        pf::logInfo("button prompts: key cap %s %s", cap.name, cap.code ? "replaced" : "cleared");
    }
}

// ---- fonts ----

struct FontSet
{
    reg8        set;
    reg32       textureHash;
    const char* name;
    bool        failed;         // the glyphs could not be drawn into the texture
};

FontSet s_fonts[] =
{
    { Tt21, 0x5B9D88B9, "conduitmditc_tt21i", false },
    { Tt14, 0x5B9D84DB, "conduitmditc_tt14i", false },
};

// The glyphs of a set drawn into a texture-sized alpha canvas; rows [*top, *bottom) hold them.
std::vector<reg8> fontCanvas(reg8 set, reg32 width, reg32 height, reg32* top, reg32* bottom)
{
    std::vector<reg8> canvas(size_t(width) * height, 0);
    *top = height;
    *bottom = 0;
    for (const Glyph& g : s_glyphs)
    {
        if (g.set != set || g.x + g.w > width || g.y + g.h > height)
            continue;
        for (reg32 y = 0; y < g.h; ++y)
            memcpy(&canvas[(g.y + y) * width + g.x], g.alpha + y * g.w, g.w);
        *top = std::min<reg32>(*top, g.y);
        *bottom = std::max<reg32>(*bottom, g.y + g.h);
    }
    return canvas;
}

// Draws the set's glyphs into every level of the font texture. Level 0 blocks must be empty or hold the glyphs
// already, else nothing is written (false). In the smaller levels a block with alpha keeps its colours and gets
// the larger alpha of each pixel.
bool paintFont(const FontSet& font, const Record& r)
{
    if (r.format != FourccDxt3 || r.width != 256 || r.height != 256 || r.levels < 1)
        return false;
    reg32 top, bottom;
    const std::vector<reg8> canvas = fontCanvas(font.set, r.width, r.height, &top, &bottom);
    if (top >= bottom)
        return false;
    for (int pass = 0; pass < 2; ++pass)
    {
        reg32 offset = 0;
        for (reg32 level = 0; level < r.levels; ++level)
        {
            const reg32 w = r.width >> level, h = r.height >> level;
            const reg32 bytes = dxtLevelSize(w, h);
            if (!w || !h || offset + bytes > r.bytes)
                break;
            const reg32 scale = 1u << level, bw = std::max<reg32>(1, w / 4);
            for (reg32 by = (top >> level) / 4; by * 4 < std::min(h, ((bottom - 1) >> level) + 1); ++by)
            {
                for (reg32 bx = 0; bx < bw; ++bx)
                {
                    reg8 a[16] = {};
                    bool any = false;
                    for (reg32 y = 0; y < 4; ++y)
                    {
                        for (reg32 x = 0; x < 4; ++x)
                        {
                            reg32 sum = 0;
                            for (reg32 sy = 0; sy < scale; ++sy)
                                for (reg32 sx = 0; sx < scale; ++sx)
                                {
                                    const reg32 cx = (bx * 4 + x) * scale + sx, cy = (by * 4 + y) * scale + sy;
                                    if (cx < r.width && cy < r.height)
                                        sum += canvas[cy * r.width + cx];
                                }
                            a[y * 4 + x] = reg8(sum / (scale * scale));
                            any = any || a[y * 4 + x] >= 9;
                        }
                    }
                    if (!any)
                        continue;
                    reg8 block[16];
                    encodeBlock(a, block);
                    reg8* old = win32::g_guestBase + r.pixels + offset + (by * bw + bx) * 16;
                    static const reg8 clear[8] = {};
                    const bool empty = memcmp(old, clear, 8) == 0;
                    if (pass == 0 && level == 0 && !empty && memcmp(old, block, 8) != 0)
                        return false;
                    if (pass == 1 && empty)
                        memcpy(old, block, 16);
                    else if (pass == 1)
                        mergeBlock(old, block);
                }
            }
            offset += bytes;
        }
    }
    return true;
}

// FEngFont objects: a list with its head node at 0x7460FC (+0 next); +0xC the font data ("FNTF": +0xa glyph count,
// +0xc flags, 0x40000 for 16-byte glyphs, +0x14 the glyph table's offset from the header), +0x1C the font name.
// Glyph: +0 code, +2 width, +3 height, +4 x, +6 y, +9 x offset, +0xa y offset, +0xe advance; sorted by code.
const reg32 FontList = 0x007460FC;
const reg32 FntfMagic = 0x46544E46;

struct PatchedFont
{
    reg32 header, table, oldOffset, oldCount;
    reg8  set;
};
std::vector<PatchedFont> s_patched;
std::vector<win32::MemMap*> s_memory;   // the extended glyph tables, never freed

bool guestNameIs(reg32 address, const char* name)
{
    if (!valid(address, 1))
        return false;
    for (size_t i = 0;; ++i)
    {
        if (!valid(address + reg32(i), 1))
            return false;
        char c = char(ld8(address + reg32(i)));
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
        if (c != name[i])
            return false;
        if (!c)
            return true;
    }
}

void restoreFonts(reg8 set)
{
    for (PatchedFont& p : s_patched)
    {
        if (p.set != set || !p.header || ld32(p.header + 0x14) != p.table - p.header)
            continue;
        st32(p.header + 0x14, p.oldOffset);
        st16(p.header + 0xa, reg16(p.oldCount));
        p.header = 0;
        pf::logInfo("button prompts: glyphs removed from the font of set %u again", unsigned(set));
    }
}

void extendFont(reg32 header, const FontSet& font)
{
    for (const PatchedFont& p : s_patched)
    {
        if (p.header == header && ld32(header + 0x14) == p.table - header)
            return;
    }
    if (!valid(header, 0x20) || ld32(header) != FntfMagic || !(ld32(header + 0xc) & 0x40000))
        return;
    const reg32 count = ld16(header + 0xa), offset = ld32(header + 0x14), table = header + offset;
    if (!count || count > 4096 || !valid(table, count * 16))
        return;
    std::vector<const Glyph*> added;
    for (const Glyph& g : s_glyphs)
    {
        if (g.set == font.set)
            added.push_back(&g);
    }
    std::sort(added.begin(), added.end(), [](const Glyph* a, const Glyph* b) { return a->code < b->code; });
    reg32 previous = 0;
    for (reg32 i = 0; i < count; ++i)
    {
        const reg32 code = ld16(table + i * 16);
        if (i && code <= previous)
            return;
        for (const Glyph* g : added)
        {
            if (g->code == code)
                return;
        }
        previous = code;
    }
    const reg32 total = count + reg32(added.size());
    win32::MemMap* memory = new win32::MemMap(total * 16, win32::MemMap::High);
    s_memory.push_back(memory);
    const reg32 copy = memory->getBlockStart();
    reg32 out = copy;
    size_t next = 0;
    for (reg32 i = 0; i <= count; ++i)
    {
        const reg32 code = i < count ? ld16(table + i * 16) : 0x10000;
        for (; next < added.size() && added[next]->code < code; ++next)
        {
            const Glyph& g = *added[next];
            reg8 e[16] = {};
            e[0] = g.code;
            e[2] = g.w;
            e[3] = g.h;
            memcpy(e + 4, &g.x, 2);
            memcpy(e + 6, &g.y, 2);
            e[9] = reg8(g.xoff);
            e[0xa] = reg8(g.yoff);
            e[0xe] = g.advance;
            memcpy(win32::g_guestBase + out, e, 16);
            out += 16;
        }
        if (i < count)
        {
            memcpy(win32::g_guestBase + out, win32::g_guestBase + table + i * 16, 16);
            out += 16;
        }
    }
    s_patched.push_back(PatchedFont{ header, copy, offset, count, font.set });
    st32(header + 0x14, copy - header);
    st16(header + 0xa, reg16(total));
    pf::logInfo("button prompts: %u glyphs added to font %s (0x%08X)", unsigned(added.size()), font.name,
                unsigned(header));
}

void pollFonts()
{
    reg32 node = ld32(FontList);
    for (int n = 0; n < 64 && node && node != FontList && valid(node, 0x20); ++n, node = ld32(node))
    {
        for (const FontSet& font : s_fonts)
        {
            if (!font.failed && guestNameIs(ld32(node + 0x1c), font.name))
                extendFont(ld32(node + 0xc), font);
        }
    }
}

// ---- strings ----

// The language table: entries (label hash, string) sorted by hash at [0x734434], their count at [0x73442C].
const reg32 StringTable = 0x00734434, StringCount = 0x0073442C;

struct StringPatch
{
    reg32       hash;
    const char* label;
    const char* original;
    const char* text;
    reg32       address;    // the replacement in guest memory
};

StringPatch s_strings[] =
{
    { 0x350891CF, "PRESS_ENTER_TO_CONTINUE", "Press enter to continue", "Press \x81 to continue", 0 },
    { 0x70C47A47, "GAME_TIPS_DRAG_PC",
      "Tap the directional key left or right to change lanes during a drag race",
      "Tap \x94 left or right to change lanes during a drag race", 0 },
};
win32::MemMap* s_stringMemory = nullptr;

bool guestStringIs(reg32 address, const char* text)
{
    const size_t length = strlen(text) + 1;
    return valid(address, reg32(length)) && memcmp(win32::g_guestBase + address, text, length) == 0;
}

void pollStrings()
{
    const reg32 table = ld32(StringTable), count = ld32(StringCount);
    if (!s_stringMemory || !count || count > 100000 || !valid(table, count * 8))
        return;
    for (StringPatch& s : s_strings)
    {
        reg32 lo = 0, hi = count;
        while (lo < hi)
        {
            const reg32 mid = (lo + hi) / 2;
            if (ld32(table + mid * 8) < s.hash)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo >= count || ld32(table + lo * 8) != s.hash)
            continue;
        const reg32 entry = table + lo * 8 + 4;
        if (ld32(entry) == s.address || !guestStringIs(ld32(entry), s.original))
            continue;
        st32(entry, s.address);
        pf::logInfo("button prompts: string %s replaced", s.label);
    }
}

// ---- menu bar ----

// Object names (hashes) a screen's package has for each menu bar item, bit by bit (0x4F8000): quit, next,
// customize, race options, back, ?, create room, change colour, performance, delete persona, agreement, delete
// profile, decal colour, create game, stock, default settings. Quit and change colour are hidden.
const reg32 MenuBarTags = 0x00706FD8;
const int s_hiddenItems[] = { 0, 7 };

// The items shown on the current screen, bit by bit as above ([0x735780], 0x4F8000), the current screen's package
// name (0x72CDD0) and the character the menu bar's tick (0x4E6F00) reads and clears ([0x7363B1]: t tutorial in
// the quick race mode select, the others per item below). [0x7363A8]: a text field is being edited.
const reg32 MenuBarItems = 0x00735780, CurrentScreen = 0x0072CDD0, MenuBarKey = 0x007363B1,
            TextEditing = 0x007363A8;

struct ItemKey
{
    int  bit;
    char key;
};

// Triangle: the first item shown in this order.
const ItemKey s_triangleItems[] =
{
    { 2, 'c' }, { 3, 'r' }, { 8, 'p' }, { 14, 'z' }, { 15, 'z' }, { 12, 'z' }, { 11, 'd' }, { 9, 'd' }, { 10, 'a' },
    { 6, 'z' }, { 13, 'z' },
};

char s_pendingKey = 0;

// Joystick object names on the controls screen (button order of src/lib/controls.cpp, axes X, Y, Z, Rx, Ry, Rz).
const char s_cross[] = { char(Cross), 0 }, s_circle[] = { char(Circle), 0 }, s_square[] = { char(Square), 0 },
           s_triangle[] = { char(Triangle), 0 }, s_l[] = { char(ShoulderL), 0 }, s_r[] = { char(ShoulderR), 0 },
           s_start[] = { char(Start), 0 }, s_select[] = { char(Select), 0 }, s_up[] = { char(DpadUp), 0 },
           s_down[] = { char(DpadDown), 0 }, s_left[] = { char(DpadLeft), 0 }, s_right[] = { char(DpadRight), 0 },
           s_rearLeft[] = { char(RearLeft), 0 }, s_rearRight[] = { char(RearRight), 0 },
           s_leftStick[] = { char(LeftStick), 0 }, s_rightStick[] = { char(RightStick), 0 };
const char* const s_buttonNames[16] =
{
    s_triangle, s_circle, s_cross, s_square, s_rearLeft, s_rearRight, s_l, s_r,
    s_select, s_start, nullptr, nullptr, s_up, s_right, s_down, s_left,
};
const char* const s_axisNames[win32::controls::AxisCount] =
{
    s_leftStick, s_leftStick, nullptr, nullptr, nullptr, s_rightStick, nullptr, nullptr,
};

}

void configure()
{
    const char* mode = pf::Config::getString("button_prompts", "vita");
    if (strcmp(mode, "pc") == 0)
    {
        pf::logInfo("button prompts: the game's PC prompts (button_prompts=pc)");
        return;
    }
    if (strcmp(mode, "vita") != 0)
        pf::logError("button_prompts=%s: unknown value, using vita", mode);
    if (!loadAtlas("app0:glyphs.bin"))
    {
        pf::logError("button prompts: app0:glyphs.bin is missing or invalid, keeping the PC prompts");
        s_glyphs.clear();
        return;
    }
    s_on = true;
    for (int item : s_hiddenItems)
        st32(MenuBarTags + 4 * item, 0);
    s_stringMemory = new win32::MemMap(4096, win32::MemMap::High);
    reg32 at = s_stringMemory->getBlockStart();
    for (StringPatch& s : s_strings)
    {
        const size_t length = strlen(s.text) + 1;
        memcpy(win32::g_guestBase + at, s.text, length);
        s.address = at;
        at += reg32((length + 3) & ~size_t(3));
    }
    win32::controls::setObjectNames(s_buttonNames, 16, s_axisNames);
    pf::logInfo("button prompts: Vita buttons (button_prompts=vita), %u glyphs", unsigned(s_glyphs.size()));
}

void texture(reg32 t)
{
    if (!s_on)
        return;
    const reg32 hash = valid(t, 0x28) ? ld32(t + 0x24) : 0;
    for (const KeyCap& cap : s_keyCaps)
    {
        Record r;
        if (cap.hash == hash && readRecord(t, &r))
            replaceKeyCap(cap, r);
    }
    for (FontSet& font : s_fonts)
    {
        Record r;
        if (font.textureHash != hash || font.failed || !readRecord(t, &r))
            continue;
        if (paintFont(font, r))
            pf::logInfo("button prompts: glyphs drawn into font texture %s", font.name);
        else
        {
            font.failed = true;
            restoreFonts(font.set);
            pf::logError("button prompts: the free rows of font texture %s are not empty, no glyphs for it",
                         font.name);
        }
    }
}

void poll()
{
    if (!s_on)
        return;
    if (s_pendingKey && char(ld8(MenuBarKey)) == s_pendingKey)
        win32::g_guestBase[MenuBarKey] = 0;
    s_pendingKey = 0;
    pollFonts();
    pollStrings();
}

void menuTriangle()
{
    if (!s_on || ld32(TextEditing) != 0)
        return;
    const reg32 items = ld32(MenuBarItems);
    char key = 0;
    for (const ItemKey& item : s_triangleItems)
    {
        if (items & (1u << item.bit))
        {
            key = item.key;
            break;
        }
    }
    if (!key && guestNameIs(CurrentScreen, "mu_quickracemodeselect.fng"))
        key = 't';
    if (!key)
        return;
    win32::g_guestBase[MenuBarKey] = reg8(key);
    s_pendingKey = key;
}

}}
