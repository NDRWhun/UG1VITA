// Hitch log (hitch.h): records of the game thread's work per begun frame, matched with the render thread's slow
// presents.

#include "hitch.h"

#include "bench.h"
#include "phases.h"

#include <lib/memmap.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <platform/perf.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace nfsu { namespace hitch {

namespace
{

namespace perf = pf::perf;

// Game flow state: 3 in the front end, 6 from the track load's end to the race's end.
const x86::reg32 GameFlowState = 0x0077A920;
const uint32_t FlowFrontEnd = 3;

const uint32_t RecordSlots = 8;
const uint32_t LineSize = 1024;
// Kept lines: about 280 at most; written in one block (a line written on its own costs about 20 ms of file I/O).
const uint32_t KeptBytes = 256 * 1024;
const char LinePrefix[] = "[info] ";
// No cause is named below this excess over the running average.
const float MinExcessUs = 1000.0f;
// Each new value's part of a running average.
const float AverageWeight = 1.0f / 32.0f;

enum Cause
{
    // Game thread phases, each without the phases timed inside it.
    CausePhysics, CauseMellow, CauseAi, CauseStepRest, CauseCameras, CauseSound, CauseStreamer, CauseLoading,
    CauseCull, CauseStuff, CauseFlush, CauseFlares, CauseCars, CauseParticleDraw, CauseViewsRest, CauseFe,
    CauseRenderRest, CauseGameRest,
    // Game thread lock releases by reason.
    CauseFileWait, CauseCritSecWait, CauseObjectWait, CauseSleep, CauseFrameSlot, CausePreempted, CauseReleasedOther,
    // Waits and load calls inside the phases.
    CauseD3dQueue, CauseUploadWait, CauseReadBack, CauseGuestBlocks, CauseJdlz, CauseTextureCreate, CauseSkin,
    CauseGpuGrowth,
    // Writing hitch lines.
    CauseHitchLog,
    // Render thread parts.
    CauseResolve, CauseRenderPlan, CausePrograms, CauseListRecord, CauseListWait, CauseRenderUploadWait,
    CauseScenes, CauseRenderOffCpu, CauseRenderThreadRest, CauseDisplayQueue,
    CauseCount,
    CauseUnclear = CauseCount,  // no cause over MinExcessUs
    CauseNoRecord,              // no game frame record: a layer present, or the record is gone
    CauseSlots
};
const int FirstRelease = CauseFileWait;
const int FirstInside = CauseD3dQueue;
const int LastInside = CauseGpuGrowth;
const int FirstRender = CauseResolve;

const char* const CauseNames[CauseSlots] = {
    "physics", "mellow", "ai", "step_rest", "cameras", "sound", "streamer", "loading", "cull", "stuff", "flush",
    "flares", "cars", "particle_draw", "views_rest", "fe", "render_rest", "game_rest",
    "file_wait", "cs_wait", "object_wait", "sleep", "frame_slot", "preempted", "released_other",
    "d3d_queue", "upload_wait", "read_back", "guest_blocks", "jdlz", "texture_create", "skin", "gpu_growth",
    "hitch_log",
    "resolve", "render_plan", "programs", "list_record", "list_wait", "render_upload_wait", "scenes",
    "render_off_cpu", "render_thread_rest", "display_queue",
    "unclear", "no_game_record",
};
static_assert(sizeof(CauseNames) / sizeof(CauseNames[0]) == CauseSlots, "a name per cause");

// Phases in the order of the log line; ai is AiCar and AiDriver together.
struct ShownPhase
{
    phases::Id id;
    const char* name;
};
const ShownPhase ShownPhases[] = {
    {phases::Step, "step"}, {phases::PhysicsMove, "physics"}, {phases::MellowMove, "mellow"}, {phases::AiCar, "ai"},
    {phases::Cameras, "cameras"}, {phases::Sound, "sound"}, {phases::Streamer, "streamer"},
    {phases::Loading, "loading"}, {phases::Render, "render"}, {phases::Views, "views"}, {phases::Cull, "cull"},
    {phases::Stuff, "stuff"}, {phases::Flush, "flush"}, {phases::Flares, "flares"}, {phases::Cars, "cars"},
    {phases::ParticleDraw, "particle_draw"}, {phases::FrontEnd, "fe"},
};

const uint32_t TotalsWords = sizeof(perf::Totals) / sizeof(uint32_t);
static_assert(sizeof(perf::Totals) % sizeof(uint32_t) == 0, "Totals holds uint32_t fields only");

// The game thread's work from the end of the frame render that began the previous record's frame to the end of the
// frame render that began this one's: the work that produced frames firstFrame..frame.
struct Record
{
    uint32_t firstFrame = 0;    // render::Frame indexes begun in it
    uint32_t frame = 0;
    uint32_t wallUs = 0;
    uint32_t renders = 0;       // frame renders (0x40A580)
    uint32_t logUs = 0;         // writing hitch lines
    bool racing = false;
    uint32_t raceUs = 0;        // since the race started, when racing
    uint32_t flow = 0;          // game flow state
    uint32_t coordinator = 0;   // race coordinator state and race stage
    uint32_t stage = 0;
    uint32_t phase[phases::Count] = {};
    perf::Totals delta = {};
    float excess[FirstRender] = {};     // over the running averages before this record
};

struct Line
{
    char text[LineSize] = {};
    int length = 0;

