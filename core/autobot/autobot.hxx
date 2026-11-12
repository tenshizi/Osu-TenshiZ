#pragma once

// ============================================================================
//  autobot.hxx  --  Autobot controller (host adapter).
//
//  Architecture:  ONE MotionState, ONE MovementPlan, ONE output path.
//
//      snapshot  ->  shared Relax schedule  ->  motion_engine_t (pure planner)
//                ->  continuous playfield position  ->  project  ->  OS cursor
//
//  ALL movement mathematics and the entire planner live in motion_core.hxx
//  (pure, host-independent, unit-tested off-line).  This file is only the glue
//  to the live game:
//    * reads the OS cursor and projects it to playfield space,
//    * turns the shared Relax scheduler into a per-object schedule_view_t,
//    * drives motion_engine_t::update() once per frame,
//    * projects the engine's single position back to the screen and emits it,
//    * owns the two screen-space safeguards the pure engine cannot see:
//        - persistent external-cursor mismatch  -> engine.reanchor(),
//        - a single anomalous playfield geometry -> hold this frame.
//
//  The engine holds the one MotionState and the one MovementPlan; this adapter
//  never second-guesses where the cursor should be.  There is no output-side
//  target correction: the engine decides, the adapter projects and injects.
// ============================================================================

#include <core/autobot/motion_core.hxx>
#include <impl/struct/game_snapshot.hxx>
#include <impl/memory/input.hxx>
#include <impl/util/playfield.hxx>
#include <core/relax/relax.hxx>

#include <Windows.h>
#include <chrono>
#include <cmath>

namespace autobot {

    class c_autobot {
    public:
        // ---- settings (names preserved for config/UI compatibility) -------
        bool  enabled = false;
        bool  perfect_play = false;
        float aim_spread = 0.10f;
        float curve_strength = 0.36f;
        float drift_amount = 1.05f;      // retained for config compatibility
        float momentum = 0.82f;
        float slider_laziness = 0.08f;
        float spinner_rpm_min = 400.f, spinner_rpm_max = 440.f;
        bool  startup_motion = true;
        bool  break_motion = true;
        bool  energetic_dances = true;
        bool  gameplay_flow = true;
        float startup_energy = 0.82f;
        float break_energy = 0.68f;
        float target_accuracy = 98.5f;

        // ---- diagnostics accessors ----------------------------------------
        [[nodiscard]] diagnostics_t diagnostics( ) const { return m_engine.diagnostics( ); }
        [[nodiscard]] plan_type_t   plan_type( ) const { return m_engine.plan_type( ); }
        [[nodiscard]] uint64_t      record_count( ) const { return m_engine.record_count( ); }
        [[nodiscard]] bool last_record( flight_record_t& out ) const { return m_engine.last_record( out ); }
        [[nodiscard]] bool last_suspicious_record( flight_record_t& out ) const { return m_engine.last_suspicious_record( out ); }
        [[nodiscard]] bool last_discontinuity_record( flight_record_t& out ) const { return m_engine.last_discontinuity_record( out ); }

        void on_leave_play( const osu::game_snapshot_t& ) { reset_session( ); }

        // ==================================================================
        //  Per-frame entry point.
        // ==================================================================
        void update( const osu::game_snapshot_t& game, const osu::beatmap_data_t& map,
                     const relax::c_relax& timing, bool user_control = false ) {
            if ( !enabled || game.cur_state != osu::game_state_t::play || !map.loaded || map.objects.empty( ) ) {
                if ( m_in_play ) reset_session( );
                return;
            }
            const HWND hwnd = input::target_window( );
            RECT window{};
            if ( !hwnd || !playfield::get_playfield_rect( hwnd, window ) ) return;
            if ( ( window.right - window.left ) <= 1 || ( window.bottom - window.top ) <= 1 ) return;

            if ( user_control ) {
                m_paused = true;
                m_last_real = std::chrono::steady_clock::now( );
                return;
            }

            const bool new_session = !m_in_play || m_map_object_count != map.objects.size( ) ||
                                     game.cur_time < m_last_game_time - 200;
            m_hr_flip = ( game.cur_mod_state & 16 ) != 0 && !map.hr;

            if ( new_session ) { if ( !begin_session( game, map, window ) ) return; }
            else if ( m_paused ) reanchor_from_cursor( window, "resume" );
            m_in_play = true;
            m_paused = false;
            if ( !m_engine.initialized( ) ) return;

            // ---- time base ------------------------------------------------
            const auto now = std::chrono::steady_clock::now( );
            float real_dt = 1.f / 120.f;
            if ( m_last_real.time_since_epoch( ).count( ) != 0 )
                real_dt = std::chrono::duration<float>( now - m_last_real ).count( );
            m_last_real = now;
            const float dt = sanitize_controller_dt( real_dt );

            const int prev_game_time = m_last_game_time;
            if ( !new_session && prev_game_time >= 0 && game.cur_time > prev_game_time ) {
                const float rate = static_cast<float>( game.cur_time - prev_game_time ) / ( std::max( real_dt, 0.001f ) * 1000.f );
                if ( rate >= 0.5f && rate <= 2.f ) m_game_rate += ( rate - m_game_rate ) * 0.08f;
            }
            const int control_time = std::max( game.cur_time,
                static_cast<int>( std::floor( timing.prepared_game_time( ) ) ) );

            // ---- feed schedule + settings into the engine -----------------
            ingest_schedule( map, timing );
            m_engine.configure( current_settings( ) );

            // ---- screen-space external-cursor safeguard -------------------
            sync_external_cursor( window );

            // ---- run the pure engine (planner + evaluator + commit) -------
            m_engine.update( map, m_sched, control_time, dt, game.cur_time );

            // ---- project the single position and emit ---------------------
            emit_cursor( window, dt );

            // ---- diagnostics passthrough ----------------------------------
            const auto acc = timing.accuracy_telemetry( );
            m_engine.set_accuracy_diag( acc.target, acc.predicted, acc.debt, acc.grade_300,
                acc.grade_100, acc.grade_50,
                acc.controlled_100, acc.controlled_50, acc.intentional_miss,
                acc.unsafe_at_100, acc.objects, acc.perfect_play, acc.safe_300_margin_ms,
                acc.timing_clamp_count );
            m_last_game_time = game.cur_time;
        }

