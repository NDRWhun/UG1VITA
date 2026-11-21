// Phase log (phases.h): pass-through overrides of the timed guest functions and the per-frame sums.

#include "phases.h"

#include "cars.h"

#include "hitch.h"
#include "lights.h"
#include "retstate.h"
#include "sceneshadow.h"
#include "scenesnap.h"
#include "views.h"
#include "worldmodels.h"

#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <recomp.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace nfsu
{

// Generated guest bodies of the timed functions.
void sub_420b40_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_420310_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4774a0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_474a90_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_420380_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_420890_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_44b1b0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_462430_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_48e8b0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_51d240_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_445050_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4483c0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_409580_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_40ed50_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_571050_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_570db0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_570d00_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_56dc30_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_56cf10_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_56d0c0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_56d700_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_569460_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5437f0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_544390_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5791c0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_469580_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_468060_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_46b5e0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_46bae0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_577010_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_57a530_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_42a790_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_426450_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_429620_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4267a0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_426930_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_595670_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4281e0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4286a0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_589f00_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_58e040_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_430730_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5793e0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_435570_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace phases
{

bool g_on = false;

namespace
{

const uint32_t ReportFrames = 300;

struct Phase
{
    const char* name;
    uint64_t us = 0;        // inclusive, lock releases taken out
    uint32_t frameUs = 0;   // the same in the current frame (hitch log, collision frames)
    uint32_t calls = 0;
    uint32_t frameCalls = 0;
    uint32_t depth = 0;
    uint32_t start = 0;
    uint32_t blockedAtStart = 0;
};

Phase s_phases[Count] = {
    {"step"}, {"move"}, {"physics"}, {"mellow"}, {"collide"}, {"predict"}, {"witnesses"}, {"contact_test"},
    {"contact_resolve"}, {"gjk"}, {"simplex"}, {"impulse"}, {"ai_car"}, {"ai_driver"}, {"particles"}, {"topology"},
    {"avoidables"}, {"ride_info"}, {"draft"}, {"car_forces"}, {"begin_move"}, {"finish_move"}, {"smokeables"}, {"car_events"}, {"terrain_effects"},
    {"skids"}, {"event_manager"}, {"player_state"}, {"space_nodes"}, {"race_update"}, {"cameras"}, {"sound"},
    {"streamer"}, {"loading"}, {"render"}, {"views"}, {"cull"}, {"stuff"}, {"flush"}, {"flares"}, {"cars"},
    {"particle_draw"}, {"fe"}, {"clans"}, {"world_models"}, {"car_reflections"}, {"mini_world"}, {"car_flares"},
    {"car_render"}, {"car_shadow"}, {"car_part_cull"}, {"light_context"}, {"light_clone"}, {"world_model"},
    {"space_node"}, {"submit"},
};

bool s_report = false;      // timing_phases
bool s_started = false;
uint32_t s_frames = 0;
uint32_t s_intervalStart = 0;
uint32_t s_blockedAtInterval = 0;
// With the hitch log on: the game thread's lock releases by reason at the interval start.
pf::perf::Totals s_totalsAtInterval = {};

struct Line
{
    char text[1024] = {};
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

void appendPhases(Line& line, int first, int last, double frames)
{
    for (int i = first; i < last; ++i)
    {
        const Phase& p = s_phases[i];
        line.append(" %s %.2f (%.1f)", p.name, double(p.us) / 1000.0 / frames, double(p.calls) / frames);
    }
}

// The phases of the collision frame summary.
const Id CollisionShown[] = {Step, PhysicsMove, Collide, Witnesses, ContactTest, ContactResolve, Gjk, SimplexDistance,
                             Impulse};
const int CollisionShownCount = sizeof(CollisionShown) / sizeof(CollisionShown[0]);

struct CollisionFrames
{
    uint32_t frames = 0;
    uint32_t allFrames = 0;
    uint64_t us[CollisionShownCount] = {};
    uint32_t maxUs[CollisionShownCount] = {};
    uint64_t calls[CollisionShownCount] = {};
};

CollisionFrames s_collision;

void collisionFrame()
{
    CollisionFrames& c = s_collision;
    ++c.allFrames;
    if (s_phases[ContactResolve].frameCalls == 0)
        return;
    ++c.frames;
    for (int i = 0; i < CollisionShownCount; ++i)
    {
        const Phase& p = s_phases[CollisionShown[i]];
        c.us[i] += p.frameUs;
        c.maxUs[i] = std::max(c.maxUs[i], p.frameUs);
        c.calls[i] += p.frameCalls;
    }
}

}

void resetCollisionFrames()
{
    s_collision = CollisionFrames();
}

void logCollisionFrames()
{
    if (!s_report)
        return;
    const CollisionFrames& c = s_collision;
    Line line;
    line.append("phases: collision frames (a contact resolved): %u of %u; ms avg / max (calls per frame):",
                unsigned(c.frames), unsigned(c.allFrames));
    const double frames = c.frames ? double(c.frames) : 1.0;
    for (int i = 0; i < CollisionShownCount; ++i)
        line.append(" %s %.2f / %.2f (%.1f)", s_phases[CollisionShown[i]].name, double(c.us[i]) / 1000.0 / frames,
                    double(c.maxUs[i]) / 1000.0, double(c.calls[i]) / frames);
    pf::logInfo("%s", line.text);
}

void enterSlow(Id id)
{
    Phase& p = s_phases[id];
    if (p.depth++ != 0)
        return;
    p.start = pf::perf::now();
    p.blockedAtStart = pf::perf::gameBlockedTotal();
}

void leaveSlow(Id id)
{
    Phase& p = s_phases[id];
    if (p.depth == 0 || --p.depth != 0)
        return;
    const uint32_t wall = pf::perf::now() - p.start;
    const uint32_t blocked = pf::perf::gameBlockedTotal() - p.blockedAtStart;
    const uint32_t us = wall > blocked ? wall - blocked : 0;
    p.us += us;
    p.frameUs += us;
    ++p.calls;
    ++p.frameCalls;
}

void configure()
{
    s_report = pf::Config::getBool("timing_phases", false);
    g_on = s_report || pf::perf::hitch();
    if (!s_report)
        return;
    pf::perf::startCounting();
    pf::logInfo("phases: phase log on (timing_phases), every %u frames; %s", unsigned(ReportFrames),
                pf::perf::timed() ? "lock releases taken out of the phase times"
                                  : "phase times include lock releases (timing_log=on or hitch_log=on takes them out)");
}

void frame()
{
    if (!g_on)
        return;
    if (s_report)
        collisionFrame();
    uint32_t us[Count];
    for (int i = 0; i < Count; ++i)
    {
        us[i] = s_phases[i].frameUs;
        s_phases[i].frameUs = 0;
        s_phases[i].frameCalls = 0;
    }
    if (pf::perf::hitch())
        hitch::frame(us);
    if (!s_report)
        return;
    const uint32_t now = pf::perf::now();
    if (!s_started)
    {
        // The interval starts at the first frame end; earlier phase sums are dropped.
        s_started = true;
        s_intervalStart = now;
        s_blockedAtInterval = pf::perf::gameBlockedTotal();
        pf::perf::totals(s_totalsAtInterval);
        for (Phase& p : s_phases)
            p.us = p.calls = 0;
        for (int i = 0; i < pf::perf::CountCount; ++i)
            pf::perf::takeCount(pf::perf::Count(i));
        return;
    }
    if (++s_frames < ReportFrames)
        return;
    const double frames = double(s_frames);
    const uint32_t blocked = pf::perf::gameBlockedTotal() - s_blockedAtInterval;
    Line line;
    line.append("phases: frame %.2f ms, released %.2f", double(now - s_intervalStart) / 1000.0 / frames,
                double(blocked) / 1000.0 / frames);
    if (pf::perf::hitch())
    {
        pf::perf::Totals totals;
        pf::perf::totals(totals);
        auto blockedMs = [&](pf::perf::Block block)
        {
            return double(totals.blocked[block] - s_totalsAtInterval.blocked[block]) / 1000.0 / frames;
        };
        line.append(" (object waits %.2f, critical sections %.2f, files %.2f, sleeps %.2f, preemptions %.2f)",
                    blockedMs(pf::perf::BlockWait), blockedMs(pf::perf::BlockCritSec), blockedMs(pf::perf::BlockFile),
                    blockedMs(pf::perf::BlockSleep), blockedMs(pf::perf::BlockPreempt));
        s_totalsAtInterval = totals;
    }
    line.append(" | ms (calls) per frame:");
    appendPhases(line, Step, Render, frames);
    pf::logInfo("%s", line.text);
    Line render;
    render.append("phases: render:");
    appendPhases(render, Render, Count, frames);
    render.append(" | per frame: clock reads %.0f, critical sections %.0f (%.1f blocking), lock releases %.1f,"
                  " preemptions %.1f", double(pf::perf::takeCount(pf::perf::CountClock)) / frames,
                  double(pf::perf::takeCount(pf::perf::CountCritSec)) / frames,
                  double(pf::perf::takeCount(pf::perf::CountCritSecWait)) / frames,
                  double(pf::perf::takeCount(pf::perf::CountRelease)) / frames,
                  double(pf::perf::takeCount(pf::perf::CountYield)) / frames);
    pf::logInfo("%s", render.text);
    for (Phase& p : s_phases)
        p.us = p.calls = 0;
    s_frames = 0;
    s_intervalStart = now;
    s_blockedAtInterval = pf::perf::gameBlockedTotal();
}

}

namespace native
{

void sub_420b40(win32::WinApplication* app, x86::CPU& cpu)
{
    {
        phases::Scope scope(phases::Step);
        sub_420b40_guest(app, cpu);
    }
    if (!cpu.terminate)
        scenesnap::afterStep(app, cpu);
}

void sub_420310(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::Move);
    sub_420310_guest(app, cpu);
}

void sub_4774a0(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::PhysicsMove);
    sub_4774a0_guest(app, cpu);
}

void sub_474a90(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::MellowMove);
    sub_474a90_guest(app, cpu);
}

