#define TENSHIZ_RELAX_TESTING
#include <core/relax/relax.hxx>
#include <impl/config/config_store.hxx>
#include <sstream>
#include <cassert>
#include <cstdio>
int main() {
    relax::c_relax r;
    r.seed_for_tests(1);
    osu::game_snapshot_t g{}; g.cur_state=osu::game_state_t::play; g.left_key='Z'; g.right_key='X';
    osu::beatmap_data_t m{}; m.loaded=true; m.cs=4; m.od=8;
    osu::hit_object_t o{}; o.type=osu::slider; o.start_time=1000; o.end_time=4000;
    m.objects.push_back(o);
    int down=0,up=0;
    r.set_input_sink_for_tests([&](WORD,bool pressed){ if(pressed)++down;else ++up;});
    r.update(g,m);
    relax::c_relax::scheduled_object_t s{}; assert(r.scheduled_object(0,s));
    r.flush_normal_at_for_tests(g,s.press_time); assert(down==1);
    r.enabled=false; r.update(g,m);
    assert(up==1 && r.queue_size()==0);
    r.enabled=true; r.update(g,m); assert(r.scheduled_object(0,s));
    r.flush_normal_at_for_tests(g,s.press_time); assert(down==2);
    r.enabled=false; r.update(g,m); assert(up==2);
    config::settings_t settings{};
    std::istringstream saved("keys.relax_toggle=120\n");
    config::read_profile(saved,settings); assert(settings.relax_toggle_key==VK_F9);
    std::istringstream legacy("");
    config::read_profile(legacy,settings); assert(settings.relax_toggle_key==VK_F8);
    puts("Relax toggle: disable releases hold, re-enable schedules, custom binding and legacy default passed.");
}
