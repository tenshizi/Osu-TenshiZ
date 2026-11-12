#ifndef NOMINMAX
#define NOMINMAX
#endif
#define TENSHIZ_LAZER_READER_TESTING
#include <core/beatmap/lazer_ruleset_reader.hxx>
#include <core/autobot/motion_core.hxx>
#include <array>
#include <cstdio>
#include <cstring>

static int checks = 0, failures = 0;
static void check( bool ok, const char* message ) {
    ++checks;
    if ( !ok ) { ++failures; std::printf( "FAIL: %s\n", message ); }
}
template<class T, class Buffer> static void put( Buffer& b, size_t offset, T value ) {
    std::memcpy( b.data( ) + offset, &value, sizeof value );
}
int main( ) {
    memory::c_process process;
    process.enable_fallback( );
    if ( !process.attach( GetCurrentProcessId( ) ) ) return 2;
    offsets::lazer::table_t off{};
    for ( const int points : { 2, 500, 501, 2048 } ) {
        std::array<uint8_t, 0x200> object{}, path{}, list{};
        std::vector<uint8_t> array( off.array_first_element + points * 8 );
        put( object, off.slider_path_wrapper, reinterpret_cast<uint64_t>( path.data( ) ) );
        put( object, off.slider_repeat_count, int32_t{ 3 } );
        put( path, off.path_ctrl_points_list, reinterpret_cast<uint64_t>( list.data( ) ) );
        put( list, off.list_items, reinterpret_cast<uint64_t>( array.data( ) ) );
        put( list, off.list_size, points );
        for ( int i = 0; i < points; ++i ) {
            const float f = static_cast<float>( i ) / ( points - 1 );
            put( array, off.array_first_element + i * 8, 300.f * f );
            put( array, off.array_first_element + i * 8 + 4, 50.f * std::sin( f * 3.14159265f ) );
        }
        osu::hit_object_t o{};
        o.x = 100; o.y = 100; o.type = osu::slider; o.start_time = 1000; o.end_time = 9000;
        beatmap::c_lazer_ruleset_reader::read_slider_path_for_tests(
            process, off, reinterpret_cast<uint64_t>( object.data( ) ), o, false );
        check( !o.slider_curve_str.empty( ), "calculated paths above 500 points are retained" );
        check( o.slider_curve_str.front( ) == 'L', "sampled path stays a polyline" );
        check( o.slider_repeat == 4, "three lazer repeats decode as four spans" );
        const auto decoded = autobot::build_slider_path( o, false );
        check( !decoded.empty( ), "decoded path is usable by Relax" );
        const auto reverse = autobot::eval_slider_ball( decoded, o, 3000 );
        const auto tail = autobot::eval_slider_ball( decoded, o, 9000 );
        check( std::abs( reverse.x - 400.f ) < .1f, "first reverse reaches far endpoint" );
        check( std::abs( tail.x - 100.f ) < .1f, "even spans return to starting endpoint" );
    }
    process.detach( );
    std::printf( "Lazer slider reader: %d checks, %d failures\n", checks, failures );
    return failures ? 1 : 0;
}
