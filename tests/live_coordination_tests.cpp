#define TENSHIZ_RELAX_TESTING
#include <core/relax/relax.hxx>
#include <core/aim_assist/a_core.hxx>
#include <cstdio>

static int checks = 0, failures = 0;
static void check(bool ok,const char* msg) { ++checks; if(!ok){++failures; std::printf("FAIL: %s\n",msg);} }
struct fixture {
    relax::c_relax r;
    osu::game_snapshot_t g;
    osu::beatmap_data_t m;
    std::shared_ptr<osu::live_playfield_t> frame;
    bool held[2]{};
    int presses = 0;
    std::vector<int> press_times;
    fixture(std::initializer_list<std::pair<int,int>> objects) : fixture(std::vector<std::pair<int,int>>(objects)) {}
    fixture(const std::vector<std::pair<int,int>>& objects) {
        g.client=osu::client_kind_t::lazer;g.cur_state=osu::game_state_t::play;
        g.left_key='Z';g.right_key='X';g.drawable_ruleset=0x10000;
        m.loaded=true;m.od=10;m.cs=4;
        int index=0;
        for(auto [start,end]:objects) {
            osu::hit_object_t o{};o.source_object=0x20000+(++index)*0x100;
            o.start_time=start;o.end_time=end;o.type=end>start?osu::slider:osu::circle;
            m.objects.push_back(o);
        }
        r.timing_variation=false;
        r.seed_for_tests(17);
        r.set_input_sink_for_tests([&](WORD key,bool down){held[key=='X']=down;if(down){++presses;press_times.push_back(g.cur_time);}});
        tick(0);
    }
    void tick(int time,bool hover=false,int judged=-1,bool paused=false) {
        g.cur_time=time;
        auto next=std::make_shared<osu::live_playfield_t>();
        if(frame) *next=*frame;
        next->drawable_ruleset=g.drawable_ruleset;next->game_time=time;
        next->sampled_wall_ms=GetTickCount64();next->paused=paused;
        if(next->objects.empty()) for(const auto& o:m.objects) {
            osu::live_hit_object_t live{};live.source_object=o.source_object;live.start_time=o.start_time;
            live.slider=o.type==osu::slider;live.great_window=19.5;live.late_window=99.5;
            live.ok_window=59.5;
            live.radius=60;next->objects.push_back(live);
        }
        for(auto& o:next->objects) o.hovered=hover;
        if(judged>=0) {auto& o=next->objects[judged];o.judged=true;o.hit=true;o.result=5;o.result_offset_valid=true;o.result_offset=-11;}
        frame=next;g.live_playfield=frame;r.update(g,m);
    }
    bool down() const{return held[0]||held[1];}
};
int main() {
    for(bool variation:{true,false}) for(bool early_hover:{true,false}) {
        std::vector<std::pair<int,int>> objects;
        for(int i=0;i<30;++i) objects.push_back({1000+i*35,1000+i*35});
        fixture f(objects);
        f.r.timing_variation=variation;f.r.tap_style=0;f.r.ur=0;f.r.timing_drift_ms=0;
        f.r.early_variation_ms=f.r.late_variation_ms=0;f.r.density_tolerance=0;
        int early_count=0;
        for(int t=940;t<=2060;++t) {
            const int before=f.presses;
            const int normal_offset=variation?0:-12;
            f.tick(t,early_hover||(t>=1000+normal_offset&&(t-1000-normal_offset)%35==0));
            if(f.presses>before) {
                const int offset=t-f.m.objects[before].start_time;
                check(offset==normal_offset||(offset>=-59&&offset<=-20),"density uses actual 100 window, never a 50 or forced late hit");
                early_count+=offset<=-20;
                auto& result=f.frame->objects[before];result.judged=true;result.hit=true;result.result=offset<=-20?3:5;
            }
        }
        check(f.presses==30,"density preserves every note and never duplicates an input");
        check(early_hover?early_count>0:early_count==0,"unavailable early 100 opportunity falls back to normal timing without waiting");
    }
    {
        std::mt19937 rng(17);const auto seed=rng();int start=1000;
        while(relax::density_roll(0,start,seed)>=.3f) start+=100;
        std::vector<std::pair<int,int>> objects{{start,start+8000}};
        for(int i=1;i<=14;++i) objects.push_back({start+i*300,start+i*300});
        fixture f(objects);f.r.density_tolerance=0;f.r.tap_style=0;
        for(int t=start-40;t<start+50;++t) f.tick(t,true);
        check(f.press_times.size()==1&&f.press_times[0]<start-20,"density applies to slider heads independently of Timing Variation");
        f.tick(start+7000,false,0);check(f.down(),"density slider head preserves long hold");
        f.tick(start+8200,false);check(!f.down(),"density slider hold releases after tail");
    }
    for (bool late_only : {false,true}) {
        fixture f{{1000,1000},{1200,1200},{1400,1400},{1600,1600},{1800,1800},{2000,2000}};
        f.r.timing_variation=true;f.r.ur=200;f.r.timing_drift_ms=0;
        f.r.early_variation_ms=late_only?0:-25;f.r.late_variation_ms=late_only?25:0;
        for(int t=960;t<=2040;++t) f.tick(t,true);
        check(f.presses==6,"live timing variation preserves exactly one press per head");
        bool varied=false;
        for(size_t i=0;i<f.press_times.size();++i) {
            const int offset=f.press_times[i]-f.m.objects[i].start_time;
            check(late_only ? offset>=0&&offset<=15 : offset<=0&&offset>=-15,"native Early/Late bounds and actual Great window apply");
            varied|=offset!=0;
        }
        check(varied,"native timing is no longer fixed at the same early offset");
    }
    {
        fixture f{{1000,1000},{1200,1200}};
        f.r.timing_variation=true;f.r.ur=0;f.r.timing_drift_ms=0;
        f.r.early_variation_ms=0;f.r.late_variation_ms=0;
        f.tick(988,true);check(f.presses==0,"zero variation targets object time rather than fixed native early time");
        f.tick(1000,true);check(f.presses==1,"zero variation hits at object time");
        f.r.manual_offset_ms=10;
        f.tick(1200,true);check(f.presses==1,"editing timing after scheduling affects the pending note");
        f.tick(1210,true);check(f.presses==2,"pending note uses updated Base Offset without a map restart");
    }
    {
        fixture f{{1000,1000},{1075,1075}};
        f.tick(988,true);
        f.g.cur_time=989;f.g.live_playfield.reset();f.r.update(f.g,f.m);
        check(f.down(),"one unreadable frame must not interrupt an ordinary tap");
        f.tick(990,true,0);
        check(f.presses==1,"recovering a live read must not produce a second physical press");
        f.tick(991,true,-1,true);
        f.tick(992,true);
        check(f.presses==1,"resuming cannot re-arm an already-hit ordinary tap");
        f.tick(1063,true);
        check(f.presses==2,"next burst note still dispatches at its own native threshold");
    }
    {
        fixture f{{1000,1000},{1100,1100}};
        f.tick(987,true);check(f.presses==0,"native allowance rejects too-early hover");
        f.tick(988,false);check(f.presses==0,"no timed click before native hover");
        f.tick(995,true);check(f.presses==1,"arriving at hit area creates one press");
        f.tick(996,true,0);check(f.presses==1,"game-judged head is not pressed again");
        check(f.r.live_telemetry().great==1,"game's actual Great result is observed");
        f.tick(1088,false);check(f.presses==1,"next object independently waits for hover");
        f.tick(1092,true);check(f.presses==2,"next object hits at its own live readiness time");
    }
    {
        fixture f{{1000,1000},{1075,1075}};
        f.tick(988,false);
        f.frame->objects[0].judged=true;f.frame->objects[0].hit=false;f.frame->objects[0].result=1;
        f.tick(1063,true);check(f.presses==1,"a judged miss does not shift the following burst note");
    }
    for(int duration:{20,80,500,8000}) {
        fixture f{{1000,1000+duration},{1100,1100}};
        f.tick(988,true);check(f.down(),"slider head acquires input");
        f.tick(1001,true,0);
        for(int t=1002;t<=1000+duration;t+=7) {
            f.tick(t,false);check(f.down(),"native hover loss never drops duration hold");
        }
        f.tick(1000+duration+150,false);check(!f.down(),"slider eventually releases after its tail");
    }
    {
        fixture f{{1000,9000},{1300,1300}};
        f.tick(988,true);f.tick(1001,true,0);f.tick(1288,true);
        check(f.presses==2&&f.down(),"overlap produces a fresh edge without ending slider hold");
        f.tick(1350,false,1);check(f.down(),"circle release cannot cancel active slider hold");
        f.g.cur_time=1360;f.g.live_playfield.reset();f.r.update(f.g,f.m);
        check(f.down(),"brief unreadable frame preserves a protected slider");
        f.tick(1370,false);f.tick(1400,false,-1,true);check(!f.down(),"actual native pause releases input");
        f.tick(1401,false);check(f.down(),"resume re-establishes protected duration hold");
        f.tick(9200,false);check(!f.down(),"resume does not extend slider beyond tail");
    }
    {
        assist::waypoint_t head{}, follow{};
        head.start_time=1000;head.hit_deadline=1060;head.native_geometry=true;
        follow.start_time=500;follow.hit_deadline=2000;follow.slider_follow=true;follow.is_slider=true;
        assist::config cfg{};assist::trace state{};state.bind_id=1000;
        assist::waypoint_t targets[]{follow,head};
        check(assist::select_target(targets,2,1005,cfg,state)==&targets[1],"Aim retains a pending late head");
        state.bind_id=500;
        check(assist::select_target(targets,2,990,cfg,state)==&targets[1],"imminent burst head takes priority over slider follow");
        check(assist::select_target(targets,2,1100,cfg,state)==&targets[0],"Aim resumes slider follow after head deadline");
        check(assist::select_target(targets,2,2100,cfg,state)==nullptr,"expired native targets cannot lock Aim");
    }
    {
        fixture f{{1000,1000}};
        f.frame->objects[0].source_object += 0x100000;
        f.tick(988,true);
        check(f.presses==1,"managed pointer relocation retains unique beatmap identity");
    }
    {
        fixture f{{1000,1000}};
        f.tick(988,true);
        f.r.on_leave_play(f.g);
        check(!f.down(),"leaving native play releases owned input");
        f.tick(988,true);
        check(f.presses==2,"new session may dispatch at the same map timestamp");
    }
    std::printf("Live Relax/Aim coordination: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