    __attribute__((format(printf, 2, 3))) void append(const char* format, ...)
    {
        if (length >= int(sizeof(text)))
            return;
        va_list args;
        va_start(args, format);
        length += vsnprintf(text + length, sizeof(text) - length, format, args);
        va_end(args);
    }
};

bool s_started = false;
bool s_racing = false;
uint32_t s_raceStartUs = 0;
uint32_t s_lastEndUs = 0;
perf::Totals s_lastTotals = {};
Record s_current;
Record s_records[RecordSlots];
uint32_t s_recordCount = 0;
uint32_t s_lastFrame = 0;               // frame of the newest record
float s_average[FirstRender] = {};
uint32_t s_pendingLogUs = 0;            // writing time not yet in a record

char s_kept[KeptBytes];
uint32_t s_keptBytes = 0;
uint32_t s_keptLines = 0;
uint32_t s_droppedLines = 0;            // not kept: the space was full

uint32_t s_counts[CauseSlots] = {};
uint32_t s_slow = 0;
uint32_t s_droppedAtReset = 0;

inline double ms(uint32_t us)
{
    return double(us) / 1000.0;
}

inline uint32_t rest(uint32_t total, uint32_t parts)
{
    return total > parts ? total - parts : 0;
}

uint32_t guestDword(x86::reg32 address)
{
    return *reinterpret_cast<const uint32_t*>(win32::g_guestBase + address);
}

void addDelta(perf::Totals& sum, const perf::Totals& now, const perf::Totals& before)
{
    uint32_t* s = reinterpret_cast<uint32_t*>(&sum);
    const uint32_t* n = reinterpret_cast<const uint32_t*>(&now);
    const uint32_t* b = reinterpret_cast<const uint32_t*>(&before);
    for (uint32_t i = 0; i < TotalsWords; ++i)
        s[i] += n[i] - b[i];
}

uint32_t released(const perf::Totals& t)
{
    uint32_t sum = 0;
    for (int i = 0; i < perf::BlockCount; ++i)
        sum += t.blocked[i];
    return sum;
}

// The game thread causes of a record in microseconds.
void gameCauses(const Record& r, float* v)
{
    const uint32_t* p = r.phase;
    const perf::Totals& t = r.delta;
    const uint32_t ai = p[phases::AiCar] + p[phases::AiDriver];
    v[CausePhysics] = float(p[phases::PhysicsMove]);
    v[CauseMellow] = float(p[phases::MellowMove]);
    v[CauseAi] = float(ai);
    v[CauseStepRest] = float(rest(p[phases::Step], p[phases::PhysicsMove] + p[phases::MellowMove] + ai));
    v[CauseCameras] = float(p[phases::Cameras]);
    v[CauseSound] = float(p[phases::Sound]);
    v[CauseStreamer] = float(p[phases::Streamer]);
    v[CauseLoading] = float(p[phases::Loading]);
    v[CauseCull] = float(p[phases::Cull]);
    v[CauseStuff] = float(p[phases::Stuff]);
    v[CauseFlush] = float(p[phases::Flush]);
    v[CauseFlares] = float(p[phases::Flares]);
    v[CauseCars] = float(p[phases::Cars]);
    v[CauseParticleDraw] = float(p[phases::ParticleDraw]);
    const uint32_t drawn = p[phases::Cull] + p[phases::Stuff] + p[phases::Flush] + p[phases::Flares] + p[phases::Cars]
                         + p[phases::ParticleDraw];
    v[CauseViewsRest] = float(rest(p[phases::Views], drawn));
    v[CauseFe] = float(p[phases::FrontEnd]);
    v[CauseRenderRest] = float(rest(p[phases::Render], p[phases::Views] + p[phases::FrontEnd]));
    const uint32_t top = p[phases::Step] + p[phases::Cameras] + p[phases::Sound] + p[phases::Streamer]
                       + p[phases::Loading] + p[phases::Render];
    v[CauseGameRest] = float(rest(r.wallUs, released(t) + top + r.logUs));
    const uint32_t slot = t.events[perf::EventSlotWaitUs];
    v[CauseFileWait] = float(t.blocked[perf::BlockFile]);
    v[CauseCritSecWait] = float(t.blocked[perf::BlockCritSec]);
    v[CauseObjectWait] = float(t.blocked[perf::BlockWait]);
    v[CauseSleep] = float(t.blocked[perf::BlockSleep]);
    v[CauseFrameSlot] = float(slot);
    v[CausePreempted] = float(t.blocked[perf::BlockPreempt]);
    // The frame slot wait is a release of the other kind.
    v[CauseReleasedOther] = float(rest(t.blocked[perf::BlockOther] + t.blocked[perf::BlockSwap], slot));
    v[CauseD3dQueue] = float(t.events[perf::EventDrainWaitUs]);
    v[CauseUploadWait] = float(t.uploads[perf::UploadGameWaitUs]);
    v[CauseReadBack] = float(t.uploads[perf::UploadReadBackUs]);
    v[CauseGuestBlocks] = float(t.uploads[perf::UploadGuestBlockUs]);
    v[CauseJdlz] = float(t.events[perf::EventJdlzUs]);
    v[CauseTextureCreate] = float(t.events[perf::EventTextureCreateUs]);
    v[CauseSkin] = float(t.events[perf::EventSkinUs]);
    v[CauseGpuGrowth] = float(t.events[perf::EventGpuGrowthUs]);
    v[CauseHitchLog] = float(r.logUs);
}

// The render thread causes of a present (render, queue: its times; parts: its parts), from v[FirstRender].
void renderCauses(uint32_t render, uint32_t queue, const uint32_t* parts, float* v)
{
    const uint32_t lists = parts[perf::RenderLists], listWait = parts[perf::RenderListWait];
    const uint32_t uploadWait = parts[perf::RenderUploadWait];
    v[CauseResolve] = float(parts[perf::RenderResolve]);
    v[CauseRenderPlan] = float(parts[perf::RenderPlan]);
    v[CausePrograms] = float(parts[perf::RenderPrograms]);
    v[CauseListRecord] = float(rest(lists, listWait));
    v[CauseListWait] = float(listWait);
    v[CauseRenderUploadWait] = float(uploadWait);
    v[CauseScenes] = float(parts[perf::RenderScenes]);
    // Time the thread neither ran nor waited in one of the waits above.
    v[CauseRenderOffCpu] = float(rest(rest(render, queue + listWait + uploadWait), parts[perf::RenderCpu]));
    v[CauseRenderThreadRest] = float(rest(render, queue + parts[perf::RenderResolve] + parts[perf::RenderPlan]
                                                 + parts[perf::RenderPrograms] + lists + uploadWait
                                                 + parts[perf::RenderScenes]));
    v[CauseDisplayQueue] = float(queue);
}

const Record* findRecord(uint32_t frame)
{
    const uint32_t count = std::min(s_recordCount, RecordSlots);
    for (uint32_t i = 0; i < count; ++i)
    {
        const Record& r = s_records[(s_recordCount - 1 - i) % RecordSlots];
        if (int32_t(frame - r.firstFrame) >= 0 && int32_t(r.frame - frame) >= 0)
            return &r;
    }
    return nullptr;
}

// The cause with the largest excess; a wait or load call inside the phases replaces a phase when its excess is at
// least half of the phase's.
int dominant(const float* excess, float* largest)
{
    int best = -1;
    float bestExcess = MinExcessUs;
    int inside = -1;
    float insideExcess = 0.0f;
    for (int c = 0; c < CauseCount; ++c)
    {
        if (excess[c] > bestExcess)
        {
            best = c;
            bestExcess = excess[c];
        }
        if (c >= FirstInside && c <= LastInside && excess[c] > insideExcess)
        {
            inside = c;
            insideExcess = excess[c];
        }
    }
    if (best < 0)
    {
        *largest = 0.0f;
        return CauseUnclear;
    }
    if (best < FirstRelease && inside >= 0 && insideExcess * 2.0f >= bestExcess)
    {
        best = inside;
        bestExcess = insideExcess;
    }
    *largest = bestExcess;
    return best;
}

void appendWhen(Line& line, bool racing, uint32_t raceUs, uint32_t flow, uint32_t coordinator, uint32_t stage)
{
    if (racing)
        line.append("race %.2f s", double(raceUs) / 1000000.0);
    else if (flow == FlowFrontEnd)
        line.append("front end (flow 3)");
    else
        line.append("not racing (flow %d, race coordinator %d, race stage %d)", int(flow), int(coordinator),
                    int(stage));
}

void appendRender(Line& line, const perf::SlowPresent& p)
{
    const uint32_t* q = p.parts;
    const uint32_t* a = p.partAvgs;
    line.append(" | frame %u interval %.1f ms, render %.1f (avg %.1f): resolve %.1f plan %.1f programs %.1f"
                " (%u new %.1f) lists %.1f (wait %.1f, steals %u won %u) tex waits %.1f scenes %.1f queue %.1f,"
                " cpu %.1f (avg %.1f); scenes %u draws %u (avg %u %u)",
                unsigned(p.frame), ms(p.intervalUs), ms(p.renderUs), ms(p.renderAvgUs), ms(q[perf::RenderResolve]),
                ms(q[perf::RenderPlan]), ms(q[perf::RenderPrograms]), unsigned(q[perf::RenderProgramCreates]),
                ms(q[perf::RenderProgramCreateUs]), ms(q[perf::RenderLists]), ms(q[perf::RenderListWait]),
                unsigned(q[perf::RenderListSteals]), unsigned(q[perf::RenderListStealsWon]),
                ms(q[perf::RenderUploadWait]), ms(q[perf::RenderScenes]), ms(p.queueUs), ms(q[perf::RenderCpu]),
                ms(a[perf::RenderCpu]), unsigned(q[perf::RenderSceneCount]), unsigned(q[perf::RenderDrawCount]),
                unsigned(a[perf::RenderSceneCount]), unsigned(a[perf::RenderDrawCount]));
}

void appendRecord(Line& line, const Record& r)
{
    const perf::Totals& t = r.delta;
    const uint32_t* p = r.phase;
    const uint32_t slot = t.events[perf::EventSlotWaitUs];
    const uint32_t others = t.held[perf::HolderTimer] + t.held[perf::HolderIo] + t.held[perf::HolderHelper];
    line.append(" | game %.1f ms in %u render%s, released %.1f (file %.1f cs %.1f wait %.1f sleep %.1f preempt %.1f"
                " other %.1f; %ux relock %.1f), slot %.1f (fence %.1f), others held %.1f, preempt waits %u %.1f",
                ms(r.wallUs), unsigned(r.renders), r.renders == 1 ? "" : "s", ms(released(t)),
                ms(t.blocked[perf::BlockFile]), ms(t.blocked[perf::BlockCritSec]), ms(t.blocked[perf::BlockWait]),
                ms(t.blocked[perf::BlockSleep]), ms(t.blocked[perf::BlockPreempt]),
                ms(t.blocked[perf::BlockOther] + t.blocked[perf::BlockSwap]), unsigned(t.releases), ms(t.relockUs),
                ms(slot), ms(t.events[perf::EventFenceWaitUs]), ms(others), unsigned(t.preemptWaits),
                ms(t.preemptWaitUs));
    line.append(" | phases");
    for (const ShownPhase& shown : ShownPhases)
    {
        const uint32_t us = shown.id == phases::AiCar ? p[phases::AiCar] + p[phases::AiDriver] : p[shown.id];
        line.append(" %s %.1f", shown.name, ms(us));
    }
    line.append(" | uploads %u %.0f KB (waits game %u %.1f render %u %.1f), read-backs %u %.0f KB %.1f,"
                " locks tex %u buf %u, guest blocks %u %.1f",
                unsigned(t.uploads[perf::UploadLevels]), double(t.uploads[perf::UploadBytes]) / 1024.0,
                unsigned(t.uploads[perf::UploadGameWaits]), ms(t.uploads[perf::UploadGameWaitUs]),
                unsigned(t.uploads[perf::UploadRenderWaits]), ms(t.uploads[perf::UploadRenderWaitUs]),
                unsigned(t.uploads[perf::UploadReadBacks]), double(t.uploads[perf::UploadReadBackBytes]) / 1024.0,
                ms(t.uploads[perf::UploadReadBackUs]), unsigned(t.events[perf::EventTextureLocks]),
                unsigned(t.events[perf::EventBufferLocks]), unsigned(t.uploads[perf::UploadGuestBlocks]),
                ms(t.uploads[perf::UploadGuestBlockUs]));
    line.append(" | file reads %u %.0f KB %.1f | jdlz %u %.1f, tex creates %u %.1f, skins %u %.1f"
                " | d3d waits %u %.1f, queue run %.1f",
                unsigned(t.events[perf::EventFileReads]), double(t.events[perf::EventFileBytes]) / 1024.0,
                ms(t.events[perf::EventFileUs]), unsigned(t.events[perf::EventJdlz]), ms(t.events[perf::EventJdlzUs]),
                unsigned(t.events[perf::EventTextureCreates]), ms(t.events[perf::EventTextureCreateUs]),
                unsigned(t.events[perf::EventSkins]), ms(t.events[perf::EventSkinUs]),
                unsigned(t.events[perf::EventDrainWaits]), ms(t.events[perf::EventDrainWaitUs]),
                ms(t.events[perf::EventQueueRunUs]));
    if (t.events[perf::EventGpuGrowths])
        line.append(" | gpu grown %ux %u KB %.1f",unsigned(t.events[perf::EventGpuGrowths]),
                    unsigned(t.events[perf::EventGpuGrowthKb]), ms(t.events[perf::EventGpuGrowthUs]));
    if (r.logUs)
        line.append(" | hitch log %.1f", ms(r.logUs));
}

// Keeps a line as the log writes it.
void keep(const char* text, int length)
{
    const uint32_t prefix = sizeof(LinePrefix) - 1;
    const uint32_t size = uint32_t(std::min(length, int(LineSize) - 1));
    if (s_keptBytes + prefix + size + 1 > KeptBytes)
    {
        ++s_droppedLines;
        return;
    }
    memcpy(s_kept + s_keptBytes, LinePrefix, prefix);
    memcpy(s_kept + s_keptBytes + prefix, text, size);
    s_keptBytes += prefix + size;
    s_kept[s_keptBytes++] = '\n';
    ++s_keptLines;
}

// Writes the kept lines in one block.
void flush()
{
    if (!s_keptLines && !s_droppedLines)
        return;
    pf::logInfo("hitch: %u lines kept since the last write, %u dropped (no space):", unsigned(s_keptLines),
                unsigned(s_droppedLines));
    pf::logBlock(s_kept, s_keptBytes);
    s_keptBytes = 0;
    s_keptLines = 0;
    s_droppedLines = 0;
}

void report(const perf::SlowPresent& present, const Record* r)
{
    float excess[CauseCount] = {};
    if (r)
        memcpy(excess, r->excess, sizeof(r->excess));
    float value[CauseCount], average[CauseCount];
    renderCauses(present.renderUs, present.queueUs, present.parts, value);
    renderCauses(present.renderAvgUs, present.queueAvgUs, present.partAvgs, average);
    for (int c = FirstRender; c < CauseCount; ++c)
        excess[c] = value[c] - average[c];
    float largest = 0.0f;
    int cause = dominant(excess, &largest);
    if (!r && cause == CauseUnclear)
        cause = CauseNoRecord;
    ++s_counts[cause];
    ++s_slow;

    Line line;
    line.append("hitch: ");
    if (r)
    {
        appendWhen(line, r->racing, r->raceUs, r->flow, r->coordinator, r->stage);
    }
    else
    {
        x86::reg32 coordinator, stage;
        bench::raceState(&coordinator, &stage);
        appendWhen(line, s_racing, s_racing ? present.presentUs - s_raceStartUs : 0, guestDword(GameFlowState),
                   coordinator, stage);
    }
    appendRender(line, present);
    if (r)
        appendRecord(line, *r);
    else
        line.append(" | no game frame record%s", present.frame ? "" : " (a layer present)");
    if (cause < CauseCount)
        line.append(" | dominant %s +%.1f ms", CauseNames[cause], double(largest) / 1000.0);
    else
        line.append(" | dominant %s", CauseNames[cause]);
    keep(line.text, line.length);
}

// Keeps the lines of the slow presents whose records are finished.
void reportPresents()
{
    while (const perf::SlowPresent* present = perf::peekSlowPresent())
    {
        if (present->frame && int32_t(present->frame - s_lastFrame) > 0)
            break;
        report(*present, present->frame ? findRecord(present->frame) : nullptr);
        perf::popSlowPresent();
    }
}

}