    private:
        motion_engine_t m_engine{};
        schedule_view_t m_sched{};

        bool   m_in_play = false, m_paused = false, m_hr_flip = false;
        size_t m_map_object_count = 0;
        int    m_last_game_time = 0;
        int    m_sched_through = -1;
        float  m_game_rate = 1.f;
        uint32_t m_seed = 0x9e3779b9u;

        std::chrono::steady_clock::time_point m_last_real{};

        POINT  m_last_output_screen{};
        bool   m_last_output_valid = false;
        point_t m_last_emit_pf{};
        float   m_last_emit_speed = 0.f, m_last_emit_accel = 0.f;
        POINT  m_last_observed_screen{};
        cursor_observation_guard_t m_cursor_guard{};

        settings_t current_settings( ) const {
            settings_t s;
            s.aim_spread = aim_spread; s.curve_strength = curve_strength; s.momentum = momentum;
            s.slider_laziness = slider_laziness;
            s.spinner_rpm_min = spinner_rpm_min; s.spinner_rpm_max = spinner_rpm_max;
            s.perfect_play = perfect_play;
            s.gameplay_flow = gameplay_flow; s.startup_motion = startup_motion; s.break_motion = break_motion;
            s.energetic_dances = energetic_dances; s.startup_energy = startup_energy; s.break_energy = break_energy;
            return s;
        }

        void reset_session( ) {
            m_in_play = false; m_paused = false;
            m_map_object_count = 0; m_last_game_time = 0; m_sched_through = -1;
            m_cursor_guard.reset( ); m_last_output_valid = false;
            m_sched = {};
            m_engine = motion_engine_t{};
        }

        bool begin_session( const osu::game_snapshot_t& game, const osu::beatmap_data_t& map, const RECT& window ) {
            POINT c{};
            float x = 0.f, y = 0.f;
            if ( !input::get_cursor_pos( &c ) || !playfield::screen_to_playfield( c.x, c.y, window, x, y ) )
                return false;                          // retry next frame

            m_map_object_count = map.objects.size( );
            m_game_rate = std::clamp( game.speed_mult, 0.5f, 2.f );
            const uint64_t entropy = static_cast<uint64_t>( std::chrono::steady_clock::now( ).time_since_epoch( ).count( ) );
            m_seed = mix_seed( static_cast<uint32_t>( entropy ) ^ static_cast<uint32_t>( entropy >> 32 ) ^
                static_cast<uint32_t>( game.map_id * 2654435761u ) ^ static_cast<uint32_t>( GetCurrentProcessId( ) ) );

            m_engine = motion_engine_t{};
            m_engine.configure( current_settings( ) );
            m_engine.begin_session( { x, y }, m_hr_flip, m_game_rate, m_map_object_count, m_seed );

            // seed the schedule with object start-times; real press times fill in
            m_sched.press_time.assign( m_map_object_count, 0 );
            m_sched.release_time.assign( m_map_object_count, 0 );
            m_sched.edge_bias.assign( m_map_object_count, 0.f );
            for ( size_t i = 0; i < m_map_object_count; ++i )
                m_sched.press_time[ i ] = map.objects[ i ].start_time;
            for ( size_t i = 0; i < m_map_object_count; ++i )
                m_sched.release_time[ i ] = map.objects[ i ].end_time;
            m_sched_through = -1;

            m_last_output_screen = c; m_last_output_valid = true;
            m_cursor_guard.reset( );
            m_cursor_guard.note_output( { static_cast<float>( c.x ), static_cast<float>( c.y ) } );
            m_last_emit_pf = { x, y };
            m_last_emit_speed = m_last_emit_accel = 0.f;
            m_last_real = std::chrono::steady_clock::now( );
            m_engine.set_projection_error( projection_error( window ) );
            return true;
        }

