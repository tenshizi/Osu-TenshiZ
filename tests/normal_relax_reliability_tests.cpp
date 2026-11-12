#define TENSHIZ_RELAX_TESTING
#include <core/relax/relax.hxx>
#include <cstdio>
#include <map>

static int checks = 0, failures = 0;
static void check( bool ok, const char* message ) {
    ++checks;
    if ( !ok ) { ++failures; if ( failures < 12 ) std::printf( "FAIL: %s\n", message ); }
}
static osu::hit_object_t object( int start, int end, uint8_t type ) {
    osu::hit_object_t o{};
    o.start_time = start; o.end_time = end; o.type = type;
    o.x = 256; o.y = 192;
    return o;
}
static osu::game_snapshot_t game( ) {
    osu::game_snapshot_t g{};
    g.cur_state = osu::game_state_t::play; g.left_key = 'Z'; g.right_key = 'X';
    return g;
}
static osu::beatmap_data_t map( ) {
    osu::beatmap_data_t m{}; m.loaded = true; m.cs = 4; m.od = 10;
    return m;
}
int main( ) {
    input::set_target_window( nullptr );
    for ( const int interval : { 150, 100, 75, 50, 20, 1 } ) {
        auto m = map( ); auto g = game( );
        for ( int i = 0; i < 256; ++i ) m.objects.push_back( object( 1000 + i * interval, 1000 + i * interval, osu::circle ) );
        relax::c_relax r;
        r.seed_for_tests( 73u + interval ); r.manual_offset_ms = 3;
        int time = 0, presses = 0;
        std::map<int, int> emitted;
        bool z = false, x = false;
        r.set_input_sink_for_tests( [&]( WORD key, bool down ) {
            bool& held = key == 'Z' ? z : x;
            check( held != down, "each key event is a real state change" );
            held = down;
            if ( down ) { ++presses; ++emitted[ time ]; }
        } );
        r.update( g, m );
        int previous = -9999, end = 0;
        std::vector<int> targets;
        for ( size_t i = 0; i < m.objects.size( ); ++i ) {
            relax::c_relax::scheduled_object_t s{};
            check( r.scheduled_object( i, s ), "each burst/stream object is scheduled" );
            const int offset = s.press_time - m.objects[i].start_time;
            check( offset >= -11 && offset <= 17, "object-local configured timing range is respected" );
            check( s.press_time > previous, "burst targets are strictly ordered" );
            previous = s.press_time; end = std::max( end, s.release_time );
            targets.push_back( s.press_time );
        }
        for ( time = 900; time <= end + 1; ++time ) r.flush_normal_at_for_tests( g, time );
        check( presses == 256, "every burst/stream note emits exactly one press" );
        for ( int target : targets ) check( emitted[ target ] == 1, "press fires at its own scheduled timestamp" );
        check( !z && !x && r.queue_size( ) == 0, "stream ends with both keys released" );
    }
    for ( int duration : { 20, 80, 119, 120, 500, 3000, 12000 } ) {
        auto m = map( ); auto g = game( );
        m.objects.push_back( object( 1000, 1000 + duration, osu::slider ) );
        relax::c_relax r; r.seed_for_tests( duration );
        bool held[2]{}; int time = 0, presses = 0;
        r.set_input_sink_for_tests( [&]( WORD key, bool down ) {
            held[ key == 'X' ] = down; if ( down ) ++presses;
        } );
        r.update( g, m );
        relax::c_relax::scheduled_object_t s{};
        check( r.scheduled_object( 0, s ), "slider head is scheduled" );
        check( s.release_time >= 1000 + duration + 24, "all slider lengths protect their tail" );
        for ( time = 900; time <= s.release_time + 1; ++time ) {
            r.flush_normal_at_for_tests( g, time );
            if ( time >= s.press_time && time <= 1000 + duration )
                check( held[0] || held[1], "short/large/long slider remains continuously held" );
        }
        check( presses == 1 && !held[0] && !held[1], "slider ends with one head and no stuck key" );
    }
    {
        auto m = map( ); auto g = game( );
        m.objects.push_back( object( 1000, 9000, osu::slider ) );
        for ( int t = 1200; t <= 1800; t += 75 ) m.objects.push_back( object( t, t, osu::circle ) );
        relax::c_relax r; r.seed_for_tests( 19 );
        bool held[2]{}; int time = 0, presses = 0;
        r.set_input_sink_for_tests( [&]( WORD key, bool down ) {
            held[ key == 'X' ] = down; if ( down ) ++presses;
        } );
        r.update( g, m );
        for ( time = 900; time <= 9200; ++time ) {
            r.flush_normal_at_for_tests( g, time );
            if ( time >= 1025 && time <= 9000 )
                check( held[0] || held[1], "ordinary burst leaves protected slider key held" );
        }
        check( presses == static_cast<int>( m.objects.size( ) ), "ordinary burst heads emit alongside slider" );
        check( !held[0] && !held[1], "mixed session releases both keys" );
    }
    {
        auto m = map( ); auto g = game( );
        m.objects.push_back( object( 1000, 9000, osu::slider ) );
        relax::c_relax r; bool held[2]{};
        r.set_input_sink_for_tests( [&]( WORD key, bool down ) { held[key == 'X'] = down; } );
        r.update( g, m );
        r.flush_normal_at_for_tests( g, 1100 );
        check( held[0] || held[1], "slider is held before pause" );
        r.release_for_tests( g );
        check( !held[0] && !held[1], "pause/focus loss releases both keys" );
        r.on_leave_play( g );
        check( r.queue_size( ) == 0, "leaving play clears original dispatch queue" );
    }
    std::printf( "Normal Relax emitted input: %d checks, %d failures\n", checks, failures );
    return failures ? 1 : 0;
}
