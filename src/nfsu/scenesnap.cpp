// The scene build's snapshot at the handoff and its shadow check (scenesnap.h).

#include "scenesnap.h"

#include "bench.h"
#include "coll.h"
#include "scenethread.h"
#include "worldmodels.h"

#include <winapi/d3d9_native.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace nfsu { namespace scenesnap
{

namespace
{

using x86::reg32;
using x86::sreg32;
namespace bridge = win32::d3d9::bridge;

typedef void (*Function)(win32::WinApplication*, x86::CPU&);

// Guest data.
const reg32 CarList = 0x7361f8;         // the list head of the cars (next car at +0)
const reg32 WorldModels = 0x73781c;     // world model list (sentinel); space node at +0x60
const reg32 ParticleLists[2] = { 0x78e980, 0x78e9e0 };
const reg32 Clans = 0x737bc4;           // clan list (sentinel), skid links at clan +8
const reg32 DrawnSkids = 0x737bdc;
const reg32 Regions = 0x77a940;         // region list sentinel (VisibleSectionManager 0x77a930 + 0x10)
const reg32 TexturePacks = 0x740418;    // texture pack list (sentinel), moved to front by GetTextureInfo 0x5461c0
const reg32 TextureAnimations = 0x740410;   // packs with animations: count +0x10, records of 0x34 bytes at +8
const reg32 Views = 0x740460;           // 16 eViews of 0x60 bytes, camera at +0x40
const reg32 PlayerView = 0x7404c0;      // view 1: the one RenderWorldModels draws
const reg32 GameState = 0x77a920;
const reg32 LightGroups = 0x743c10;     // light groups (sentinel): on +0x28, light entries from +0x2c, light at entry +8
const reg32 Spawners = 0x734470;        // smokeable spawners, 0x70 bytes each; their number at +4
const reg32 SceneryTable = 0x72a008;    // scenery sections by number: instance records of 0x30 bytes at +0x20

const reg32 LowestAddress = 0x00400000, HighestAddress = 0x20000000;
const uint32_t ListLimit = 20000;
const uint32_t ShadowLogLimit = 20;
const reg32 StackRewind = 0x10000;
const reg32 Line = 64;
const reg32 PoseBytes = 0x40 + 48 * 0x40;   // an animation pose [space+0xe8]: its matrix at +0x40, the bone palette
const reg32 CarRenderInfoSize = 0x1dc0;     // the allocation of CarRenderInfo::CarRenderInfo 0x56a130's callers

// Words of the image the views read that the frame writes after them, or that the views write and later parts
// read (they are put back with the snapshot in the shadow check).
const struct { reg32 address, size; } ImageRanges[] = {
    { 0x6d3a00, 4 },        // the clear flag copy the views wrapper stores
    { 0x6f0884, 0x28 },     // 60 Hz frame totals (AdvanceRealTime / AdvanceWorldTime)
    { 0x7010b8, 4 },        // last view
    { 0x7070c0, 0x14 },     // alpha and blend caches
    { 0x718738, 0x80 },     // particle vertex buffers, quad indices, the buffer in use
    { 0x71b1d8, 0x30 },     // draw statistics counters
    { 0x71db20, 8 },        // culling flags
    { 0x73457c, 4 },        // frame delta
    { 0x73462c, 4 },        // cars rendered flag
    { 0x7355b4, 0x18 },     // frame arena cursor, limit, overflow
    { 0x7363e8, 8 },        // stream source and indices caches
    { 0x7363fc, 4 },        // draw node counter
    { 0x736404, 0xc },      // last model, its group, current group
    { 0x736414, 8 },        // transform switches
    { 0x736450, 4 },        // quad texture
    { 0x736458, 4 },        // current view
    { 0x7364dc, 4 },        // blink phase
    { 0x73654c, 4 },
    { 0x73ad34, 0xc },      // world and real time
    { 0x78a4e8, 0x384 },    // draw-queue list heads
    { 0x78eb7c, 8 },        // last group and world
};

struct Range
{
    reg32 address, size;
    bool operator<(const Range& o) const { return address < o.address; }
};

struct Settings
{
    bool on;
    bool shadow;
    enum Join { Step, Streamer, Handoff } join;
    bool liveTopology;          // the check's second run uses the topology caches as the step left them
    bool thread;                // shadow_scene_thread: native runs compared, the first on the worker
};

const Settings& settings()
{
    static const Settings s = []
    {
        const char* mode = pf::Config::getString("scene_snapshot", "off");
        const char* join = pf::Config::getString("shadow_scene_snapshot_join", "step");
        Settings r = { strcmp(mode, "serial") == 0, pf::Config::getBool("shadow_scene_snapshot", false),
                       strcmp(join, "streamer") == 0 ? Settings::Streamer
                       : strcmp(join, "handoff") == 0 ? Settings::Handoff : Settings::Step,
                       strcmp(pf::Config::getString("shadow_scene_snapshot_topology", "rewind"), "live") == 0,
                       pf::Config::getBool("shadow_scene_thread", false) };
        // The worker reads the snapshot's state (scenethread.h); its check is the handoff check of native runs.
        if (r.thread)
        {
            r.shadow = true;
            r.join = Settings::Handoff;
        }
        if (r.shadow || scenethread::on())
            r.on = true;
        if (r.on)
            pf::logInfo("scene_snapshot: serial: space nodes updated and the snapshot taken before the views%s",
                        !r.shadow ? "" : r.thread ? ", shadow check of the worker's native views against the game "
                                                    "thread's right after them"
                        : r.join == Settings::Streamer ? ", shadow check at the track streamer call"
                        : r.join == Settings::Handoff ? ", shadow check right after the views"
                                                      : ", shadow check after the step");
        return r;
    }();
    return s;
}

inline bool valid(reg32 address, reg32 size)
{
    return address >= LowestAddress && address < HighestAddress && size < HighestAddress - address;
}

// ---- The ranges ----

struct Collector
{
    std::vector<Range>& ranges;
    std::vector<Range>& keeps;

    void add(reg32 address, reg32 size)
    {
        if (valid(address, size))
            ranges.push_back(Range{address, size});
    }
    void keep(reg32 address, reg32 size)
    {
        if (valid(address, size))
            keeps.push_back(Range{address, size});
    }
};

// A doubly linked list with its sentinel at head (next +0, previous +4): each node's links.
void linkList(Collector& c, reg32 head, reg32 nodeBytes)
{
    c.add(head, 8);
    uint32_t n = 0;
    for (reg32 node = rc::ld32(head); node != head && valid(node, nodeBytes) && n < ListLimit;
         node = rc::ld32(node), ++n)
        c.add(node, nodeBytes);
}

void cars(Collector& c)
{
    const reg32 list = rc::ld32(CarList);
    if (!valid(list, 0x30))
        return;
    c.add(list, 0x30);
    uint32_t n = 0;
    for (reg32 car = rc::ld32(list); car != list && valid(car, 0x900) && n < 64; car = rc::ld32(car), ++n)
    {
        // The collision shape the car shadow reads through 0x472ee0 ([[car+0x3c]+0x10]: count +0x14, 0x30-byte
        // polygons at [+0x54]), moved by the step.
        const reg32 holder = rc::ld32(car + 0x3c);
        if (valid(holder, 0x14))
        {
            const reg32 shape = rc::ld32(holder + 0x10);
            if (valid(shape, 0x58))
            {
                c.add(shape, 0x58);
                const reg32 count = rc::ld32(shape + 0x14), polygons = rc::ld32(shape + 0x54);
                if (count <= 256 && valid(polygons, count * 0x30))
                    c.add(polygons, count * 0x30);
            }
        }
        // The driver [car+0x30] the step's PsychoDriver::Drive 0x45e970 updates: the word +0x1d0 the part culler
        // reads and the reverse level +0x1e4 the head flares read. The state object [car+0x14] (+4 read by the
        // flares and 0x5693b0).
        const reg32 driver = rc::ld32(car + 0x30);
        if (valid(driver, 0x1e8))
            c.add(driver + 0x1d0, 0x18);
        const reg32 state = rc::ld32(car + 0x14);
        if (valid(state, 0x20))
            c.add(state, 0x20);
        // The topology coordinate +0x540..+0x5bf and the cache words at +0x7e0..+0x7ff are not snapshot state
        // (decision (a)).
        c.add(car, 0x540);
        c.keep(car + 0x540, 0x80);
        c.add(car + 0x5c0, 0x220);
        c.keep(car + 0x7e0, 0x20);
        c.add(car + 0x800, 0x100);
    }
}

void worldModels(Collector& c)
{
    c.add(WorldModels, 8);
    uint32_t n = 0;
    for (reg32 node = rc::ld32(WorldModels); node != WorldModels && valid(node, 0x80) && n < ListLimit;
         node = rc::ld32(node), ++n)
    {
        c.add(node, 0x80);
        // Its model (the step's smokeables re-create world models and their models).
        const reg32 model = rc::ld32(node + 8);
        if (model)
            c.add(model, 0x20);
        reg32 space = rc::ld32(node + 0x60);
        if (space && valid(space, 0xf0))
        {
            const reg32 pose = rc::ld32(space + 0xe8);
            if (pose)
                c.add(pose, PoseBytes);
        }
        for (uint32_t depth = 0; space && valid(space, 0xf0) && depth < 16; ++depth, space = rc::ld32(space + 0xe0))
            c.add(space, 0xf0);
    }
}

void particles(Collector& c)
{
    for (const reg32 list : ParticleLists)
    {
        c.add(list, 0x60);
        uint32_t n = 0;
        for (reg32 node = rc::ld32(list); node != list && valid(node, 0x20) && n < ListLimit;
             node = rc::ld32(node))
        {
            c.add(node, 0x20);
            // What the particle draw reads of the system: hash, texture and cell shifts +0x8..+0x1b, life +0x38,
            // mode +0x58, the colour and size curves +0x100..+0x17f.
            const reg32 system = rc::ld32(node + 8);
            if (system)
            {
                c.add(system + 8, 0x14);
                c.add(system + 0x38, 4);
                c.add(system + 0x58, 4);
                c.add(system + 0x100, 0x80);
            }
            const reg32 head = node + 0x14;
            for (reg32 p = rc::ld32(head); p != head && valid(p, 0x20) && n < ListLimit; p = rc::ld32(p), ++n)
                c.add(p, 0x20);
            ++n;
        }
    }
}

// Every clan; the skid links and skids only of the clans whose box RenderClans can draw in a view with a camera
// (the skids the views read; the drawn-skid list's other nodes are only written, by its moves to the front).
void clans(Collector& c)
{
    reg32 views[16];
    uint32_t viewCount = 0;
    for (reg32 v = 0; v < 16; ++v)
        if (rc::ld32(Views + v * 0x60 + 0x40) != 0)
            views[viewCount++] = Views + v * 0x60;
    c.add(Clans, 8);
    c.add(DrawnSkids, 8);
    uint32_t n = 0;
    for (reg32 clan = rc::ld32(Clans); clan != Clans && valid(clan, 0x50) && n < ListLimit;
         clan = rc::ld32(clan), ++n)
    {
        c.add(clan, 0x50);
        bool drawn = false;
        for (uint32_t v = 0; !drawn && v < viewCount; ++v)
            drawn = worldmodels::clanInView(clan, views[v]);
        if (!drawn)
            continue;
        const reg32 head = clan + 8;
        for (reg32 link = rc::ld32(head); link != head && valid(link, 0xc) && n < ListLimit;
             link = rc::ld32(link), ++n)
        {
            c.add(link, 0xc);
            const reg32 skid = rc::ld32(link + 8);
            if (skid)
                c.add(skid, 0x110);
        }
    }
}

// The light groups the light contexts query (lights.cpp query), which the step changes: each group's links, switch
// and entries, each entry's link and light record.
void lightGroups(Collector& c)
{
    c.add(LightGroups, 8);
    uint32_t n = 0;
    for (reg32 group = rc::ld32(LightGroups); group != LightGroups && valid(group, 0x30) && n < ListLimit;
         group = rc::ld32(group), ++n)
    {
        c.add(group, 0x30);
        const reg32 end = group + 0x2c;
        for (reg32 e = rc::ld32(end); e != 0 && e != end && valid(e, 0x40) && n < ListLimit; e = rc::ld32(e), ++n)
            c.add(e, 0x40);
    }
}

// The scenery instance flags of the smokeable spawners: VirginSmokeable 0x5962e0 (in the step) clears the bit 0x10
// of the instance record's byte +0xe that shows the static prop, ~Smokeable 0x5964c0 sets it again.
void spawners(Collector& c)
{
    const reg32 base = rc::ld32(Spawners);
    const reg32 count = rc::ld32(Spawners + 4);
    if (count > 4096 || !valid(base, count * 0x70))
        return;
    for (reg32 i = 0; i < count; ++i)
    {
        const reg32 spawner = base + i * 0x70;
        const sreg32 section = sreg32(int16_t(rc::ld16(spawner + 4)));
        if (section < 0 || section >= 0x1000)
            continue;
        const reg32 table = rc::ld32(SceneryTable + 4 * reg32(section));
        if (!valid(table, 0x24))
            continue;
        const reg32 record = rc::ld32(table + 0x20) + 0x30 * reg32(sreg32(int16_t(rc::ld16(spawner + 6))));
        if (valid(record, 0x30))
            c.add(record + 0xc, 4);
    }
}

// UpdateTextureAnimations 0x545ea0: each animation's current frame (+0xc) and the texture word (+0x18) of the
// texture infos of its frames.
void textureAnimations(Collector& c)
{
    uint32_t n = 0;
    for (reg32 pack = rc::ld32(TextureAnimations); pack != TextureAnimations && valid(pack, 0x14) && n < ListLimit;
         pack = rc::ld32(pack), ++n)
    {
        const x86::sreg32 count = x86::sreg32(rc::ld32(pack + 0x10));
        reg32 record = rc::ld32(pack + 8) + 0x24;
        for (x86::sreg32 i = 0; i < count && valid(record - 8, 0x34) && n < ListLimit; ++i, record += 0x34, ++n)
        {
            c.add(record + 0xc, 4);
            const x86::sreg32 frames = x86::sreg32(rc::ld32(record - 8));
            const reg32 table = rc::ld32(record + 4);
            for (x86::sreg32 f = 0; f < frames && valid(table + 4 + 16 * reg32(f), 4) && n < ListLimit; ++f, ++n)
            {
                const reg32 slot = rc::ld32(table + 4 + 16 * reg32(f));
                if (!valid(slot, 4))
                    continue;
                const reg32 info = rc::ld32(slot);
                if (info)
                    c.add(info + 0x18, 4);
            }
        }
    }
}

void views(Collector& c)
{
    c.add(Views, 16 * 0x60);
    for (reg32 v = 0; v < 16; ++v)
    {
        const reg32 camera = rc::ld32(Views + v * 0x60 + 0x40);
        if (camera)
        {
            c.add(camera, 0x100);
            c.add(camera + 0x280, 0x10);
        }
    }
}

// Widened to whole 64-byte lines (fewer ranges and cache misses: PARALLEL_SCENE.md 6a), sorted and merged; keeps
// take precedence (cut out of the ranges).
void normalize(std::vector<Range>& ranges, std::vector<Range>& keeps)
{
    std::sort(keeps.begin(), keeps.end());
    for (Range& r : ranges)
    {
        const reg32 end = (r.address + r.size + Line - 1) & ~(Line - 1);
        r.address &= ~(Line - 1);
        r.size = end - r.address;
    }
    std::sort(ranges.begin(), ranges.end());
    std::vector<Range> merged;
    for (const Range& r : ranges)
    {
        if (!merged.empty() && r.address <= merged.back().address + merged.back().size)
        {
            const reg32 end = std::max(merged.back().address + merged.back().size, r.address + r.size);
            merged.back().size = end - merged.back().address;
        }
        else
            merged.push_back(r);
    }
    ranges.clear();
    size_t k = 0;
    for (Range r : merged)
    {
        while (k < keeps.size() && keeps[k].address + keeps[k].size <= r.address)
            ++k;
        size_t j = k;
        while (r.size && j < keeps.size() && keeps[j].address < r.address + r.size)
        {
            const Range& cut = keeps[j];
            if (cut.address > r.address)
                ranges.push_back(Range{r.address, cut.address - r.address});
            const reg32 end = r.address + r.size, cutEnd = cut.address + cut.size;
            if (cutEnd >= end)
                r.size = 0;
            else
            {
                r.size = end - std::max(cutEnd, r.address);
                r.address = std::max(cutEnd, r.address);
            }
            ++j;
        }
        if (r.size)
            ranges.push_back(r);
    }
}

// Bytes per owner before merging, summed over the frames: image, cars, world models, particles, clans and skids,
// lists and texture animations, views and cameras.
const int OwnerCount = 7;
uint64_t s_ownerBytes[OwnerCount] = {};

struct Snapshot
{
    std::vector<Range> ranges, keeps;
    std::vector<uint8_t> data;
    uint32_t bytes = 0;
    uint32_t pages = 0;     // distinct 4 KB pages of the ranges

    void build()
    {
        ranges.clear();
        keeps.clear();
        Collector c{ranges, keeps};
        size_t mark = 0;
        const auto owner = [&](int i)
        {
            for (size_t k = mark; k < ranges.size(); ++k)
                s_ownerBytes[i] += ranges[k].size;
            mark = ranges.size();
        };
        for (const auto& r : ImageRanges)
            c.add(r.address, r.size);
        owner(0);
        cars(c);
        owner(1);
        worldModels(c);
        spawners(c);
        owner(2);
        particles(c);
        owner(3);
        clans(c);
        owner(4);
        linkList(c, Regions, 8);
        linkList(c, TexturePacks, 8);
        lightGroups(c);
        textureAnimations(c);
        owner(5);
        views(c);
        owner(6);
        normalize(ranges, keeps);
        bytes = 0;
        pages = 0;
        reg32 lastPage = 0;
        for (const Range& r : ranges)
        {
            bytes += r.size;
            const reg32 first = r.address >> 12, last = (r.address + r.size - 1) >> 12;
            pages += last - first + 1 - (pages != 0 && first == lastPage ? 1 : 0);
            lastPage = last;
        }
    }
    // Live memory of the ranges into out.
    void gather(std::vector<uint8_t>& out, const std::vector<Range>& from) const
    {
        uint32_t total = 0;
        for (const Range& r : from)
            total += r.size;
        out.resize(total);
        uint8_t* p = out.data();
        for (const Range& r : from)
        {
            memcpy(p, rc::host(r.address), r.size);
            p += r.size;
        }
    }
    static void scatter(const std::vector<uint8_t>& in, const std::vector<Range>& to)
    {
        const uint8_t* p = in.data();
        for (const Range& r : to)
        {
            memcpy(rc::host(r.address), p, r.size);
            p += r.size;
        }
    }
};

Snapshot s_snapshot;

struct Stats
{
    uint32_t frames = 0;
    uint64_t bytes = 0, ranges = 0, us = 0, pages = 0;
    uint32_t maxBytes = 0, maxRanges = 0, maxUs = 0, maxPages = 0;
    uint32_t nodesUpdated = 0;
    uint32_t viewUpdates = 0;       // updates the views still made
};
Stats s_stats;

struct Shadow
{
    bridge::Capture capture;
    x86::CPU entry;
    reg32 returnAddress = 0;
    reg32 gameState = 0;
    reg32 carList = 0;
    bool pending = false;
    std::vector<uint8_t> live, kept, liveRewinds;
    // Memory only the views read and write that carries over between their runs, not snapshot state but put back
    // for the second run: the views' stack below their entry (unwritten padding the views copy into vertices) and
    // the cars' CarRenderInfo (render state carried from frame to frame).
    std::vector<Range> rewinds;
    std::vector<uint8_t> rewindData;
    std::vector<uint8_t> stack, liveStack;  // StackRewind bytes below the entry esp
    uint32_t checks = 0, differing = 0, logged = 0, skipped = 0, dropped = 0, unjournaled = 0, relocated = 0;
    uint64_t topologyStores = 0;    // topology stores since the handoff undone for the checks
    uint32_t frame = 0;
};
Shadow s_shadow;

bool s_inViews = false;
// Instance flag words StuffScenery read in the record run, with the value at the read, in order.
std::vector<std::pair<reg32, uint16_t>> s_flags;
std::vector<uint16_t> s_liveFlags;
// Node check frames: the nodes the views update themselves (with the update before the views skipped).
std::vector<reg32>* s_updated = nullptr;
struct NodeCheck
{
    std::vector<reg32> expected, got;
    uint32_t frames = 0, differing = 0, logged = 0, nodes = 0;
};
NodeCheck s_nodes;
// The views' entry (phases.cpp: the views with their timing scope).
Function s_viewsRun = nullptr;

const char* shadowName()
{
    return settings().thread ? "shadow_scene_thread" : "shadow_scene_snapshot";
}

void check(win32::WinApplication* app, x86::CPU& cpu)
{
    Shadow& s = s_shadow;
    s.pending = false;
    if (bridge::capturing() || rc::ld32(GameState) != s.gameState || rc::ld32(CarList) != s.carList
        || !bench::racing())
    {
        coll::journalEnd();
        ++s.skipped;
        return;
    }
    // The views' frame at the handoff's place, below the current stack (both joins are calls of MainLoop, as the
    // frame render is); else as far below the current esp with the same offset in its page.
    x86::CPU c = s.entry;
    if (c.esp + 4 > cpu.esp)
    {
        c.esp = ((cpu.esp - 0x2000) & ~0xfffu) | (s.entry.esp & 0xfffu);
        ++s.relocated;
    }
    const reg32 stackBase = c.esp - StackRewind;

    // The live bytes first, then the handoff's: the topology stores since the handoff undone (decision (a): the
    // views' own topology state, not the snapshot's), CarRenderInfo, the stack, the instance flags, the snapshot.
    const Snapshot& snap = s_snapshot;
    snap.gather(s.live, snap.ranges);
    snap.gather(s.kept, snap.keeps);
    snap.gather(s.liveRewinds, s.rewinds);
    s.liveStack.assign(rc::host(stackBase), rc::host(stackBase + StackRewind + 4));
    s_liveFlags.resize(s_flags.size());
    for (size_t i = 0; i < s_flags.size(); ++i)
        s_liveFlags[i] = rc::ld16(s_flags[i].first);
    if (settings().liveTopology)
        coll::journalBegin();
    else
        s.topologyStores += coll::journalRewind();
    Snapshot::scatter(s.rewindData, s.rewinds);
    memcpy(rc::host(stackBase), s.stack.data(), StackRewind);
    rc::st32(c.esp, s.returnAddress);
    for (size_t i = s_flags.size(); i-- > 0;)
        rc::st16(s_flags[i].first, s_flags[i].second);
    Snapshot::scatter(snap.data, snap.ranges);
    s.capture.next = 0;
    s.capture.replayed = 0;
    s.capture.differs = false;
    s.capture.continueAfterDifference = true;
    s.capture.differingCalls = 0;
    s.capture.differingByMethod.clear();
    bridge::begin(s.capture, bridge::Capture::Replay);
    s_inViews = true;
    s_viewsRun(app, c);
    s_inViews = false;
    bridge::end();
    s.unjournaled += coll::journalRollback();
    memcpy(rc::host(stackBase), s.liveStack.data(), StackRewind + 4);
    for (size_t i = s_flags.size(); i-- > 0;)
        rc::st16(s_flags[i].first, s_liveFlags[i]);
    Snapshot::scatter(s.liveRewinds, s.rewinds);
    Snapshot::scatter(s.kept, snap.keeps);
    Snapshot::scatter(s.live, snap.ranges);
    if (c.terminate)
    {
        cpu.terminate = true;
        return;
    }

    ++s.checks;
    const bridge::Capture& cap = s.capture;
    const bool same = !cap.differs && cap.next == cap.calls.size();
    if (same)
        return;
    ++s.differing;
    if (s.logged >= ShadowLogLimit)
        return;
    ++s.logged;
    if (cap.differs && cap.differingCall < cap.calls.size())
    {
        // The calls before it and the differing words, for finding the code that made it.
        char text[600];
        size_t at = 0;
        const uint32_t k = cap.differingCall;
        for (uint32_t i = k > 6 ? k - 6 : 0; i <= k && at < sizeof(text); ++i)
        {
            const bridge::Capture::Call& call = cap.calls[i];
            at += snprintf(text + at, sizeof(text) - at, "%s%s(", i ? " " : "", bridge::methodName(call.method));
            for (uint32_t w = 0; w < call.count && w < (i == k ? 40u : 3u) && at < sizeof(text); ++w)
                at += snprintf(text + at, sizeof(text) - at, "%s%x", w ? "," : "", unsigned(cap.words[call.first + w]));
            if (at < sizeof(text))
                at += snprintf(text + at, sizeof(text) - at, ")");
        }
        pf::logInfo("%s: recorded %s", shadowName(), text);
        at = 0;
        for (uint32_t w = 0; w < cap.gotWords.size() && w < 40 && at < sizeof(text); ++w)
            at += snprintf(text + at, sizeof(text) - at, "%s%x", w ? "," : "", unsigned(cap.gotWords[w]));
        pf::logInfo("%s: got (%s)", shadowName(), text);
        at = 0;
        for (uint32_t m = 0; m < cap.differingByMethod.size() && at < sizeof(text); ++m)
            if (cap.differingByMethod[m])
                at += snprintf(text + at, sizeof(text) - at, " %s %u", bridge::methodName(uint8_t(m)),
                               unsigned(cap.differingByMethod[m]));
        pf::logInfo("%s: %u calls differ:%s", shadowName(), unsigned(cap.differingCalls), at ? text : "");
    }
    if (cap.differs)
        pf::logInfo("%s: frame %u: call %u of %u: expected %s, got %s, word %u: 0x%08x / 0x%08x",
                    shadowName(), unsigned(s.frame), unsigned(cap.differingCall), unsigned(cap.calls.size()),
                    bridge::methodName(cap.expected), bridge::methodName(cap.got), unsigned(cap.differingWord),
                    unsigned(cap.expectedWord), unsigned(cap.gotWord));
    else
        pf::logInfo("%s: frame %u: %u of %u calls made", shadowName(), unsigned(s.frame), unsigned(cap.next),
                    unsigned(cap.calls.size()));
}

}

void views(win32::WinApplication* app, x86::CPU& cpu, Function run)
{
    const Settings& set = settings();
    if (!set.on)
    {
        run(app, cpu);
        return;
    }
    s_viewsRun = run;
    Shadow& sh = s_shadow;
    if (sh.pending)
    {
        sh.pending = false;
        coll::journalEnd();
        ++sh.dropped;
    }
    ++sh.frame;
    const uint32_t start = pf::perf::now();
    // Decision (b): the space nodes the views would update, updated first. With the shadow, every second frame
    // only lists them and checks the list against the nodes the views then update themselves.
    const reg32 view = PlayerView;
    const bool drawn = rc::ld8(view + 8) != 0 && rc::ld32(rc::ld32(view + 0x58) + 0x10) != 0;
    if (set.shadow && (sh.frame & 1) && !bridge::capturing() && bench::racing())
    {
        NodeCheck& n = s_nodes;
        n.expected.clear();
        n.got.clear();
        if (drawn)
            worldmodels::updateNodes(view, &n.expected);
        worldmodels::listViewUpdates(&n.got);
        s_updated = &n.got;
        s_inViews = true;
        scenethread::run(app, cpu, run);
        s_inViews = false;
        s_updated = nullptr;
        worldmodels::listViewUpdates(nullptr);
        worldmodels::takeViewUpdates();
        ++n.frames;
        n.nodes += uint32_t(n.expected.size());
        if (n.expected != n.got)
        {
            ++n.differing;
            if (n.logged < ShadowLogLimit)
            {
                ++n.logged;
                pf::logInfo("shadow_scene_snapshot: frame %u: space nodes listed %u, updated by the views %u (first "
                            "0x%08x / 0x%08x)", unsigned(sh.frame), unsigned(n.expected.size()),
                            unsigned(n.got.size()), unsigned(n.expected.empty() ? 0 : n.expected[0]),
                            unsigned(n.got.empty() ? 0 : n.got[0]));
            }
        }
        return;
    }
    if (drawn)
        s_stats.nodesUpdated += worldmodels::updateNodes(view);
    Snapshot& snap = s_snapshot;
    snap.build();
    snap.gather(snap.data, snap.ranges);
    const uint32_t us = pf::perf::now() - start;
    Stats& st = s_stats;
    ++st.frames;
    st.bytes += snap.bytes;
    st.ranges += snap.ranges.size();
    st.us += us;
    st.pages += snap.pages;
    st.maxPages = std::max(st.maxPages, snap.pages);
    st.maxBytes = std::max(st.maxBytes, snap.bytes);
    st.maxRanges = std::max(st.maxRanges, uint32_t(snap.ranges.size()));
    st.maxUs = std::max(st.maxUs, us);
    worldmodels::takeViewUpdates();

    if (!set.shadow || bridge::capturing() || !bench::racing())
    {
        s_inViews = true;
        scenethread::run(app, cpu, run);
        s_inViews = false;
        st.viewUpdates += worldmodels::takeViewUpdates();
        return;
    }
    sh.entry = cpu;
    sh.rewinds.clear();
    sh.stack.assign(rc::host(cpu.esp - StackRewind), rc::host(cpu.esp));
    {
        const reg32 list = rc::ld32(CarList);
        uint32_t n = 0;
        for (reg32 car = valid(list, 4) ? rc::ld32(list) : list; car != list && valid(car, 0x900) && n < 64;
             car = rc::ld32(car), ++n)
        {
            const reg32 info = rc::ld32(car + 0x4d4);
            if (valid(info, CarRenderInfoSize))
                sh.rewinds.push_back(Range{info, CarRenderInfoSize});
        }
    }
    snap.gather(sh.rewindData, sh.rewinds);
    sh.returnAddress = rc::ld32(cpu.esp);
    sh.gameState = rc::ld32(GameState);
    sh.carList = rc::ld32(CarList);

    sh.capture.clear();
    sh.capture.guestCalls = false;
    sh.capture.frameVertices = true;
    sh.capture.natives = set.thread;
    s_flags.clear();
    coll::journalBegin();
    bridge::begin(sh.capture, bridge::Capture::Record);
    s_inViews = true;
    g_noteFlags = true;
    scenethread::run(app, cpu, run);
    g_noteFlags = false;
    s_inViews = false;
    bridge::end();
    st.viewUpdates += worldmodels::takeViewUpdates();
    sh.pending = !cpu.terminate;
    if (sh.pending && set.join == Settings::Handoff)
        check(app, cpu);
}

void afterStep(win32::WinApplication* app, x86::CPU& cpu)
{
    if (__builtin_expect(s_shadow.pending, 0) && settings().join == Settings::Step)
        check(app, cpu);
}

void beforeStreamer(win32::WinApplication* app, x86::CPU& cpu)
{
    if (__builtin_expect(s_shadow.pending, 0) && settings().join == Settings::Streamer)
        check(app, cpu);
}

bool g_noteFlags = false;

void noteFlagSlow(x86::reg32 address)
{
    s_flags.push_back(std::make_pair(address, uint16_t(rc::ld16(address))));
}

void spaceNodeUpdate(x86::reg32 node)
{
    if (!s_inViews)
        return;
    if (s_updated)
        s_updated->push_back(node);
    else
        ++s_stats.viewUpdates;
}

void logTotal()
{
    const Settings& set = settings();
    if (!set.on)
        return;
    const Stats& st = s_stats;
    const uint32_t f = st.frames ? st.frames : 1;
    pf::logInfo("scene_snapshot: %u frames, snapshot avg %u bytes (max %u) in %u ranges (max %u) on %u 4 KB pages (max "
                "%u), %.1f us avg (max %u); space nodes updated first %.2f per frame, updated by the views %u",
                unsigned(st.frames), unsigned(st.bytes / f), unsigned(st.maxBytes), unsigned(st.ranges / f),
                unsigned(st.maxRanges), unsigned(st.pages / f), unsigned(st.maxPages), double(st.us) / f,
                unsigned(st.maxUs), double(st.nodesUpdated) / f, unsigned(st.viewUpdates));
    const uint64_t* o = s_ownerBytes;
    pf::logInfo("scene_snapshot: bytes per frame before merging: image %u, cars %u, world models %u, particles %u, "
                "clans and skids %u, lists %u, views and cameras %u", unsigned(o[0] / f), unsigned(o[1] / f),
                unsigned(o[2] / f), unsigned(o[3] / f), unsigned(o[4] / f), unsigned(o[5] / f), unsigned(o[6] / f));
    if (set.shadow)
    {
        const Shadow& s = s_shadow;
        pf::logInfo("%s: %u frames checked, %u differ; %u skipped, %u without a join, %u on a moved stack; topology "
                    "queries outside the journal %u, topology stores undone %.1f per check (%s)", shadowName(),
                    unsigned(s.checks), unsigned(s.differing), unsigned(s.skipped), unsigned(s.dropped),
                    unsigned(s.relocated), unsigned(s.unjournaled),
                    double(s.topologyStores) / double(s.checks ? s.checks : 1), set.liveTopology ? "live" : "rewind");
        const NodeCheck& n = s_nodes;
        pf::logInfo("shadow_scene_snapshot: space node check: %u frames, %u differ, %u nodes listed",
                    unsigned(n.frames), unsigned(n.differing), unsigned(n.nodes));
    }
}

}}
