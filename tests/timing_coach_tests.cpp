#define NOMINMAX
#include <core/tap_assist/timing_coach.hxx>
#include <cassert>
#include <cstdio>
int main() {
    timing::coach_t coach;
    osu::game_snapshot_t g{};
    g.cur_state = osu::game_state_t::play;
    g.drawable_ruleset = 123; g.map_id = 4; g.speed_mult = 2;
    osu::beatmap_data_t map{}; map.loaded = true;
    for (int i=0;i<60;++i) {
        osu::hit_object_t o{}; o.source_object=100+i; o.start_time=1000+i*100;
        map.objects.push_back(o);
    }
    auto frame=std::make_shared<osu::live_playfield_t>();
    frame->drawable_ruleset=g.drawable_ruleset;
    g.live_playfield=frame;
    for (int i=0;i<55;++i) {
        g.cur_time=frame->game_time=1000+i*100;
        osu::live_hit_object_t hit{};
        hit.source_object=100+i; hit.start_time=g.cur_time; hit.judged=hit.hit=true;
        hit.result_offset_valid=true; hit.result_offset=40;
        frame->objects={hit};
        coach.update(g,map,true); coach.update(g,map,true);
    }
    auto s=coach.summary();
    assert(s.hits==55 && s.recent==50 && s.mean==20 && s.last==20);
    // Relocation with a unique timestamp must not double-count the last hit.
    frame->objects[0].source_object=99999;
    coach.update(g,map,true); assert(coach.summary().hits==55);
    frame->objects[0].start_time=6500; frame->objects[0].source_object=155;
    coach.update(g,map,false); coach.update(g,map,true);
    assert(coach.summary().hits==55);
    frame->objects[0].start_time=6600; frame->objects[0].source_object=156;
    frame->objects[0].hit=false;
    coach.update(g,map,true); coach.update(g,map,true);
    assert(coach.summary().missed==1 && coach.summary().mean==20);
    // A retry clears the prior run.
    g.cur_time=frame->game_time=0; frame->objects.clear();
    coach.update(g,map,true);
    assert(coach.summary().hits==0 && coach.summary().missed==0);
    puts("Timing feedback: duplicate, relocation, rolling window, speed, mode exclusion, misses, retry checks passed.");
}