void frame(const uint32_t* phaseUs)
{
    const uint32_t now = perf::now();
    perf::Totals totals;
    perf::totals(totals);
    const bool racing = bench::racing();
    const bool wasRacing = s_racing;
    if (racing && !wasRacing)
        s_raceStartUs = now;
    s_racing = racing;
    if (!s_started)
    {
        s_started = true;
        s_lastEndUs = now;
        s_lastTotals = totals;
        s_lastFrame = pf::gxm::Device::get().lastBegunFrame();
        pf::logInfo("hitch: hitch log on (hitch_log): one line per presented frame over %.1f ms, kept in memory and"
                    " written in the front end, with the summary, or outside a race when the space is 3/4 full",
                    ms(perf::HitchUs));
        return;
    }

    Record& c = s_current;
    c.wallUs += now - s_lastEndUs;
    ++c.renders;
    c.logUs += s_pendingLogUs;
    s_pendingLogUs = 0;
    for (int i = 0; i < phases::Count; ++i)
        c.phase[i] += phaseUs[i];
    addDelta(c.delta, totals, s_lastTotals);
    s_lastEndUs = now;
    s_lastTotals = totals;
    const uint32_t begun = pf::gxm::Device::get().lastBegunFrame();
    if (begun != s_lastFrame)
    {
        c.firstFrame = s_lastFrame + 1;
        c.frame = begun;
        c.racing = racing;
        c.raceUs = racing ? now - s_raceStartUs : 0;
        c.flow = guestDword(GameFlowState);
        bench::raceState(&c.coordinator, &c.stage);
        float v[FirstRender];
        gameCauses(c, v);
        for (int i = 0; i < FirstRender; ++i)
        {
            c.excess[i] = v[i] - s_average[i];
            s_average[i] += (v[i] - s_average[i]) * AverageWeight;
        }
        s_records[s_recordCount++ % RecordSlots] = c;
        s_lastFrame = begun;
        c = Record();
    }

    // The kept lines are written in the front end, when the space is three quarters full outside a race, and with the
    // summary; never while racing.
    const bool raceEnded = wasRacing && !racing && !bench::enabled();
    const bool raceStarted = racing && !wasRacing && !bench::enabled();
    const bool writeKept = s_keptLines && !racing
                        && (guestDword(GameFlowState) == FlowFrontEnd || s_keptBytes >= KeptBytes / 4 * 3);
    if (!perf::peekSlowPresent() && !raceEnded && !raceStarted && !writeKept)
        return;
    const uint32_t start = perf::now();
    reportPresents();
    if (raceEnded && s_slow)
        logSummary("race");
    if (raceStarted)
        resetSummary();
    if (writeKept)
        flush();
    s_pendingLogUs += perf::now() - start;
}

void resetSummary()
{
    if (!perf::hitch())
        return;
    memset(s_counts, 0, sizeof(s_counts));
    s_slow = 0;
    s_droppedAtReset = perf::droppedSlowPresents();
}

void logSummary(const char* label)
{
    if (!perf::hitch())
        return;
    flush();
    int order[CauseSlots];
    int shown = 0;
    for (int c = 0; c < CauseSlots; ++c)
    {
        if (s_counts[c])
            order[shown++] = c;
    }
    std::sort(order, order + shown, [](int a, int b) { return s_counts[a] > s_counts[b]; });
    Line line;
    line.append("hitch: summary (%s): %u frames over %.1f ms by dominant cause:", label, unsigned(s_slow),
                ms(perf::HitchUs));
    for (int i = 0; i < shown; ++i)
        line.append("%s %s %u", i ? "," : "", CauseNames[order[i]], unsigned(s_counts[order[i]]));
    const uint32_t dropped = perf::droppedSlowPresents() - s_droppedAtReset;
    if (dropped)
        line.append("; %u slow presents dropped (not logged)", unsigned(dropped));
    pf::logInfo("%s", line.text);
    resetSummary();
}

}}
