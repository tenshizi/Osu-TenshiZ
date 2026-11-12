#include <core/relax/relax.hxx>
#include <core/relax/normal_relax.hxx>
#include <cstdio>

static int checks = 0, failures = 0;
static void check( bool ok, const char* message ) {
    ++checks;
    if ( !ok ) { ++failures; std::printf( "FAIL: %s\n", message ); }
}
static osu::hit_object_t object( int time, float x = 256.f, float y = 192.f ) {
    osu::hit_object_t o{};
    o.start_time = o.end_time = time; o.x = x; o.y = y; o.type = osu::circle;
    return o;
}
static osu::beatmap_data_t map_of( std::initializer_list<osu::hit_object_t> objects ) {
    osu::beatmap_data_t map{};
    map.loaded = true; map.cs = 4; map.od = 10; map.objects = objects;
    return map;
}
static relax::normal_relax_t::output_t tick( relax::normal_relax_t& engine,
    const osu::beatmap_data_t& map, int time, float x = 256.f, float y = 192.f, int mods = 0 ) {
    osu::game_snapshot_t game{};
    game.cur_time = time; game.cur_mod_state = mods;
    return engine.update( game, map, x, y );
}
int main( ) {
    {
        relax::normal_relax_t engine;
        auto map = map_of( { object( 1000 ) } );
        check( !tick( engine, map, 987 ).press, "no hit before 12ms allowance" );
        check( !tick( engine, map, 988, 400 ).press, "no blind press before aim arrives" );
        auto hit = tick( engine, map, 1020 );
        check( hit.press && hit.object_index == 0, "late cursor arrival can still hit" );
        check( hit.key_index == 1, "first press uses native right-button order" );
        check( !tick( engine, map, 1020 ).press, "object is not repeatedly pressed" );
        check( !tick( engine, map, 1070 ).release, "hold through 50ms" );
        check( tick( engine, map, 1071 ).release, "release after 50ms" );
    }
    for ( int interval : { 100, 75, 50, 20, 1 } ) {
        relax::normal_relax_t engine;
        auto map = map_of( {} );
        for ( int i = 0; i < 256; ++i ) map.objects.push_back( object( 1000 + i * interval, i % 2 ? 400.f : 100.f ) );
        int previous_key = -1;
        for ( int i = 0; i < 256; ++i ) {
            const auto& o = map.objects[ i ];
            auto hit = tick( engine, map, o.start_time - 12, o.x, o.y );
            check( hit.press && hit.object_index == i, "every stream note stays anchored to its own time" );
            check( hit.key_index != previous_key, "consecutive presses alternate" );
            if ( i ) check( hit.release, "new stream hit creates fresh release/press edge" );
            previous_key = hit.key_index;
        }
    }
    {
        relax::normal_relax_t engine;
        auto map = map_of( { object( 1000, 100 ), object( 1075, 300 ), object( 1150, 400 ) } );
        check( !tick( engine, map, 988, 500 ).press, "missed aim does not spend a click" );
        auto second = tick( engine, map, 1063, 300 );
        check( second.press && second.object_index == 1, "missed first note does not block next note" );
        auto third = tick( engine, map, 1138, 400 );
        check( third.press && third.object_index == 2, "missed first note does not shift stream" );
        check( !tick( engine, map, 1200, 100 ).press, "expired notes never trigger stale clicks" );
    }
    {
        auto slider = object( 1000, 100 );
        slider.type = osu::slider; slider.end_time = 1500;
        slider.slider_curve_str = "L|200:192"; slider.slider_length = 100; slider.slider_repeat = 1;
        auto map = map_of( { slider, object( 1200, 140 ) } );
        relax::normal_relax_t engine;
        check( tick( engine, map, 988, 100 ).press, "slider head press" );
        check( !tick( engine, map, 1100, 120 ).release, "follow area retains slider hold" );
        auto overlap = tick( engine, map, 1188, 140 );
        check( overlap.release && overlap.press && overlap.object_index == 1, "overlap creates new edge during slider hold" );
        check( !tick( engine, map, 1300, 160 ).release, "slider held after overlap" );
        check( tick( engine, map, 1350, 450 ).release, "leaving follow area releases hold" );
        check( !tick( engine, map, 1400, 230 ).press, "reacquire requires small radius" );
        check( tick( engine, map, 1401, 180 ).press, "reacquire inside ball" );
        check( !tick( engine, map, 1450, 240 ).release, "tracking expands follow area" );
        check( tick( engine, map, 1501, 200 ).release, "slider releases after end" );
    }
    {
        relax::normal_relax_t engine;
        auto spinner = object( 1000 ); spinner.type = osu::spinner; spinner.end_time = 2000;
        auto map = map_of( { spinner } );
        check( tick( engine, map, 988, 0, 0 ).press, "spinner holds independent of cursor" );
        check( !tick( engine, map, 1999 ).release, "spinner holds until end" );
        check( tick( engine, map, 2001 ).release, "spinner releases after end" );
    }
    {
        relax::normal_relax_t engine;
        auto o = object( 1000, 100, 80 ); o.stack_index = 2;
        auto map = map_of( { o } );
        check( !tick( engine, map, 988, 88, 68, 16 ).press, "Hard Rock flips hit area" );
        check( tick( engine, map, 988, 88, 292, 16 ).press, "stacked Hard Rock object hittable" );
        auto rewind = tick( engine, map, 900, 88, 292, 16 );
        check( rewind.release && !rewind.press, "rewind releases and resets heads" );
        check( tick( engine, map, 988, 88, 292, 16 ).press, "retry can hit object again" );
        engine.suspend( );
        check( !engine.down( ), "suspension clears held state" );
        check( !tick( engine, map, 988, 88, 292, 16 ).press, "resume does not repeat consumed head" );
    }
    {
        relax::normal_relax_t engine;
        auto map = map_of( { object( 1000 ) } );
        check( tick( engine, map, 1099 ).press, "hit inside OD10 late window" );
        engine.reset( );
        check( !tick( engine, map, 1100 ).press, "outside OD10 window rejected" );
    }
    {
        relax::normal_relax_t engine;
        auto map = map_of( { object( 1000 ), object( 1001 ), object( 1002 ) } );
        check( tick( engine, map, 1000 ).press, "backlog first head can press" );
        check( !tick( engine, map, 1000 ).press, "unchanged clock cannot dump multiple heads" );
        check( tick( engine, map, 1001 ).object_index == 1, "next clock sample processes next head" );
    }
    {
        // Exercise the production router without sending input: every note is
        // in the future, and Normal has no target window in this test process.
        input::set_target_window( nullptr );
        relax::c_relax controller;
        osu::game_snapshot_t game{};
        game.cur_state = osu::game_state_t::play;
        auto map = map_of( { object( 1000 ), object( 1100 ) } );
        controller.update( game, map );
        check( controller.queue_size( ) == 2, "Normal restores the beatmap-anchored scheduler" );
        controller.relax_mode = 1;
        controller.update( game, map );
        check( controller.queue_size( ) == 2, "Smart retains existing scheduler" );
        check( controller.smart_owns_input( ), "Smart retains physical-input ownership" );
        controller.relax_mode = 0;
        controller.update( game, map );
        check( controller.queue_size( ) == 2 && !controller.smart_owns_input( ), "switching to Normal rebuilds queue and clears Smart ownership" );
        controller.prepare_for_autobot( game, map, 100.f, true );
        check( controller.queue_size( ) == 2, "Autobot retains existing shared schedule" );
        controller.on_leave_play( game );
        check( !controller.is_synced( ) && controller.queue_size( ) == 0, "leaving play clears session" );
    }
    {
        auto slider = object( 1000, 100 );
        slider.type = osu::slider; slider.end_time = 9000;
        slider.slider_curve_str = "L|400:192";
        slider.slider_length = 300; slider.slider_repeat = 4;
        auto map = map_of( { slider } );
        relax::normal_relax_t engine;
        check( tick( engine, map, 988, 100 ).press, "long repeating slider acquires head" );
        // Four two-second spans, including every reversal. No hold timeout.
        for ( int t = 1000; t <= 9000; t += 10 ) {
            const int elapsed = t - 1000;
            const int span = std::min( elapsed / 2000, 3 );
            const float progress = ( elapsed - span * 2000 ) / 2000.f;
            const float x = 100.f + 300.f * ( span % 2 ? 1.f - progress : progress );
            const auto action = tick( engine, map, t, x );
            check( !action.release && !action.press && engine.down( ), "hold survives long spans and reverse arrows" );
        }
        check( tick( engine, map, 9001, 100 ).release, "long slider releases after its tail" );
    }
    {
        auto slider = object( 1000, 100 );
        slider.type = osu::slider; slider.end_time = 12000;
        auto map = map_of( { slider, object( 5000, 400 ) } );
        relax::normal_relax_t engine;
        check( !tick( engine, map, 988, 400 ).press, "unreadable path does not acquire an unaimed head" );
        check( tick( engine, map, 1000, 100 ).press, "unreadable slider can acquire its head" );
        check( !tick( engine, map, 3000, 400, 40 ).release, "missing geometry does not pin follow area to head" );
        const auto overlap = tick( engine, map, 4988, 400 );
        check( overlap.release && overlap.press && overlap.object_index == 1, "fallback hold allows overlapping head" );
        check( !tick( engine, map, 11000, 50, 350 ).release, "fallback retains long slider to known end" );
        check( tick( engine, map, 12001 ).release, "fallback hold releases at tail" );
    }
    std::printf( "Retired cursor experiment and mode routing: %d checks, %d failures\n", checks, failures );
    return failures ? 1 : 0;
}
