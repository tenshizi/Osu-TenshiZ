#define NOMINMAX
#include <core/relax/density_tolerance.hxx>
#include <impl/config/config_store.hxx>
#include <cstdio>
#include <sstream>
#include <cmath>
int main() {
    int checks=0,failures=0;
    auto check=[&](bool ok,const char* name){++checks;if(!ok){++failures;std::printf("FAIL: %s\n",name);}};
    using namespace relax;
    float previous=0;
    for(int n=0;n<=4;++n) check(density_100_chance(n,0)==0,"uncrowded notes preserve normal timing");
    for(int n=5;n<=16;++n) {
        const float chance=density_100_chance(n,50);
        check(chance>=previous,"more visible notes increases probability");previous=chance;
        check(chance<=density_100_chance(n,0),"lower tolerance increases errors");
        check(density_100_chance(n,100)==0,"100 preserves base timing at all densities");
    }
    check(std::abs(density_100_chance(8,50)-.075f)<.0001f,"eight heads at 50 yields 7.5 percent eligible chance");
    for(int legacy=0;legacy<3;++legacy) {
        config::settings_t s;std::istringstream in("relax.burst_tolerance="+std::to_string(legacy));config::read_profile(in,s);
        check(s.relax_burst_tolerance==legacy*50,"old Low/Medium/High profiles migrate to percent");
    }
    config::settings_t settings;
    std::istringstream in("relax.burst_tolerance_percent=73\nrelax.burst_tolerance=0\nrelax.stream_tolerance_percent=42");
    config::read_profile(in,settings);
    check(settings.relax_density_tolerance==42,"old profile migrates to one density tolerance");
    std::istringstream current("relax.density_tolerance=67\nrelax.burst_tolerance_percent=0\nrelax.stream_tolerance_percent=0");
    config::read_profile(current,settings);
    check(settings.relax_density_tolerance==67,"explicit density overrides legacy settings");
    osu::live_playfield_t frame{};frame.game_time=1000;
    osu::live_hit_object_t head{};head.start_time=1100;head.late_window=100;
    frame.objects.push_back(head);head.slider=true;frame.objects.push_back(head);
    head.judged=true;frame.objects.push_back(head);head.judged=false;head.start_time=500;frame.objects.push_back(head);
    check(visible_note_heads(frame)==2,"count circles and slider heads excluding judged and expired heads");
    std::printf("Density tolerance: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