        void reanchor_from_cursor( const RECT& window, const char* reason ) {
            POINT c{};
            float x = 0.f, y = 0.f;
            if ( input::get_cursor_pos( &c ) && playfield::screen_to_playfield( c.x, c.y, window, x, y ) ) {
                m_engine.reanchor( { x, y }, reason );
                m_last_output_screen = c; m_last_output_valid = true;
                m_cursor_guard.reset( );
                m_cursor_guard.note_output( { static_cast<float>( c.x ), static_cast<float>( c.y ) } );
                m_last_emit_pf = { x, y };
                m_last_emit_speed = m_last_emit_accel = 0.f;
            }
        }

        static float projection_error( const RECT& window ) {
            static constexpr point_t s[] = { { 0,0 }, { 256,192 }, { 512,384 } };
            float e = 0.f;
            const int w = window.right - window.left, h = window.bottom - window.top;
            for ( auto p : s ) {
                const auto canon = playfield::playfield_to_screen( p.x, p.y, window );
                float lx = 0.f, ly = 0.f;
                playfield::project_osu_to_window( p.x, p.y, w, h, lx, ly );
                e = std::max( e, std::abs( static_cast<float>( canon.x ) - ( window.left + lx ) ) );
                e = std::max( e, std::abs( static_cast<float>( canon.y ) - ( window.top + ly ) ) );
            }
            return e;
        }

        void ingest_schedule( const osu::beatmap_data_t& map, const relax::c_relax& timing ) {
            for ( int i = m_sched_through + 1; i < static_cast<int>( map.objects.size( ) ); ++i ) {
                relax::c_relax::scheduled_object_t s{};
                if ( !timing.scheduled_object( static_cast<size_t>( i ), s ) ) break;
                if ( i < static_cast<int>( m_sched.press_time.size( ) ) ) {
                    m_sched.press_time[ static_cast<size_t>( i ) ] = s.press_time;
                    m_sched.release_time[ static_cast<size_t>( i ) ] = s.release_time;
                    m_sched.edge_bias[ static_cast<size_t>( i ) ] = s.accuracy_edge_bias;
                }
                m_sched_through = i;
            }
        }

        // A delayed GetCursorPos sample may equal any of several recent outputs,
        // not necessarily the newest one.  Only a stable physical position that
        // matches none of them for three observations is an external reanchor.
        void sync_external_cursor( const RECT& window ) {
            POINT actual{};
            if ( !input::get_cursor_pos( &actual ) ) return;
            m_last_observed_screen = actual;
            if ( !m_last_output_valid ) return;
            if ( !m_cursor_guard.observe( { static_cast<float>( actual.x ), static_cast<float>( actual.y ) } ) ) return;
            reanchor_from_cursor( window, "external-mismatch" );
        }

        // Project the engine's single position and inject it.  Guard against a
        // one-frame anomalous playfield rect turning a small internal move into
        // a huge screen jump: if the screen delta dwarfs the playfield delta,
        // hold this frame and record it (never emit the jump).
        void emit_cursor( const RECT& window, float dt ) {
            const point_t pf = m_engine.position( );
            const auto p = playfield::playfield_to_screen( pf.x, pf.y, window );
            float screen_disp = 0.f;
            bool output_discontinuity = false;

            if ( m_last_output_valid ) {
                const float scale = static_cast<float>( window.bottom - window.top ) * 0.8f / 384.f;
                const float pf_disp = length( pf - m_last_emit_pf );
                screen_disp = std::hypot( static_cast<float>( p.x - m_last_output_screen.x ),
                                          static_cast<float>( p.y - m_last_output_screen.y ) );
                if ( screen_disp > pf_disp * std::max( scale, 0.25f ) * 3.f + 120.f ) {
                    m_engine.record_external( pf, screen_disp, pf_disp * std::max( scale, 0.25f ), "geometry" );
                    return;                            // hold last output this frame
                }
                const auto& motion = m_engine.motion( );
                const float expected_pf = std::max( m_last_emit_speed, length( motion.velocity ) ) * dt +
                    0.5f * std::max( m_last_emit_accel, length( motion.acceleration ) ) * dt * dt + 2.f;
                const float expected_screen = expected_pf * std::max( scale, 0.25f ) + 2.f;
                output_discontinuity = screen_disp > 8.f && screen_disp > expected_screen + 1.f;
                if ( output_discontinuity )
                    m_engine.record_external( pf, screen_disp, expected_screen, "output-discontinuity" );
            }

            if ( input::move_absolute_virtual_desktop( p.x, p.y ) ) {
                const point_t previous_emit_pf = m_last_emit_pf;
                m_last_output_screen = { p.x, p.y };
                m_last_output_valid = true;
                m_cursor_guard.note_output( { static_cast<float>( p.x ), static_cast<float>( p.y ) } );
                m_last_emit_pf = pf;
                m_last_emit_speed = length( m_engine.motion( ).velocity );
                m_last_emit_accel = length( m_engine.motion( ).acceleration );
                m_engine.set_screen_diag( p.x, p.y, m_last_observed_screen.x, m_last_observed_screen.y );
                m_engine.set_output_diag( previous_emit_pf, pf, screen_disp, output_discontinuity );
            } else {
                m_engine.invalidate_plan( "injection-failed" ); // replan from current state next frame
            }
        }
    };

}
