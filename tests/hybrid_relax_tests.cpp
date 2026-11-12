#define TENSHIZ_RELAX_TESTING
#include <core/relax/relax.hxx>
#include <cassert>
#include <impl/config/config_store.hxx>
#include <sstream>
#include <cstdio>
int main() {
    relax::c_relax r;
    r.relax_mode=2; r.timing_variation=false;
    r.seed_for_tests(5);
    osu::game_snapshot_t g{};
    g.cur_state=osu::game_state_t::play; g.left_key='Z'; g.right_key='X';
    osu::beatmap_data_t m{}; m.loaded=true; m.od=8; m.cs=4;
    for(int i=0;i<5;++i) {
        osu::hit_object_t o{}; o.start_time=o.end_time=1000+i*400; o.type=osu::circle;
        m.objects.push_back(o);
    }
    int presses=0;
    r.set_input_sink_for_tests([&](WORD,bool down){if(down)++presses;});
    r.update(g,m);
    double at[5]{};
    for(int i=0;i<5;++i) {
        relax::c_relax::scheduled_object_t o{};
        assert(r.scheduled_object(i,o)); at[i]=o.press_time;
    }
    r.flush_normal_at_for_tests(g,at[0]); assert(presses==1);
    assert(!r.handle_key_event('Z',true)); // physical input passes through
    r.flush_normal_at_for_tests(g,at[1]); assert(presses==1);
    Sleep(280);
    r.flush_normal_at_for_tests(g,at[2]); assert(presses==1); // held key overrides timeout
    assert(!r.handle_key_event('Z',false));
    r.flush_normal_at_for_tests(g,at[3]); assert(presses==2); // release permits immediate takeover
    Sleep(280);
    r.flush_normal_at_for_tests(g,at[3]+200); // release the previous automatic hold
    r.flush_normal_at_for_tests(g,at[4]); assert(presses==3); // no backlog, just upcoming note
    r.on_leave_play(g);
    r.update(g,m);
    r.flush_normal_at_for_tests(g,at[0]); assert(presses==4); // session reset clears manual state
    r.on_leave_play(g);
    r.hybrid_delay_ms=100;
    r.update(g,m);
    r.handle_key_event('Z',true); r.handle_key_event('Z',false);
    r.flush_normal_at_for_tests(g,at[0]); assert(presses==4);
    Sleep(130);
    r.flush_normal_at_for_tests(g,at[1]); assert(presses==5);
    config::settings_t settings;
    std::istringstream saved("relax.mode=2\nrelax.hybrid_delay_ms=375\n");
    config::read_profile(saved,settings);
    assert(settings.relax_mode==2 && settings.relax_hybrid_delay_ms==375);
    std::istringstream excessive("relax.hybrid_delay_ms=9000\n");
    config::read_profile(excessive,settings);
    assert(settings.relax_hybrid_delay_ms==1000);
    std::istringstream legacy("relax.mode=2\n");
    config::read_profile(legacy,settings);
    assert(settings.relax_hybrid_delay_ms==0);
    puts("Hybrid: automatic start, physical passthrough, held key, instant release takeover, no backlog and reset checks passed.");
}


