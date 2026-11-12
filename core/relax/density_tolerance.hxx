#pragma once
#include <impl/struct/game_snapshot.hxx>
#include <algorithm>
#include <cstdint>

namespace relax {
    inline int visible_note_heads(const osu::live_playfield_t& frame) {
        return static_cast<int>(std::count_if(frame.objects.begin(),frame.objects.end(),[&](const auto& o){
            return !o.judged && frame.game_time<=o.start_time+o.late_window;
        }));
    }
    inline float density_100_chance(int visible,int tolerance) {
        const float density=std::clamp(static_cast<float>(visible-4)/8.f,0.f,1.f);
        return .30f*density*(1.f-static_cast<float>(std::clamp(tolerance,0,100))*.01f);
    }
    inline float density_roll(int index,int time,uint32_t seed) {
        uint32_t x=static_cast<uint32_t>(index)*0x9e3779b9u ^ static_cast<uint32_t>(time) ^ seed ^ 0xa511e9b3u;
        x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;
        return static_cast<float>((static_cast<double>(x)+.5)/4294967296.0);
    }
}