void sub_420380(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::Collide);
    sub_420380_guest(app, cpu);
}

void sub_420890(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::Predict);
    sub_420890_guest(app, cpu);
}

void sub_44b1b0(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::AiCar);
    sub_44b1b0_guest(app, cpu);
}

void sub_462430(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::AiDriver);
    sub_462430_guest(app, cpu);
}

void sub_48e8b0(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::Cameras);
    sub_48e8b0_guest(app, cpu);
}

void sub_51d240(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::Sound);
    sub_51d240_guest(app, cpu);
}

void sub_445050(win32::WinApplication* app, x86::CPU& cpu)
{
    scenesnap::beforeStreamer(app, cpu);
    if (cpu.terminate)
        return;
    phases::Scope scope(phases::Streamer);
    sub_445050_guest(app, cpu);
}

void sub_4483c0(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::Loading);
    sub_4483c0_guest(app, cpu);
}

void sub_409580(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::Views);
    // Its first clear test (0x4098A2, PATCHES in generate.py) reads [0x6D3A00], zero padding past the envmap rows in
    // .rdata: the game's clear flag [0x73645C], or 0 while the magazine screen is shown ([0x735768]) so the background
    // its FE view cleared and drew stays (native.cpp sub_409cd0).
    rc::st32(0x6d3a00, rc::ld8(0x735768) ? 0 : rc::ld32(0x73645c));
    scenesnap::views(app, cpu, &views::run);
}

