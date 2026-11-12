#define TENSHIZ_RELAX_TESTING
#include <core/relax/relax.hxx>
#include <cassert>
#include <cstdio>
int run(bool manual, bool repeat, int gap) {
    relax::c_relax r;
    r.relax_mode=1; r.smart_early_correction=false;
    r.timing_variation=false; r.manual_offset_ms=0;
    r.seed_for_tests(7);
    osu::game_snapshot_t g{};
    g.cur_state=osu::game_state_t::play; g.left_key='Z'; g.right_key='X';
    osu::beatmap_data_t m{}; m.loaded=true; m.od=8; m.cs=4;
    for(int i=0;i<6;++i) {
        osu::hit_object_t o{}; o.start_time=o.end_time=1000+i*gap; o.type=osu::circle;
        m.objects.push_back(o);
    }
    int down=0;
    r.set_input_sink_for_tests([&](WORD,bool pressed){if(pressed)++down;});
    for(int t=0;t<1000+6*gap;++t) {
        g.cur_time=t;
        r.update(g,m);
        if(manual && t==1000) {
            r.handle_key_event('Z',true);
            if(!repeat) r.handle_key_event('Z',false);
        }
        if(manual && repeat && t>1000 && t%30==0)
            r.handle_key_event('Z',true);
    }
    return down;
}
int main() {
    assert(run(false,false,100)==0); // no manual input: never full auto
    assert(run(true,false,100)==1);  // one manual tap buys just one rescue
    assert(run(true,true,100)==1);   // held key/repeat cannot renew rescue
    assert(run(true,false,500)==0);  // manual activity expires after 350ms
    puts("Manual rescue: idle, single rescue, key-repeat and timeout checks passed.");
}
