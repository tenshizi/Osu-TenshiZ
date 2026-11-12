#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <core/game/osu_lazer.hxx>
#include <core/game/lazer_live_feedback.hxx>
#include <core/beatmap/lazer_ruleset_reader.hxx>
#include <chrono>
#include <cstdio>
#include <cmath>

static std::vector<uint64_t> list( memory::c_process& p, uint64_t ptr ) {
    const int n = p.read<int>( ptr + 0x10 );
    const auto items = p.read<uint64_t>( ptr + 8 );
    if ( !items || n < 0 || n > 512 ) return {};
    std::vector<uint64_t> out( n );
    if ( n && !p.read_buffer( items + 0x10, out.data( ), n * 8 ) ) return {};
    return out;
}
static double start( memory::c_process& p, uint64_t drawable ) {
    const auto b = p.read<uint64_t>( drawable + 0x370 );
    return p.read<double>( b + 0x40 );
}
static void dump( memory::c_process& p, uint64_t drawable, const char* kind ) {
    const auto entry = p.read<uint64_t>( drawable + 0x320 );
    const auto object = p.read<uint64_t>( entry + 0x38 );
    const auto result = p.read<uint64_t>( entry + 0x48 );
    const int grade = p.read<int>( result + 0x20 );
    const auto area = p.read<uint64_t>( drawable + 0x410 );
    const auto draw = p.read<uint64_t>( area + 0xb0 );
    const auto size = p.read<uint64_t>( area + 0x70 );
    float matrix[9]{}, dimensions[2]{};
    p.read_buffer( draw + 0x2c, matrix, sizeof matrix );
    p.read_buffer( size + 0x2c, dimensions, sizeof dimensions );
    const float x = dimensions[0] * .5f, y = dimensions[1] * .5f;
    const float sx = x * matrix[0] + y * matrix[3] + matrix[6];
    const float sy = x * matrix[1] + y * matrix[4] + matrix[7];
    std::printf( "%s drawable=%llx entry=%llx object=%llx time=%.1f result=%d area=%llx hovered=%d size=%.2f,%.2f center=%.2f,%.2f scale=%.3f,%.3f\n",
        kind, drawable, entry, object, start(p,drawable), grade, area,
        p.read<uint8_t>(area + 0x144), dimensions[0], dimensions[1], sx, sy, matrix[0], matrix[4] );
}
int main( ) {
    memory::c_process p; p.enable_fallback( );
    const int pid = memory::c_process::find_pid_by_name( L"osu!.exe" );
    if ( !p.attach(pid) ) return 1;
    game::c_osu_lazer client( offsets::lazer::table_t{} );
    if ( !client.attach(p) ) return 2;
    osu::game_snapshot_t snap{}; client.update(p,snap);
    const auto feedback = game::c_lazer_live_feedback{}.read(p,snap.drawable_ruleset,snap.cur_time);
    std::printf("validated_feedback=%d\n",feedback ? 1 : 0);
    if(feedback) {
        std::printf("paused=%d replay=%d objects=%zu\n",feedback->paused,feedback->replay,feedback->objects.size());
        for(const auto& o : feedback->objects) std::printf("live t=%d judged=%d hit=%d hover=%d slider=%d tracking=%d center=%.2f,%.2f radius=%.2f great=%.2f late=%.2f ball=%.2f,%.2f\n",o.start_time,o.judged,o.hit,o.hovered,o.slider,o.tracking,o.client_x,o.client_y,o.radius,o.great_window,o.late_window,o.ball_x,o.ball_y);
        osu::beatmap_data_t map{};
        beatmap::c_lazer_ruleset_reader map_reader;
        if(map_reader.try_load(p,snap,offsets::lazer::table_t{},map)) {
            size_t matches=0;
            for(const auto& live:feedback->objects) for(const auto& obj:map.objects)
                if(live.source_object==obj.source_object && live.start_time==obj.start_time) {++matches;break;}
            std::printf("live_to_map_identity_matches=%zu/%zu\n",matches,feedback->objects.size());
        }
        const auto begin=std::chrono::steady_clock::now();int valid=0;
        for(int i=0;i<50;++i) if(game::c_lazer_live_feedback{}.read(p,snap.drawable_ruleset,snap.cur_time))++valid;
        const double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-begin).count()/50;
        std::printf("feedback_read_avg_us=%.1f valid_reads=%d/50\n",us,valid);
    }
    const auto lazy = p.read<uint64_t>(snap.drawable_ruleset + 0x360);
    const auto playfield = p.read<uint64_t>(lazy + 0x18);
    const auto container_lazy = p.read<uint64_t>(playfield + 0x320);
    const auto container = p.read<uint64_t>(container_lazy + 0x18);
    const auto sorted = p.read<uint64_t>(container + 0x230);
    const auto children = list(p,p.read<uint64_t>(sorted + 8));
    std::printf("time=%d ruleset=%llx playfield=%llx container=%llx alive=%zu\n", snap.cur_time,snap.drawable_ruleset,playfield,container,children.size());
    for (auto child : children) {
        const auto nested = list(p,p.read<uint64_t>(child+0x350));
        if (nested.empty()) dump(p,child,"circle");
        else {
            std::printf("duration drawable=%llx time=%.1f nested=%zu\n",child,start(p,child),nested.size());
            for(auto head : nested) if(std::abs(start(p,head)-start(p,child))<.5) dump(p,head,"head");
        }
    }
    p.detach();
}
