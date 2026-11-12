#define TENSHIZ_RELAX_TESTING
#include <core/relax/relax.hxx>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0, checks = 0;
static void check( bool condition, const char* profile, float target, const char* message ) {
    ++checks;
    if ( !condition ) {
        ++failures;
        std::printf( "  [FAIL] %-12s %5.1f%% : %s\n", profile, target, message );
    }
}

enum class profile_t { circles, stream, mixed, sparse };

static osu::beatmap_data_t make_map( profile_t profile ) {
    osu::beatmap_data_t map{};
    map.loaded = true; map.od = 8.f; map.cs = 4.f;
    int time = 1000;
    constexpr int count = 720;
    for ( int i = 0; i < count; ++i ) {
        osu::hit_object_t object{};
        int interval = 180;
        if ( profile == profile_t::stream ) interval = 82;
        if ( profile == profile_t::mixed ) interval = 115 + ( i % 5 ) * 28;
        if ( profile == profile_t::sparse ) interval = 360 + ( i % 4 ) * 45;
        time += interval;
        object.start_time = time; object.end_time = time;
        object.x = 256.f + 150.f * std::sin( i * 0.31f );
        object.y = 192.f + 110.f * std::sin( i * 0.19f + 0.8f );
        object.type = 1;
        if ( profile == profile_t::mixed && i % 7 == 3 ) {
            object.type = 2; object.end_time = time + 220;
            object.slider_curve_str = "L|320:192"; object.slider_length = 80.f; object.slider_repeat = 1;
        }
        map.objects.push_back( object );
    }
    return map;
}

static void run_profile( const char* name, profile_t profile, float target, uint32_t seed ) {
    auto map = make_map( profile );
    osu::game_snapshot_t game{};
    game.cur_state = osu::game_state_t::play;
    game.cur_time = 0;
    game.left_key = 'Z'; game.right_key = 'X';

    relax::c_relax timing;
    timing.seed_for_tests( seed );
    const bool perfect = target == 100.f;
    if ( perfect ) {
        timing.manual_offset_ms = 25;
        timing.early_variation_ms = -25;
        timing.late_variation_ms = 25;
    }
    timing.prepare_for_autobot( game, map, perfect ? 98.5f : target, perfect );
    const auto telemetry = timing.accuracy_telemetry( );

    check( telemetry.objects == map.objects.size( ), name, target, "all objects scheduled" );
    check( telemetry.intentional_miss == 0, name, target, "no intentional misses" );
    check( telemetry.unsafe_at_100 == 0, name, target, "100% timing guard remained feasible" );
    check( std::abs( telemetry.predicted - target ) <= 0.65f, name, target, "predicted accuracy converged" );
    check( telemetry.perfect_play == perfect, name, target, "explicit Perfect Play state propagated" );
    check( telemetry.grade_300 + telemetry.grade_100 + telemetry.grade_50 == telemetry.objects,
           name, target, "grade counts equal scheduled objects" );

    int previous = -999999;
    int key_release[ 2 ] = { -999999, -999999 };
    bool ordered = true, safe_grade = true, hold_safe = true, slider_tail_safe = true;
    for ( size_t i = 0; i < map.objects.size( ); ++i ) {
        relax::c_relax::scheduled_object_t scheduled{};
        if ( !timing.scheduled_object( i, scheduled ) ) { ordered = false; break; }
        ordered = ordered && scheduled.press_time > previous;
        previous = scheduled.press_time;
        safe_grade = safe_grade && scheduled.simulated_grade != 0;
        const int key = std::clamp( scheduled.key_index, 0, 1 );
        hold_safe = hold_safe && scheduled.press_time > key_release[ key ];
        key_release[ key ] = std::max( key_release[ key ], scheduled.release_time );
        if ( ( map.objects[ i ].type & 2 ) != 0 )
            slider_tail_safe = slider_tail_safe && scheduled.release_time >= map.objects[ i ].end_time + ( perfect ? 36 : 24 );
        if ( perfect ) {
            const float window_300 = std::max( 5.f, 79.5f - 6.f * map.od );
            safe_grade = safe_grade && std::abs( scheduled.press_time - map.objects[ i ].start_time ) <=
                static_cast<int>( std::floor( window_300 * 0.35f ) );
        }
    }
    check( ordered, name, target, "scheduled presses remain strictly ordered" );
    check( safe_grade, name, target, "controller never schedules a miss" );
    check( hold_safe, name, target, "no press is suppressed by an existing hold on the same key" );
    check( slider_tail_safe, name, target, "slider releases retain the protected tail margin" );
    if ( perfect )
        check( telemetry.controlled_100 == 0 && telemetry.controlled_50 == 0, name, target, "100% stays all-300" );
    if ( !perfect && target >= 95.f )
        check( telemetry.controlled_100 >= telemetry.controlled_50, name, target, "high targets prefer controlled 100s" );
    if ( target <= 85.f && profile == profile_t::sparse )
        check( telemetry.controlled_50 > 0 && telemetry.controlled_50 * 10 < telemetry.objects,
               name, target, "50s are present but sparse" );
}

