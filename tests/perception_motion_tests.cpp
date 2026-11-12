#include <core/autobot/motion_core.hxx>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace autobot;
static int checks=0, failures=0;
static bool baseline=false;
static void check(bool condition,const char* message) {
    ++checks; if(!condition && !baseline) {++failures;std::printf("FAIL %s\n",message);}
}
static osu::hit_object_t circle(int time,point_t p) {
    osu::hit_object_t o{};o.type=1;o.start_time=o.end_time=time;o.x=p.x;o.y=p.y;return o;
}
static osu::hit_object_t slider(int time,int duration,point_t p,const char* path,float distance=55.f) {
    auto o=circle(time,p);o.type=2;o.end_time+=duration;o.slider_curve_str=path;
    o.slider_length=distance;o.slider_repeat=1;return o;
}
static osu::hit_object_t spinner(int time,int duration) {
    auto o=circle(time,{256,192});o.type=8;o.end_time+=duration;return o;
}
static osu::beatmap_data_t map_of(std::vector<osu::hit_object_t> objects,float ar=10.f) {
    osu::beatmap_data_t map{};map.loaded=true;map.cs=4.f;map.od=8.f;map.ar=ar;map.objects=std::move(objects);return map;
}
static schedule_view_t schedule_of(const osu::beatmap_data_t& map) {
    schedule_view_t s;for(const auto& o:map.objects) {
        s.press_time.push_back(o.start_time);s.release_time.push_back(o.end_time+24);s.edge_bias.push_back(0.f);
    }return s;
}
static motion_engine_t engine_for(const osu::beatmap_data_t& map,bool decorations=false,float rate=1.f) {
    motion_engine_t e;settings_t s;s.startup_motion=false;s.break_motion=decorations;
    e.configure(s);e.begin_session({180,180},false,rate,map.objects.size(),0x7214u,true);return e;
}
static int expected_preempt(float ar) {
    return static_cast<int>(ar<=5.f?1800.f-120.f*ar:1200.f-150.f*(ar-5.f));
}
static void leakage_case(const char* name,int gap,bool future_slider,bool previous_slider=false,bool orientation_only=false) {
    const int previous_end=previous_slider?1800:1500, future_time=previous_end+gap;
    std::vector<osu::hit_object_t> prefix{circle(1000,{180,180})};
    if(previous_slider) prefix.push_back(slider(1500,300,{240,180},"B|260:160|285:180"));
    else prefix.push_back(circle(1500,{240,180}));
    auto left=prefix,right=prefix;
    if(future_slider) {
        left.push_back(slider(future_time,180,orientation_only?point_t{256,192}:point_t{70,192},"B|45:155|25:192"));
        right.push_back(slider(future_time,180,orientation_only?point_t{256,192}:point_t{440,192},"B|465:230|490:192"));
    } else {left.push_back(circle(future_time,{70,192}));right.push_back(circle(future_time,{440,192}));}
    // Equal later timing; different hidden vectors also exercise the flow/pressure lookahead.
    for(int i=1;i<=4;++i) {
        const point_t common{270.f+i*12,200};
        left.push_back(circle(future_time+500+i*100,orientation_only?common:point_t{60.f+i*12,100}));
        right.push_back(circle(future_time+500+i*100,orientation_only?common:point_t{450.f-i*12,290}));
    }
    const auto a=map_of(left),b=map_of(right);auto sa=schedule_of(a),sb=schedule_of(b);
    auto ea=engine_for(a,gap>=2400),eb=engine_for(b,gap>=2400);
    const int perception=future_time-expected_preempt(a.ar);
    float leakage=0.f,divergence=0.f;int first_difference=-1;
    for(int t=400;t<=future_time;t+=2) {
        ea.update(a,sa,t,0.002f);eb.update(b,sb,t,0.002f);
        const float d=length(ea.position()-eb.position());
        if(t<perception) leakage=std::max(leakage,d);else divergence=std::max(divergence,d);
        if(d>0.001f && first_difference<0) first_difference=t;
    }
    std::printf("  leakage %-29s max=%10.6f perception=%d first difference=%d after=%.2f\n",name,leakage,perception,first_difference,divergence);
    check(leakage<0.0001f,"hidden future spatial information cannot alter emitted motion");
    check(orientation_only || divergence>20.f,"perceived left/right targets can affect acquisition");
    check(ea.diagnostics().unexpected_discontinuities==0 && eb.diagnostics().unexpected_discontinuities==0,"perception handoff has no discontinuity");
}
struct metrics_t {float speed=0,accel=0,jerk=0,p95=0;uint64_t reversals=0,plans=0,replacements=0;};
static void timing_and_commitment() {
    check(object_preempt_ms(0)==1800 && object_preempt_ms(5)==1200 && object_preempt_ms(9)==600 &&
        object_preempt_ms(10)==450 && object_preempt_ms(11)==300,"real AR preempt model, not a fixed delay");
    check(object_preempt_ms(7,true)==480 && object_preempt_ms(10,true)==450,"only unapplied HR changes AR");
    for(float rate:{.75f,1.f,1.5f,2.f}) {
        auto a=map_of({circle(1000,{180,180}),circle(1800,{70,192})}),b=a;b.objects[1].x=440;
        auto schedule=schedule_of(a);auto ea=engine_for(a,false,rate),eb=engine_for(b,false,rate);
        bool coherent=true;float commitment=0;float leakage=0;
        for(int t=600;t<=1800;t+=2) {
            // Prepared scheduling clock leads the rendered/visible game by 12ms.
            ea.update(a,schedule,t+12,.002f/rate,t);eb.update(b,schedule,t+12,.002f/rate,t);
            if(t<1350) leakage=std::max(leakage,length(ea.position()-eb.position()));
            if(ea.plan().object_index==1) {
                const float c=ea.diagnostics().target_commitment;
                coherent=coherent && c+0.00001f>=commitment;commitment=c;
            }
        }
        check(leakage<0.0001f,"prepared clock and speed mods cannot reveal future geometry early");
        check(coherent && commitment>.999f,"commitment is monotone through acquisition and reaches one");
        check(ea.diagnostics().perception_promotions==1,"hidden continuation promotes the same intent exactly once");
    }
    plan_t p{};p.p0={180,180};p.p1={400,200};p.v0={45,15};p.a0={30,-10};p.v1={30,5};p.a1={};p.commitment_softness=.7f;
    const auto a=eval_acquisition_curve(p,.6f,0),b=eval_acquisition_curve(p,.6f,1);
    check(length(a.position-p.p0)<.001f && length(a.velocity-p.v0)<.001f && length(a.acceleration-p.a0)<.001f,
        "soft commitment retains exact initial p/v/a");
    check(length(b.position-p.p1)<.01f && length(b.velocity-p.v1)<.02f && length(b.acceleration-p.a1)<.1f,
        "soft commitment retains exact arrival p/v/a");
}
static metrics_t measure(const char* name,const osu::beatmap_data_t& map,int end=0) {
    auto schedule=schedule_of(map);auto e=engine_for(map,true);
    point_t p=e.position(),v{},a{};std::vector<float> jerks;metrics_t m{};int count=0;
    if(!end) end=map.objects.back().end_time+150;
    for(int t=400;t<=end;t+=2) {
        e.update(map,schedule,t,0.002f);const point_t nv=(e.position()-p)/0.002f,na=(nv-v)/0.002f;
        if(count++>4) {
            const float j=length(na-a)/0.002f;jerks.push_back(j);m.jerk=std::max(m.jerk,j);
            m.speed=std::max(m.speed,length(nv));m.accel=std::max(m.accel,length(na));
            if(length(a)>400.f && length(na)>400.f && dot(normalized(a),normalized(na))<-.25f) ++m.reversals;
        }
        p=e.position();v=nv;a=na;
    }
    std::sort(jerks.begin(),jerks.end());m.p95=jerks[static_cast<size_t>(jerks.size()*.95)];
    const auto& d=e.diagnostics();m.plans=d.plans_created;m.replacements=d.plan_transitions;
    std::printf("  dynamics %-23s v=%7.0f a=%9.0f jerk=%11.0f p95=%10.0f reversals=%llu plans=%llu changes=%llu disc=%llu\n",
        name,m.speed,m.accel,m.jerk,m.p95,(unsigned long long)m.reversals,(unsigned long long)m.plans,
        (unsigned long long)m.replacements,(unsigned long long)d.unexpected_discontinuities);
    check(d.unexpected_discontinuities==0,"emitted scenario has no unexpected discontinuity");
    check(d.slider_follow_failures==0 && d.slider_head_failures==0,"slider integrity retained");
    return m;
}
int main(int argc,char**) {
    baseline=argc>1;std::puts("Perception leakage / emitted dynamics");
    timing_and_commitment();
    leakage_case("circle -> 500 -> circle",500,false);
    leakage_case("circle -> 500 -> slider",500,true);
    leakage_case("circle -> 800 -> slider",800,true);
    leakage_case("break -> next circle",5000,false);
    leakage_case("slider -> hidden slider",500,true,true);
    leakage_case("hidden slider orientation",800,true,false,true);
    for(int gap:{300,500,800}) {
        const std::string label="circle gap "+std::to_string(gap);
        measure(label.c_str(),map_of({circle(1000,{180,180}),circle(1300,{230,180}),circle(1300+gap,{430,250}),circle(1700+gap,{300,90})}));
    }
    measure("circle -> short slider",map_of({circle(1000,{180,180}),circle(1300,{230,180}),slider(1800,140,{400,180},"B|420:165|445:180"),circle(2100,{320,130})}));
    measure("slider -> circle",map_of({circle(1000,{180,180}),slider(1300,180,{230,180},"B|250:160|275:180"),circle(1980,{430,260}),circle(2250,{280,90})}));
    measure("slider -> slider",map_of({circle(1000,{180,180}),slider(1300,180,{230,180},"B|250:160|275:180"),slider(1980,160,{410,250},"B|390:225|365:250"),circle(2400,{280,90})}));
    std::vector<osu::hit_object_t> dense;int t=1000;
    for(int i=0;i<100;++i) {
        const point_t p{256+120.f*std::sin(i*.3f),192+70.f*std::cos(i*.2f)};
        if(i%4==2) {const auto path="L|"+std::to_string(p.x+40)+":"+std::to_string(p.y);dense.push_back(slider(t,120,p,path.c_str(),40));t+=120;}
        else dense.push_back(circle(t,p));t+=140;
    }
    measure("dense mixed",map_of(dense));
    for(int gap:{3000,5000,6000}) {const auto label="break "+std::to_string(gap);measure(label.c_str(),map_of({circle(1000,{180,180}),circle(1300,{230,180}),circle(1300+gap,{430,260}),circle(1700+gap,{280,90})}));}
    measure("spinner entry/body/exit",map_of({circle(1000,{180,180}),spinner(1700,2000),circle(4050,{420,150}),circle(4400,{250,100})}));
    std::printf("%d checks, %d failures%s\n",checks,failures,baseline?" (baseline observation)":"");return failures?1:0;
}