void sub_571050(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::Cars);
    cars::run(cars::RenderCarsPort, app, cpu);
}

void sub_58a240(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::Clans);
    worldmodels::clans(app, cpu);
}

void sub_592100(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::WorldModels);
    worldmodels::worldModels(app, cpu);
}

void sub_570db0(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::CarReflections);
    cars::run(cars::Reflections, app, cpu);
}

void sub_4ae650(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::MiniWorld);
    worldmodels::miniWorld(app, cpu);
}

void sub_570d00(win32::WinApplication* app, x86::CPU& cpu)
{
    // Recorded in the guest run of a shadow compare (sceneshadow.h): the flare screen pass 0x409b70 calls it.
    if (sceneshadow::record(0x570d00, 1, retstate::Eax, app, cpu, &sub_570d00))
        return;
    phases::Scope scope(phases::CarFlares);
    cars::run(cars::CarFlares, app, cpu);
}

void sub_56dc30(win32::WinApplication* app, x86::CPU& cpu)
{
    if (cars::tap(0x56dc30, app, cpu, &sub_56dc30))
        return;
    phases::Scope scope(phases::CarRender);
    cars::run(cars::RenderPort, app, cpu);
}

void sub_56cf10(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::CarShadow);
    cars::run(cars::ShadowHull, app, cpu);
}

