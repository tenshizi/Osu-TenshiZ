#include <core/autobot/motion_core.hxx>

#include <cstdio>
#include <string>
#include <vector>

using namespace autobot;
static int checks = 0, failures = 0;
static void check( bool ok, const char* scenario, const char* message ) {
    ++checks;
    if ( !ok ) { ++failures; std::printf( "  FAIL %s: %s\n", scenario, message ); }
}
static osu::hit_object_t circle( int t, point_t p ) {
    osu::hit_object_t o{}; o.type = 1; o.start_time = o.end_time = t; o.x = p.x; o.y = p.y; return o;
}
static osu::hit_object_t slider( int t, int duration, point_t p, const char* path, float distance, int repeats = 1 ) {
    auto o = circle( t, p ); o.type = 2; o.end_time = t + duration;
    o.slider_curve_str = path; o.slider_length = distance; o.slider_repeat = repeats; return o;
}
static osu::beatmap_data_t map_of( std::vector<osu::hit_object_t> objects ) {
    osu::beatmap_data_t map{}; map.loaded = true; map.cs = 4.f; map.od = 8.f; map.objects = std::move( objects ); return map;
}
static schedule_view_t schedule_of( const osu::beatmap_data_t& map ) {
    schedule_view_t s;
    for ( const auto& o : map.objects ) {
        s.press_time.push_back( o.start_time ); s.release_time.push_back( o.end_time + ( is_slider( o ) ? 24 : 0 ) );
        s.edge_bias.push_back( 0.f );
    }
    return s;
}
struct slider_metrics_t {
    double ball = 0., hand = 0.;
    float min_margin = 1e6f, max_step = 0.f;
    float pressure = 0.f, economy = 0.f;
    uint64_t sliders = 0, completed = 0, failures = 0, discontinuities = 0, micro_plans = 0;
};
static slider_metrics_t measure_sliders( const char* name, const osu::beatmap_data_t& map,
                                         uint32_t seed = 0x8821u, float rate = 1.f, int step_ms = 2,
                                         bool report = true ) {
    auto schedule = schedule_of( map ); motion_engine_t engine; settings_t settings;
    settings.startup_motion = false; settings.break_motion = false; engine.configure( settings );
    engine.begin_session( object_center( map.objects.front(), false ), false, rate, map.objects.size(), seed, true );
    std::vector<slider_path_t> paths( map.objects.size() );
    std::vector<point_t> previous_ball( map.objects.size() ), previous_hand( map.objects.size() );
    std::vector<bool> seen( map.objects.size(), false );
    for ( size_t i = 0; i < paths.size(); ++i ) if ( is_slider( map.objects[i] ) ) paths[i] = build_slider_path( map.objects[i], false );
    slider_metrics_t out{}; point_t previous = engine.position(); size_t active = 0;
    for ( int t = map.objects.front().start_time - 350; t <= map.objects.back().end_time + 40; t += step_ms ) {
        engine.update( map, schedule, t, step_ms * 0.001f / rate );
        const point_t hand = engine.position();
        out.max_step = std::max( out.max_step, length( hand - previous ) ); previous = hand;
        if ( engine.plan_type() == plan_type_t::micro_gap ) ++out.micro_plans;
        while ( active+1 < map.objects.size() && map.objects[active+1].start_time <= t ) ++active;
        const auto& o = map.objects[active];
        if ( is_slider(o) && t >= o.start_time && t <= o.end_time ) {
            const point_t ball = eval_slider_ball( paths[active], o, t );
            out.min_margin = std::min( out.min_margin, hit_radius( map.cs, false ) * 1.85f - length( hand - ball ) );
            if ( seen[active] ) {
                out.ball += length( ball - previous_ball[active] );
                out.hand += length( hand - previous_hand[active] );
            }
            previous_ball[active] = ball; previous_hand[active] = hand; seen[active] = true;
        }
    }
    const auto& d = engine.diagnostics();
    out.sliders = d.sliders_started; out.completed = d.sliders_completed;
    out.failures = d.slider_head_failures + d.slider_follow_failures + d.slider_exit_failures + d.slider_early_releases;
    out.discontinuities = d.unexpected_discontinuities;
    out.pressure = d.local_motion_pressure; out.economy = d.slider_hand_economy;
    if ( report ) std::printf( "  %-23s ball=%7.2f hand=%7.2f ratio=%.3f margin=%6.2f plans=%llu complete=%llu/%llu disc=%llu\n",
        name, out.ball, out.hand, out.ball > 0. ? out.hand / out.ball : 0., out.min_margin,
        static_cast<unsigned long long>(d.plans_created), static_cast<unsigned long long>(out.completed),
        static_cast<unsigned long long>(out.sliders), static_cast<unsigned long long>(out.discontinuities) );
    check( out.min_margin > 6.f, name, "emitted hand path keeps comfortable follow margin" );
    check( out.failures == 0 && out.completed == out.sliders, name, "all sliders predict successful completion" );
    check( out.discontinuities == 0 && out.micro_plans == 0, name, "continuous output with no independent micro plan" );
    check( d.excessive_plan_intervals == 0 && d.target_coordinate_changes == 0, name,
        "no extra mini-plan chain or changing hit targets" );
    return out;
}
static slider_metrics_t single_case( const char* name, int duration, const char* path, float distance,
                                     int repeats = 1, point_t next = {420,240}, int gap = 180 ) {
    return measure_sliders( name, map_of( {circle(500,{80,192}),
        slider(1000,duration,{200,192},path,distance,repeats), circle(1000+duration+gap,next),
        circle(1000+duration+gap+180,{330,100})} ) );
}
static void break_case( int gap, point_t next, bool enabled, float rate = 1.f, bool perfect = false ) {
    const std::string name = "break " + std::to_string(gap) + ( next.x > 300 ? " far" : " near" ) +
        ( enabled ? " motion" : " quiet" ) + ( perfect ? " perfect" : "" );
    const int arrival = 1100 + static_cast<int>(gap * rate);
    auto map = map_of({circle(700,{120,192}),circle(1100,{160,200}),circle(arrival,next),circle(arrival+400,{280,140})});
    auto schedule = schedule_of(map); motion_engine_t engine; settings_t settings;
    settings.startup_motion = false; settings.break_motion = enabled; settings.perfect_play = perfect;
    engine.configure(settings); engine.begin_session({120,192},false,rate,map.objects.size(),0x7823u,true);
    int acquisition = -1, camp_ms = 0; uint64_t switches = 0; bool transition_continuous = true;
    plan_type_t previous_type = plan_type_t::none;
    for ( int t = 350; t <= arrival + 12; t += 4 ) {
        const auto before = engine.motion(); engine.update(map,schedule,t,0.004f/rate);
        const auto& p = engine.plan();
        if ( p.type == plan_type_t::gameplay && p.object_index == 2 && acquisition < 0 ) {
            acquisition = t;
            transition_continuous = length(p.p0-before.position)<0.001f &&
                length(p.v0-before.velocity)<0.01f && length(p.a0-before.acceleration)<0.1f;
        }
        if ( t > 1100 && t < arrival && p.type != previous_type ) ++switches;
        previous_type = p.type;
        if ( t > 1300 && t < arrival - static_cast<int>(1800*rate) &&
             length(engine.position()-next)<hit_radius(map.cs,false) && length(engine.motion().velocity)<20.f ) camp_ms += 4;
    }
    const float lead = acquisition < 0 ? -1.f : (arrival-acquisition)/rate;
    std::printf("  %-28s acquisition lead=%6.1f ms early camp=%d ms switches=%llu disc=%llu\n",name.c_str(),lead,
        camp_ms,static_cast<unsigned long long>(switches),static_cast<unsigned long long>(engine.diagnostics().unexpected_discontinuities));
    check( lead > 150.f && lead < 1500.f, name.c_str(), "acquisition is a bounded late lead-in, not the entire break" );
    check( camp_ms == 0, name.c_str(), "does not camp at the future hit circle early" );
    check( transition_continuous && engine.diagnostics().unexpected_discontinuities == 0, name.c_str(), "break feeds exact p/v/a into acquisition" );
}
static void pressure_case() {
    auto map_for = [](int interval) {
        std::vector<osu::hit_object_t> objects{circle(500,{180,192}),
            slider(1000,800,{200,192},"B|220:170|245:192",55.f)};
        for(int i=0;i<7;++i) objects.push_back(circle(2150+i*interval,{270.f+20.f*(i%3),200.f+15.f*(i%2)}));
        return map_of(std::move(objects));
    };
    const auto low=measure_sliders("pressure low",map_for(800));
    const auto high=measure_sliders("pressure high",map_for(90));
    std::printf("  pressure %.3f -> %.3f, economy %.3f -> %.3f, hand %.2f -> %.2f\n",
        low.pressure,high.pressure,low.economy,high.economy,low.hand,high.hand);
    check(high.pressure>low.pressure+0.15f,"pressure","upcoming dense links continuously raise motion pressure");
    check(high.hand<low.hand*0.80,"pressure","same geometry/timing uses less hand travel under pressure");
}
static void boundary_curve_cases() {
    plan_t p{}; p.p0={260,120}; p.v0={-281,-774}; p.a0={8309,30339};
    p.p1={339,87}; p.v1={35,20}; p.a1={};
    bool ordinary_identical=true;
    for(int i=0;i<=1000;++i) {
        const float u=i/1000.f;
        const auto a=eval_acquisition_curve(p,0.505f,u);
        const auto b=eval_quintic(p.p0,p.v0,p.a0,p.p1,p.v1,p.a1,0.505f,u);
        ordinary_identical = ordinary_identical && length(a.position-b.position)<0.00001f &&
            length(a.velocity-b.velocity)<0.00001f && length(a.acceleration-b.acceleration)<0.00001f;
    }
    check(ordinary_identical,"circle curves","ordinary acquisition remains numerically identical");
    p.slider_exit_entry=true; p.entry_response_seconds=0.09f;
    const auto start=eval_acquisition_curve(p,0.505f,0.f), end=eval_acquisition_curve(p,0.505f,1.f);
    check(length(start.position-p.p0)<0.001f && length(start.velocity-p.v0)<0.01f &&
        length(start.acceleration-p.a0)<0.1f,"slider boundary","exact initial p/v/a preserved");
    check(length(end.position-p.p1)<0.001f && length(end.velocity-p.v1)<0.01f &&
        length(end.acceleration-p.a1)<0.1f,"slider boundary","exact final p/v/a preserved");
    const float u=p.entry_response_seconds/0.505f;
    const auto before=eval_acquisition_curve(p,0.505f,u-0.000001f);
    const auto after=eval_acquisition_curve(p,0.505f,u+0.000001f);
    check(length(before.position-after.position)<0.001f && length(before.velocity-after.velocity)<0.1f &&
        length(before.acceleration-after.acceleration)<10.f,"slider boundary","localized derivative transfer is C2 continuous");
}
static void dense_stress() {
    uint64_t sliders=0, completed=0, failures_seen=0, discontinuities=0;
    float minimum_margin=1e6f, maximum_step=0.f;
    for(uint32_t seed=1;seed<=12;++seed) {
        std::vector<osu::hit_object_t> objects; int t=500;
        for(int i=0;i<300;++i) {
            t += i%71==70 ? 3000+static_cast<int>(seed%4)*1000 : 100+(i%4)*40;
            const point_t p{220.f+85.f*std::sin(i*0.4f+seed),192.f+60.f*std::sin(i*0.3f)};
            if(i%3==1) {
                const int duration=80+(i%4)*80;
                const std::string curve="B|"+std::to_string(p.x+20)+":"+std::to_string(p.y-18)+"|"+
                    std::to_string(p.x+40)+":"+std::to_string(p.y);
                objects.push_back(slider(t,duration,p,curve.c_str(),48.f,1+i%2)); t+=duration;
            } else objects.push_back(circle(t,p));
        }
        const float rates[]={0.75f,1.f,1.5f,2.f}; const int steps[]={1,4,8};
        const auto m=measure_sliders("M dense stress",map_of(std::move(objects)),0x711u+seed,
            rates[seed%4],steps[seed%3],false);
        sliders+=m.sliders; completed+=m.completed; failures_seen+=m.failures; discontinuities+=m.discontinuities;
        minimum_margin=std::min(minimum_margin,m.min_margin); maximum_step=std::max(maximum_step,m.max_step);
    }
    std::printf("  M stress: 3600 objects, sliders %llu/%llu, failures %llu, disc %llu, min margin %.2f, max step %.2f\n",
        static_cast<unsigned long long>(completed),static_cast<unsigned long long>(sliders),
        static_cast<unsigned long long>(failures_seen),static_cast<unsigned long long>(discontinuities),minimum_margin,maximum_step);
    check(sliders==1200 && completed==sliders && failures_seen==0,"M dense stress","all 1200 sliders remain safe across speeds/seeds");
}
int main() {
    std::puts("Autobot emitted hand-motion / break tests");
    const auto a=single_case("A tiny curve 120ms",120,"B|220:170|245:192",55.f);
    const auto b=single_case("B tiny loop 140ms",140,"B|220:170|240:192|220:214|200:192",82.f);
    const auto c=single_case("C tiny repeat 140ms",140,"L|238:192",38.f,2);
    single_case("D medium slow curve",1000,"B|250:115|320:250|365:200",185.f);
    single_case("E long required track",1800,"B|300:60|450:320|470:192",440.f);
    const auto h=single_case("H tiny next close",180,"B|220:170|245:192",55.f,1,{248,198},75);
    const auto i=single_case("I tiny next far",180,"B|220:170|245:192",55.f,1,{455,310},100);
    for ( const auto& tiny : {a,b,c,h,i} ) check(tiny.hand < tiny.ball*.65,"tiny slider economy","hand travels substantially less than ball with a real next target");
    std::vector<osu::hit_object_t> dense{circle(500,{180,192})};
    for(int n=0;n<16;++n) { const int t=800+n*390; dense.push_back(circle(t,{185,192}));
        dense.push_back(slider(t+100,120,{210,192},"B|225:175|245:192",42.f)); dense.push_back(circle(t+310,{270,210})); }
    measure_sliders("F dense circle/slider",map_of(dense));
    std::vector<osu::hit_object_t> consecutive{circle(500,{180,192})};
    for(int n=0;n<12;++n) consecutive.push_back(slider(800+n*220,130,{200,192},"B|220:170|245:192",55.f));
    consecutive.push_back(circle(800+12*220,{350,200}));
    measure_sliders("G consecutive short",map_of(consecutive));
    for(int gap : {3000,5000,6000}) {break_case(gap,{205,205},true);break_case(gap,{455,300},true);}
    break_case(5000,{455,300},false); break_case(5000,{455,300},true,1.5f,true);
    pressure_case(); boundary_curve_cases(); dense_stress();
    std::printf("%d checks, %d failures\n",checks,failures); return failures ? 1 : 0;
}
