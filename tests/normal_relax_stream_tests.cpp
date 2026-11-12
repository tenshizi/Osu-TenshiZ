#define TENSHIZ_RELAX_TESTING
#include <core/relax/relax.hxx>
#include <cstdio>

int main( ) {
    input::set_target_window( nullptr );
    osu::game_snapshot_t game{};
    game.cur_state = osu::game_state_t::play; game.left_key = 'Z'; game.right_key = 'X';
    osu::beatmap_data_t map{};
    map.loaded = true; map.cs = 4; map.od = 10;
    for ( int i = 0; i < 2000; ++i ) {
        osu::hit_object_t o{};
        o.type = osu::circle; o.start_time = o.end_time = 1000 + i * 75;
        map.objects.push_back( o );
    }
    relax::c_relax r;
    r.seed_for_tests( 0xaca7 ); r.early_variation_ms = -25; r.late_variation_ms = 25;
    r.timing_drift_ms = 0;
    r.set_input_sink_for_tests( []( WORD, bool ) {} );
    r.update( game, map );
    std::vector<double> offsets;
    int failures = 0;
    for ( size_t i = 0; i < map.objects.size( ); ++i ) {
        relax::c_relax::scheduled_object_t s{};
        if ( !r.scheduled_object( i, s ) ) { ++failures; continue; }
        offsets.push_back( s.press_time - map.objects[i].start_time );
    }
    double mean = 0;
    for ( double x : offsets ) mean += x;
    mean /= offsets.size( );
    double cov = 0, var = 0;
    for ( size_t i = 1; i < offsets.size( ); ++i ) {
        cov += ( offsets[i-1] - mean ) * ( offsets[i] - mean );
        var += ( offsets[i-1] - mean ) * ( offsets[i-1] - mean );
    }
    if ( var && std::abs( cov / var ) >= .12 ) ++failures;
    relax::c_relax recovered;
    recovered.seed_for_tests( 17 ); recovered.hold_floor = 14; recovered.hold_ceiling = 14;
    recovered.k1_hold_center = recovered.k2_hold_center = 14;
    recovered.k1_hold_spread = recovered.k2_hold_spread = 0;
    int emitted_time = 0, presses = 0;
    std::vector<int> emitted;
    recovered.set_input_sink_for_tests( [&]( WORD, bool down ) {
        if ( down ) { ++presses; emitted.push_back( emitted_time ); }
    } );
    recovered.update( game, map );
    relax::c_relax::scheduled_object_t first{}, last{};
    if ( !recovered.scheduled_object( 0, first ) || !recovered.scheduled_object( map.objects.size()-1, last ) ) return 2;
    // Delay the first dispatched tap; later objects keep their own targets.
    for ( emitted_time = first.release_time + 1; emitted_time <= last.release_time + 1; ++emitted_time )
        recovered.flush_normal_at_for_tests( game, emitted_time );
    if ( presses != 2000 ) ++failures;
    for ( size_t i = 1; i < map.objects.size( ) && i < emitted.size( ); ++i ) {
        relax::c_relax::scheduled_object_t s{};
        if ( !recovered.scheduled_object( i, s ) || emitted[i] != s.press_time ) ++failures;
    }
    std::printf( "Normal stream independence and delayed-first-note recovery: %d failures\n", failures );
    return failures ? 1 : 0;
}
