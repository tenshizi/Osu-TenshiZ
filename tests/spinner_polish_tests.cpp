#include <core/autobot/motion_core.hxx>
#include <algorithm>
#include <cstdio>
#include <vector>
#include <numeric>
using namespace autobot;
static int checks=0,failures=0;
static void check(bool yes,const char* message) {++checks;if(!yes){++failures;std::printf("FAIL %s\n",message);}}
static float signed_angle(point_t a,point_t b) {
    return std::atan2(a.x*b.y-a.y*b.x,dot(a,b));
}
static osu::hit_object_t spinner(int start,int duration) {
    osu::hit_object_t o{};o.type=8;o.start_time=start;o.end_time=start+duration;o.x=256;o.y=192;return o;
}
struct shape_metrics_t {
    float mean=0,sd=0,minimum=1e6f,maximum=0,centroid=0,ratio=0,orientation=0;
    float rpm_min=1e6f,rpm_max=0,rpm_drift=0,turns=0,error=0,max_step=0;
    float radius_change=0;
    int reversals=0;
};
static shape_metrics_t sample_spinner(uint32_t seed,float lo,float hi,float duration,float dt,float rate,float direction,bool report=false) {
    auto o=spinner(0,static_cast<int>(duration*rate*1000));
    const auto style=make_spinner_style(seed,duration);const auto range=sanitize_spinner_range(lo,hi);
    const auto start=spinner_entry_kinematics(style,{256,192},0.3,direction,range);
    motion_state_t m{start.position,start.velocity,start.acceleration,true};
    spinner_state_t sp;spinner_begin(sp,0,o,false,m,seed,direction,&style);
    const auto immutable_style=sp.style;
    point_t last=m.position;double sx=0,sy=0,sxx=0,syy=0,sxy=0,sr=0,srr=0;
    double early=0,late=0;int ne=0,nl=0,n=0;shape_metrics_t result;
    float smallest_command=1e6f,largest_command=0;
    const int frames=static_cast<int>(std::lround(duration/dt));
    for(int i=1;i<=frames;++i) {
        const double now=i*static_cast<double>(dt);
        const auto p=spinner_step_range(sp,o,static_cast<int>(now*rate*1000),dt,lo,hi);
        const point_t q=p-sp.center,prev=last-sp.center;
        const float delta=signed_angle(prev,q),rpm=std::abs(delta)*60.f/(2.f*k_pi*dt);
        result.rpm_min=std::min(result.rpm_min,rpm);result.rpm_max=std::max(result.rpm_max,rpm);
        result.turns+=std::abs(delta)/(2.f*k_pi);
        if(delta*direction<-0.000001f) ++result.reversals;
        result.max_step=std::max(result.max_step,length(p-last));last=p;
        smallest_command=std::min(smallest_command,sp.requested_rpm);largest_command=std::max(largest_command,sp.requested_rpm);
        const float radius=length(q);result.minimum=std::min(result.minimum,radius);result.maximum=std::max(result.maximum,radius);
        if(now>0.25) {
            sx+=q.x;sy+=q.y;sxx+=q.x*q.x;syy+=q.y*q.y;sxy+=q.x*q.y;sr+=radius;srr+=radius*radius;++n;
            if(now<duration*.35){early+=radius;++ne;}if(now>duration*.65){late+=radius;++nl;}
        }
    }
    if(n>1) {
        const double mx=sx/n,my=sy/n,xx=sxx/n-mx*mx,yy=syy/n-my*my,xy=sxy/n-mx*my;
        const double root=std::sqrt((xx-yy)*(xx-yy)+4*xy*xy),sum=xx+yy;
        result.mean=static_cast<float>(sr/n);result.sd=static_cast<float>(std::sqrt(std::max(0.0,srr/n-sr/n*sr/n)));
        result.centroid=static_cast<float>(std::hypot(mx,my));
        result.ratio=static_cast<float>(std::sqrt((sum+root)/std::max(sum-root,0.001)));
        result.orientation=static_cast<float>(0.5*std::atan2(2*xy,xx-yy)*180/k_pi);
        if(ne && nl) result.radius_change=static_cast<float>(late/nl-early/ne);
    }
    result.error=std::abs(result.turns-sp.revolutions);result.rpm_drift=largest_command-smallest_command;
    check(sp.style.radius_knots==immutable_style.radius_knots && sp.style.rpm_knots==immutable_style.rpm_knots &&
        sp.style.aspect_knots==immutable_style.aspect_knots,"frame evaluation never resamples spinner style");
    check(result.rpm_min>=lo-0.08f && result.rpm_max<=hi+0.08f,"actual emitted polar RPM stays in range (float tolerance 0.08 RPM)");
    check(smallest_command>=lo && largest_command<=hi,"instantaneous commanded RPM stays in range");
    check(result.error<0.025f,"actual revolutions match integrated RPM");
    check(result.reversals==0,"emitted signed direction never reverses");
    check(result.minimum>20.01f && result.maximum<161.99f,"style retains radial safety margin without hitting guards");
    if(report) std::printf("  spinner %u %.0f..%.0f duration %.2f dt %.4f rate %.2f: RPM %.3f..%.3f drift %.2f turns %.3f err %.5f radius %.1f sd %.1f min/max %.1f/%.1f axes %.2f @ %.1f centroid %.1f\n",
        seed,lo,hi,duration,dt,rate,result.rpm_min,result.rpm_max,result.rpm_drift,result.turns,result.error,
        result.mean,result.sd,result.minimum,result.maximum,result.ratio,result.orientation,result.centroid);
    return result;
}
static void style_distribution() {
    std::vector<shape_metrics_t> all;int buckets[5]{};int increasing=0,decreasing=0;
    for(uint32_t seed=1;seed<=512;++seed) {
        auto m=sample_spinner(mix_seed(seed*0x9221u),330,390,8.f,0.004f,1.f,seed%2?1.f:-1.f);
        ++buckets[m.mean<52?0:m.mean<65?1:m.mean<78?2:m.mean<91?3:4];
        if(m.radius_change>1.f)++increasing;if(m.radius_change<-1.f)++decreasing;all.push_back(m);
    }
    float min_mean=1e6f,max_mean=0,min_ratio=1e6f,max_ratio=0,min_orientation=180,max_orientation=-180;
    float min_sd=1e6f,max_sd=0,min_centroid=1e6f,max_centroid=0;
    for(const auto& m:all) {
        min_mean=std::min(min_mean,m.mean);max_mean=std::max(max_mean,m.mean);
        min_ratio=std::min(min_ratio,m.ratio);max_ratio=std::max(max_ratio,m.ratio);
        min_orientation=std::min(min_orientation,m.orientation);max_orientation=std::max(max_orientation,m.orientation);
        min_sd=std::min(min_sd,m.sd);max_sd=std::max(max_sd,m.sd);
        min_centroid=std::min(min_centroid,m.centroid);max_centroid=std::max(max_centroid,m.centroid);
    }
    for(int n:buckets)check(n>=25,"all five broad radius regions are meaningfully represented");
    check(max_mean-min_mean>50,"mean radius diversity is visually meaningful");
    check(max_ratio-min_ratio>.3f && min_ratio>1.03f,"covariance eccentricity varies without perfect circles");
    check(max_orientation-min_orientation>150,"principal-axis orientation covers broad directions");
    check(increasing>80 && decreasing>80,"coherent radius evolution includes both expansion and contraction");
    std::printf("  512 styles: mean radius %.2f..%.2f buckets %d/%d/%d/%d/%d radial SD %.2f..%.2f axis ratio %.3f..%.3f orientation %.1f..%.1f centroid %.2f..%.2f expanding/contracting %d/%d\n",
        min_mean,max_mean,buckets[0],buckets[1],buckets[2],buckets[3],buckets[4],min_sd,max_sd,min_ratio,max_ratio,
        min_orientation,max_orientation,min_centroid,max_centroid,increasing,decreasing);
}
static void session_direction() {
    uint64_t discontinuities=0;float worst_entry_speed_change=0,worst_entry_position_residual=0;
    for(uint32_t seed=1;seed<=24;++seed) {
        osu::beatmap_data_t map{};map.loaded=true;map.ar=10;map.cs=4;
        for(int i=0;i<4;++i)map.objects.push_back(spinner(1400+i*2400,1800));
        schedule_view_t schedule;
        for(const auto& o:map.objects){schedule.press_time.push_back(o.start_time);schedule.release_time.push_back(o.end_time+24);}
        motion_engine_t engine;settings_t settings;settings.startup_motion=false;
        const float rates[]={.75f,1.f,1.5f,2.f};const float rate=rates[seed%4],dt=.002f/rate;
        settings.spinner_rpm_min=330;settings.spinner_rpm_max=390;engine.configure(settings);
        engine.begin_session({180,180},false,rate,map.objects.size(),seed,true);
        // Preserve the old RNG sequence and prove its direction is negated once.
        rng_t legacy;legacy.s=seed;for(int i=0;i<9;++i)legacy.unit();const float old_direction=legacy.unit()<.5f?-1.f:1.f;
        const float wanted=-old_direction;
        check(engine.diagnostics().spinner_preferred_direction==wanted,"session polarity is exactly opposite old seeded direction");
        point_t previous=engine.position();int active=-1;float lo=1e6f,hi=0;
        for(int t=500;t<=10400;t+=2) {
            const auto old_motion=engine.motion();engine.update(map,schedule,t,dt);
            const auto& p=engine.plan();
            if(p.type==plan_type_t::spinner) {
                const point_t center{256,192};const float delta=signed_angle(previous-center,engine.position()-center);
                check(delta*wanted>=-0.000001f,"all four emitted session spinners use the authoritative direction");
                const float rpm=std::abs(delta)*60.f/(2.f*k_pi*dt);lo=std::min(lo,rpm);hi=std::max(hi,rpm);
                if(p.object_index!=active) {
                    worst_entry_speed_change=std::max(worst_entry_speed_change,length(engine.motion().velocity-old_motion.velocity));
                    const point_t expected = old_motion.position + old_motion.velocity * dt + old_motion.acceleration * (0.5f*dt*dt);
                    worst_entry_position_residual = std::max(worst_entry_position_residual, length(engine.position() - expected));
                }
                active=p.object_index;
            }
            previous=engine.position();
        }
        check(lo>=329.9f && hi<=390.1f,"engine emitted spinner RPM is bounded, including entry/exit frames");
        check(engine.diagnostics().spinner_direction_changes_between_objects==0,"no session direction changes");
        discontinuities+=engine.diagnostics().unexpected_discontinuities;
    }
    std::printf("  24 sessions / 96 spinners: discontinuities %llu, worst entry velocity step %.2f\n",(unsigned long long)discontinuities,worst_entry_speed_change);
    std::printf("  Entry residual from incoming p/v/a: %.4f px\n", worst_entry_position_residual);
    check(worst_entry_position_residual < 0.75f, "entry follows incoming position/velocity/acceleration without a reset");
    check(discontinuities==0,"spinner acquisition/body/exit remain continuous");
}
static void outside_radius_entry() {
    const auto o=spinner(0,3000);const auto style=make_spinner_style(77,3.f);
    const spinner_rpm_range_t range{330,390};
    const float omega=spinner_rpm_at(style,range,0)*(2.f*k_pi/60.f);
    motion_state_t incoming{{506,192},{0,250.f*omega},{-250.f*omega*omega,0},true};
    spinner_state_t sp;spinner_begin(sp,0,o,false,incoming,77,1.f,&style);
    const auto first=spinner_step_range(sp,o,1,.001f,330,390);
    check(length(first-sp.center)>248.f && length(first-incoming.position)<12.f,
        "outside-radius entry does not teleport onto the style boundary");
    for(int t=2;t<=250;++t)spinner_step_range(sp,o,t,.001f,330,390);
    check(sp.radius>20.f && sp.radius<162.f,"outside-radius entry converges to the safe style region");
}
int main() {
    std::puts("Spinner emitted RPM / shape / direction matrix");
    sample_spinner(11,330,390,.35f,.002f,1,1,true);
    sample_spinner(22,330,390,20.f,.004f,1,-1,true);
    sample_spinner(33,200,240,6.f,.001f,1,1,true);
    sample_spinner(44,430,477,6.f,.008f,1,-1,true);
    sample_spinner(55,359,361,8.f,.004f,1,1,true);
    sample_spinner(66,200,477,8.f,.004f,1,-1,true);
    for(float rate:{.75f,1.f,1.5f,2.f})for(float dt:{.00032f,.001f,.004f,.008f,.016f})
        sample_spinner(77,330,390,5.f,dt,rate,-1);
    style_distribution();session_direction();outside_radius_entry();
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