void sub_56d0c0(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::CarShadow);
    cars::run(cars::ShadowCorners, app, cpu);
}

void sub_56d700(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::CarShadow);
    cars::run(cars::ShadowSetup, app, cpu);
}

void sub_569460(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::PartCull);
    cars::run(cars::PartCull, app, cpu);
}

void sub_5437f0(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::LightContext);
    lights::fillEntry(app, cpu);
}

void sub_544390(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::LightClone);
    lights::cloneEntry(app, cpu);
}

void sub_591e40(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::WorldModel);
    worldmodels::worldModel(app, cpu);
}

void sub_5791c0(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::SpaceNode);
    scenesnap::spaceNodeUpdate(cpu.ecx);
    sub_5791c0_guest(app, cpu);
}


void sub_577010(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::Topology);
    sub_577010_guest(app, cpu);
}

void sub_57a530(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::Avoidables);
    sub_57a530_guest(app, cpu);
}

void sub_42a790(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::RideInfo);
    sub_42a790_guest(app, cpu);
}

void sub_426450(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::Draft);
    sub_426450_guest(app, cpu);
}

void sub_429620(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::CarForces);
    sub_429620_guest(app, cpu);
}

void sub_4267a0(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::BeginMove);
    sub_4267a0_guest(app, cpu);
}

void sub_426930(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::FinishMove);
    sub_426930_guest(app, cpu);
}

void sub_595670(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::Smokeables);
    sub_595670_guest(app, cpu);
}

void sub_4281e0(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::CarEvents);
    sub_4281e0_guest(app, cpu);
}

void sub_4286a0(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::TerrainEffects);
    sub_4286a0_guest(app, cpu);
}

void sub_589f00(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::Skids);
    sub_589f00_guest(app, cpu);
}

void sub_58e040(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::EventManager);
    sub_58e040_guest(app, cpu);
}

void sub_430730(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::PlayerState);
    sub_430730_guest(app, cpu);
}

void sub_5793e0(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::SpaceNodes);
    sub_5793e0_guest(app, cpu);
}

void sub_435570(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::RaceUpdate);
    sub_435570_guest(app, cpu);
}

void sub_469580(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::Witnesses);
    sub_469580_guest(app, cpu);
}

void sub_468060(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::ContactTest);
    sub_468060_guest(app, cpu);
}

void sub_46b5e0(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::ContactResolve);
    sub_46b5e0_guest(app, cpu);
}

void sub_46bae0(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::Impulse);
    sub_46bae0_guest(app, cpu);
}
}
}
