// Read-only diagnostic: never emits input or writes to the game process.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <core/game/osu_lazer.hxx>
#include <impl/util/playfield.hxx>
#include <core/beatmap/lazer_ruleset_reader.hxx>
#include <cstdio>

int main( int argc, char** ) {
    memory::c_process process;
    process.enable_fallback( );
    const int pid = memory::c_process::find_pid_by_name( L"osu!.exe" );
    if ( !process.attach( pid ) ) { std::printf( "Cannot read osu pid=%d error=%lu\n", pid, GetLastError( ) ); return 1; }
    const auto hwnd = playfield::find_osu_window( pid );
    input::init( );
    POINT public_cursor{}, helper_cursor{};
    const bool public_ok = GetCursorPos( &public_cursor );
    const bool helper_ok = input::get_cursor_pos( &helper_cursor );
    std::printf( "cursor API public=%d,%ld,%ld helper=%d,%ld,%ld nt=%d\n", public_ok,
        public_cursor.x, public_cursor.y, helper_ok, helper_cursor.x, helper_cursor.y, input::using_nt_input( ) );
    EnumWindows( []( HWND window, LPARAM arg ) -> BOOL {
        DWORD owner = 0; GetWindowThreadProcessId( window, &owner );
        if ( owner != static_cast<DWORD>( arg ) ) return TRUE;
        wchar_t title[512]{}, cls[256]{}; RECT area{};
        GetWindowTextW( window, title, 512 ); GetClassNameW( window, cls, 256 );
        const BOOL client_ok = GetClientRect( window, &area );
        RECT bounds{}; GetWindowRect( window, &bounds );
        std::printf( "candidate=%p visible=%d client_ok=%d area=%ld,%ld class=%ls title=%ls\n",
            window, IsWindowVisible( window ), client_ok, area.right, area.bottom, cls, title );
        std::printf( "iconic=%d bounds=%ld,%ld,%ld,%ld\n", IsIconic( window ), bounds.left, bounds.top, bounds.right, bounds.bottom );
        return TRUE;
    }, static_cast<LPARAM>( pid ) );
    RECT rect{};
    const bool geometry = playfield::get_playfield_rect( hwnd, rect );
    std::printf( "pid=%d window=%p foreground=%p geometry=%d rect=%ld,%ld,%ld,%ld\n",
        pid, hwnd, GetForegroundWindow( ), geometry, rect.left, rect.top, rect.right, rect.bottom );
    std::fflush( stdout );
    offsets::lazer::table_t offsets;
    game::c_osu_lazer client( offsets );
    if ( !client.attach( process ) ) { std::puts( "Cannot resolve game root" ); return 2; }
    beatmap::c_lazer_ruleset_reader reader;
    osu::beatmap_data_t map{};
    uint64_t map_drawable = 0;
    for ( int i = 0; i < ( argc > 1 ? 1 : 16 ); ++i ) {
        osu::game_snapshot_t snap{};
        client.update( process, snap );
        input::set_target_window( playfield::find_osu_window( pid ) );
        if ( snap.drawable_ruleset && map_drawable != snap.drawable_ruleset ) {
            if ( reader.try_load( process, snap, offsets, map ) ) {
                map_drawable = snap.drawable_ruleset;
                std::printf( "decoded map: count=%zu cs=%.2f od=%.2f first=%d last=%d\n", map.objects.size( ),
                    map.cs, map.od, map.objects.front( ).start_time, map.objects.back( ).start_time );
            }
        }
        POINT cursor{}; RECT screen{}; float x = 0, y = 0;
        if ( GetCursorPos( &cursor ) && playfield::get_playfield_rect( input::target_window( ), screen ) &&
             playfield::screen_to_playfield( cursor.x, cursor.y, screen, x, y ) ) {
            std::printf( "cursor=%.1f,%.1f ", x, y );
            for ( const auto& o : map.objects ) if ( o.start_time >= snap.cur_time ) {
                std::printf( "next=%d position=%.1f,%.1f type=%u\n", o.start_time, o.x, o.y, o.type ); break;
            }
        }
        const auto global_clock = process.read<uint64_t>( snap.game_base + offsets.game_base_beatmap_clock );
        const auto final_source = process.read<uint64_t>( global_clock + offsets.framed_clock_final_source );
        const auto player_clock = process.read<uint64_t>( snap.player_screen + 0x468 );
        const auto gameplay_clock = process.read<uint64_t>( player_clock + 0x340 );
        const auto gameplay_source = process.read<uint64_t>( gameplay_clock + offsets.framed_clock_final_source );
        RECT live_rect{};
        const auto live_window = playfield::find_osu_window( pid );
        std::printf( "window=%p foreground=%d minimized=%d geometry=%d gameplay_time=%.2f\n", live_window,
            live_window && live_window == GetForegroundWindow( ), live_window && IsIconic( live_window ),
            playfield::get_playfield_rect( live_window, live_rect ),
            process.read<double>( gameplay_source + offsets.framed_clock_current_time ) );
        std::printf( "time=%d state=%d map=%d root=%llx player=%llx drawable=%llx global=%llx final=%llx final_time=%.2f player_clock=%llx\n",
            snap.cur_time, (int)snap.cur_state, snap.map_id, snap.game_base, snap.player_screen,
            snap.drawable_ruleset, global_clock, final_source,
            process.read<double>( final_source + offsets.framed_clock_current_time ), player_clock );
        if ( i == 0 ) {
            for ( unsigned off = 8; off <= 0x70; off += 8 )
                std::printf( "final+%x raw=%llx double=%.2f\n", off,
                    process.read<uint64_t>( final_source + off ), process.read<double>( final_source + off ) );
        }
        std::fflush( stdout );
        Sleep( 1000 );
    }
    process.detach( );
}
