#define TENSHIZ_AIM_TESTING
#include <core/aim_assist/aimbot.hxx>
#include <cstdio>

int main() {
    int checks=0, failures=0, outputs=0;
    auto check=[&](bool ok,const char* name){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",name);}};
    aim_assist::c_aimbot aim;
    const POINT raw{500,500};
    aim.set_output_sink_for_tests([&](int,int){++outputs;return false;});
    check(!aim.apply_for_tests(raw),"unmodified physical input passes through");
    check(outputs==0,"no replacement is sent for unmodified input");
    aim.prime_offset_for_tests(12.f);
    check(!aim.apply_for_tests(raw),"failed replacement never swallows physical movement");
    check(outputs==1,"failed output was attempted once");
    check(!aim.apply_for_tests(raw),"failed output clears correction rather than repeatedly blocking");
    check(outputs==1,"no repeated failed output after correction is cleared");
    aim.set_output_sink_for_tests([&](int,int){
        ++outputs;
        check(!aim.apply_for_tests(raw),"reentrant movement passes through without waiting on the hook lock");
        return true;
    });
    aim.prime_offset_for_tests(12.f);
    check(aim.apply_for_tests(raw),"acknowledged correction replaces physical movement");
    check(outputs==2,"reentrant notification creates no extra synthetic movement");
    const HWND window=CreateWindowExW(0,L"STATIC",L"Aim regression",WS_POPUP,0,0,1920,1080,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    check(window!=nullptr,"test window created without displaying or moving the cursor");
    input::set_target_window(window);
    osu::game_snapshot_t game{};
    game.cur_state=osu::game_state_t::play;game.drawable_ruleset=0x10000;game.speed_mult=1;
    osu::beatmap_data_t map{};map.loaded=true;map.cs=4;
    osu::hit_object_t slider{};slider.type=osu::slider;slider.source_object=0x20000;slider.start_time=1000;slider.end_time=10000;
    map.objects.push_back(slider);
    auto live=std::make_shared<osu::live_playfield_t>();live->drawable_ruleset=game.drawable_ruleset;
    osu::live_hit_object_t head{};head.source_object=slider.source_object;head.start_time=1000;head.slider=true;
    head.client_x=500;head.client_y=500;head.radius=60;head.late_window=100;
    live->objects.push_back(head);game.live_playfield=live;
    aim_assist::c_aimbot approach;
    auto update=[&](int time){game.cur_time=time;live->game_time=time;live->sampled_wall_ms=GetTickCount64();approach.update(game,map);return approach.snapshot_for_tests();};
    check(update(990)->targets.size()==1,"native slider head remains assisted during approach");
    live->objects[0].judged=true;live->objects[0].hit=true;
    check(update(991)->targets.empty(),"hit head cannot become a persistent slider-ball target");
    live->objects[0].judged=false;
    check(update(1001)->targets.empty(),"overdue head cannot keep pulling against departure");
    check(update(8000)->targets.empty(),"long slider cannot re-acquire Aim later in the map");
    input::set_target_window(nullptr);DestroyWindow(window);
    std::printf("Aim input passthrough: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
