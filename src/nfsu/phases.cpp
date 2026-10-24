// Phase log (phases.h): pass-through overrides of the timed guest functions and the per-frame sums.

#include "phases.h"

#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>

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
void sub_403a60_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_48e8b0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_51d240_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_445050_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4483c0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_409580_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_40ed50_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_571050_guest(win32::WinApplication* app, x86::CPU& cpu);

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
    uint32_t calls = 0;
    uint32_t depth = 0;
    uint32_t start = 0;
    uint32_t blockedAtStart = 0;
};

Phase s_phases[Count] = {
    {"step"}, {"move"}, {"physics"}, {"mellow"}, {"collide"}, {"predict"}, {"ai_car"}, {"ai_driver"},
    {"particles"}, {"cameras"}, {"sound"}, {"streamer"}, {"loading"}, {"render"}, {"views"}, {"cull"}, {"stuff"},
    {"flush"}, {"flares"}, {"cars"}, {"particle_draw"}, {"fe"},
};

bool s_started = false;
uint32_t s_frames = 0;
uint32_t s_intervalStart = 0;
uint32_t s_blockedAtInterval = 0;

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
    p.us += wall > blocked ? wall - blocked : 0;
    ++p.calls;
}

void configure()
{
    g_on = pf::Config::getBool("perf_phases", false);
    if (!g_on)
        return;
    pf::perf::startCounting();
    pf::logInfo("phases: phase log on (perf_phases), every %u frames; %s", unsigned(ReportFrames),
                pf::perf::enabled() ? "lock releases taken out of the phase times"
                                    : "phase times include lock releases (perf_log=1 takes them out)");
}

void frame()
{
    if (!g_on)
        return;
    const uint32_t now = pf::perf::now();
    if (!s_started)
    {
        // The interval starts at the first frame end; earlier phase sums are dropped.
        s_started = true;
        s_intervalStart = now;
        s_blockedAtInterval = pf::perf::gameBlockedTotal();
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
    line.append("phases: frame %.2f ms, released %.2f | ms (calls) per frame:", double(now - s_intervalStart) / 1000.0 /
                frames, double(blocked) / 1000.0 / frames);
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
    phases::Scope scope(phases::Step);
    sub_420b40_guest(app, cpu);
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

void sub_403a60(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::ParticleUpdate);
    sub_403a60_guest(app, cpu);
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
    sub_409580_guest(app, cpu);
}

void sub_40ed50(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::Flush);
    sub_40ed50_guest(app, cpu);
}

void sub_571050(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::Cars);
    sub_571050_guest(app, cpu);
}

}
}
