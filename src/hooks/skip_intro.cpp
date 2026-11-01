// Skip intro (launcher checkbox, settings skip_intro, PGR4_SKIP_INTRO=1). The boot movies are played by
// Screens\Intro\IntroMovies.lua, which waits for each slot's MOVIE_FINISHED event. The engine's slot update
// (sub_828893C0) raises it when the Bink player says the movie is done (sub_8283B720: no movie, or its
// frame number has reached its frame count). When a boot movie is opened (kernel/imports_io.cpp), the next
// such check says done, so the script moves straight on to the title screen. Boot movies play one at a
// time, so the next check is that movie's.
#include <atomic>
#include <string>

#include "kernel/imports.h"
#include "log.h"

namespace
{
std::atomic<int> g_armed{ 0 };
}

void SkipIntroMovieOpened(const std::string& name)
{
    g_armed = 1;
    Log("intro: skipping %s", name.c_str());
}

PPC_FUNC_IMPL(__imp__sub_8283B720);

// sub_8283B720(player): whether the movie has finished.
PPC_FUNC(sub_8283B720)
{
    if (g_armed.exchange(0))
    {
        ctx.r3.u64 = 1;
        return;
    }
    __imp__sub_8283B720(ctx, base);
}