static void run_perfect_object_mix( ) {
    const char* name = "perfect-mix";
    osu::beatmap_data_t map{}; map.loaded = true; map.od = 10.f; map.cs = 5.f;
    auto add = [&]( int start, int end, uint8_t type ) {
        osu::hit_object_t o{}; o.start_time = start; o.end_time = end; o.type = type;
        o.x = 256.f; o.y = 192.f;
        if ( type & 2 ) { o.slider_curve_str = "B|290:160|330:210"; o.slider_length = 110.f; o.slider_repeat = 2; }
        map.objects.push_back( o );
    };
    add( 1000, 1000, 1 ); add( 1250, 1250, 1 );
    for ( int i = 0; i < 8; ++i ) add( 1600 + i * 82, 1600 + i * 82, 1 );
    add( 2500, 3100, 2 ); add( 3400, 3400, 1 ); add( 3750, 4300, 2 );
    add( 4700, 6000, 8 ); add( 6300, 6300, 1 );
    osu::game_snapshot_t game{}; game.cur_state = osu::game_state_t::play;
    game.left_key = 'Z'; game.right_key = 'X'; game.speed_mult = 1.5f;
    relax::c_relax timing; timing.seed_for_tests( 0x7721u );
    timing.manual_offset_ms = 35; timing.early_variation_ms = -25; timing.late_variation_ms = 25;
    timing.prepare_for_autobot( game, map, 91.f, true );
    const auto telemetry = timing.accuracy_telemetry( );
    check( telemetry.perfect_play, name, 100.f, "Perfect Play is explicit" );
    check( telemetry.grade_300 == map.objects.size( ), name, 100.f, "circles/stream/sliders/spinner are all internally 300" );
    check( telemetry.controlled_100 == 0 && telemetry.controlled_50 == 0 && telemetry.intentional_miss == 0,
        name, 100.f, "all deliberate degradation is disabled" );
    check( telemetry.unsafe_at_100 == 0, name, 100.f, "dense/hold-aware schedule remained feasible" );
    bool releases_safe = true;
    for ( size_t i = 0; i < map.objects.size( ); ++i ) {
        relax::c_relax::scheduled_object_t scheduled{};
        releases_safe = releases_safe && timing.scheduled_object( i, scheduled );
        if ( ( map.objects[ i ].type & 2 ) != 0 )
            releases_safe = releases_safe && scheduled.release_time >= map.objects[ i ].end_time + 36;
    }
    check( releases_safe, name, 100.f, "slider holds use the conservative Perfect Play tail" );
}

int main( ) {
    constexpr float targets[] = { 100.f, 99.f, 97.f, 95.f, 90.f, 85.f, 80.f };
    struct p { const char* name; profile_t profile; } profiles[] = {
        { "circles", profile_t::circles }, { "stream", profile_t::stream },
        { "mixed", profile_t::mixed }, { "sparse", profile_t::sparse }
    };
    for ( const auto& profile : profiles )
        for ( int i = 0; i < 7; ++i )
            run_profile( profile.name, profile.profile, targets[ i ], 0x900du + i * 131u );
    run_perfect_object_mix( );

    std::printf( "Autobot accuracy controller: %d checks, %d failures\n", checks, failures );
    return failures == 0 ? 0 : 1;
}
