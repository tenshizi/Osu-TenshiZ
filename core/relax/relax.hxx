#pragma once

#include <impl/struct/game_snapshot.hxx>
#include <impl/memory/input.hxx>
#include <core/relax/nt_input.hxx>
#include <core/relax/density_tolerance.hxx>
#include <Windows.h>
#include <vector>
#include <algorithm>
#include <random>
#include <cmath>
#include <chrono>
#include <mutex>
#include <functional>

namespace relax {

    enum class tap_style_t : int {
        alternate = 0,
        singletap = 1
    };

    enum class tap_style_state_t : int {
        singletap,
        burst,
        alternate,
        recovery
    };

    // Normal schedules each tap from its absolute beatmap timestamp.
    // Smart only assists the player's own physical taps.
    enum class relax_mode_t : int {
        normal = 0,
        smart = 1,
        hybrid = 2
    };

    class c_relax {
    public:
        struct scheduled_object_t {
            bool valid = false;
            int press_time = 0;
            int release_time = 0;
            int timing_variation_ms = 0;
            int accuracy_timing_ms = 0;
            int simulated_grade = 300;
            float accuracy_edge_bias = 0.f;
            int key_index = 0;
            tap_style_state_t tap_state = tap_style_state_t::singletap;
        };

        // Read-only snapshot of the Target Accuracy controller for diagnostics.
        struct accuracy_telemetry_t {
            bool active = false;
            bool perfect_play = false;
            float target = 100.f;
            float predicted = 100.f;      // projected final osu! accuracy (%)
            uint64_t objects = 0;         // objects scheduled so far
            uint64_t grade_300 = 0;
            uint64_t grade_100 = 0;
            uint64_t grade_50 = 0;
            uint64_t controlled_100 = 0;
            uint64_t controlled_50 = 0;
            uint64_t intentional_miss = 0;
            uint64_t unsafe_at_100 = 0;
            uint64_t timing_clamp_count = 0;
            uint64_t dense_groups = 0;
            int dense_lookahead = 5;
            float safe_300_margin_ms = 0.f;
            float debt = 0.f;             // outstanding normalized score loss
        };

        // Minimal Smart Relax verification counters.
        struct smart_telemetry_t {
            bool active = false;
            uint32_t physical = 0;          // real press passed through untouched
            uint32_t corrected = 0;         // early press swallowed and rescheduled
            uint32_t fallback = 0;          // Miss Rescue hit the object for the player
            uint32_t suppressed_late = 0;   // late press swallowed after a fallback
            int last_target_offset_ms = 0;  // last injected hit offset vs object time
        };
        struct live_telemetry_t {
            bool active = false;
            uint64_t presses = 0, confirmed_hits = 0, confirmed_misses = 0, send_failures = 0;
            uint64_t great = 0, ok = 0, meh = 0;
            uint64_t result_offset_samples = 0;
            uint64_t density_100_presses = 0;
            int visible_heads = 0;
            float density_chance = 0.f;
            int last_offset = 0;
            double last_result_offset = 0;
        };
        live_telemetry_t live_telemetry( ) const {
            std::lock_guard lock( m_mtx );
            auto out = m_live_stats;
            out.active = m_live_session && live_hit_feedback;
            out.density_chance = out.active ? density_100_chance(out.visible_heads,density_tolerance) : 0.f;
            return out;
        }

        bool enabled = true;

        float ur = 70.f;

        int tap_style = static_cast<int>( tap_style_t::singletap );
        int primary_key = 0;
        int singletap_speed_bpm = 390;
        int burst_tolerance = 100;
        int density_tolerance = 100;
        float stamina = 90.f;

        float k1_hold_center = 46.f;
        float k1_hold_spread = 10.f;

        float k2_hold_center = 46.f;
        float k2_hold_spread = 10.f;

        float hold_floor = 14.f;
        float hold_ceiling = 110.f;

        int32_t manual_offset_ms = 0;

        bool timing_variation = true;
        bool live_hit_feedback = true;
        int32_t early_variation_ms = -14;
        int32_t late_variation_ms = 14;
        int32_t timing_drift_ms = 3;

        // ---- Smart Relax -------------------------------------------------
        int relax_mode = static_cast<int>( relax_mode_t::normal );
        int hybrid_delay_ms = 0;
        bool smart_early_correction = true;
        int32_t smart_early_capture_ms = 120;
        bool smart_miss_rescue = true;
        // Latest safe fallback target, measured from the object time.  It is
        // clamped into the configured Relax late variation bound, so Smart
        // Relax can never introduce a timing path outside that range.
        int32_t smart_rescue_deadline_ms = 10;

        // True while Smart Relax owns the physical hit keys.  Tap Assist (and
        // anything else hooking Z/X) must stand down while this is set.
        [[nodiscard]] bool smart_owns_input( ) const {
            std::lock_guard lock( m_mtx );
            return (m_smart_active || relax_mode == 2) && m_in_play;
        }

        // Called from the low-level keyboard hook.  Returns true when the
        // physical event must be swallowed.
        bool handle_key_event( int vk, bool is_down, int64_t press_qpc = 0 ) {
            std::lock_guard lock( m_mtx );
            if ( (!m_smart_active && relax_mode != 2) || !m_in_play )
                return false;

            int slot = -1;
            if ( vk == static_cast<int>( m_smart_k1 ) ) slot = 0;
            else if ( vk == static_cast<int>( m_smart_k2 ) ) slot = 1;
            else return false;

            if (relax_mode == 2) {
                if (is_down && m_hybrid_down[slot]) return false;
                m_hybrid_down[slot] = is_down;
                m_hybrid_manual = true;
                m_hybrid_last_input = GetTickCount64();
                // Release only helper-owned holds before forwarding the real key.
                live_key(m_smart_k1, false, m_left_down);
                live_key(m_smart_k2, false, m_right_down);
                for (auto& click : m_click_queue)
                    if (click.pressed) click.released = true;
                return false;
            }

            if ( !is_down ) {
                m_smart_phys_down[ slot ] = false;
                if ( m_smart_suppress_up[ slot ] ) {
                    m_smart_suppress_up[ slot ] = false;
                    return true;
                }
                return false;
            }

            // Key repeat: mirror whatever we decided for the original press.
            if ( m_smart_phys_down[ slot ] )
                return m_smart_suppress_up[ slot ];
            m_smart_phys_down[ slot ] = true;

            const double manual_time = smart_game_time( press_qpc );
            m_rescue_credit = true;
            m_last_manual_game_time = manual_time;
            m_last_manual_wall_time = GetTickCount64();
            return smart_match_press( slot, manual_time );
        }

        [[nodiscard]] size_t queue_size( ) const {
            std::lock_guard lock( m_mtx );
            return m_click_queue.size( );
        }
        [[nodiscard]] bool is_synced( ) const { std::lock_guard lock( m_mtx ); return m_in_play; }
        [[nodiscard]] int last_hit_obj_idx( ) const {
            std::lock_guard lock( m_mtx );
            return m_last_hit_obj_idx;
        }

        [[nodiscard]] bool is_active( ) const {
            std::lock_guard lock( m_mtx );
            return enabled && m_in_play && !m_click_queue.empty( );
        }

        [[nodiscard]] bool scheduled_object(
            size_t object_index, scheduled_object_t& out ) const {
            std::lock_guard lock( m_mtx );
            if ( object_index >= m_scheduled_objects.size( ) ||
                 !m_scheduled_objects[ object_index ].valid )
                return false;
            out = m_scheduled_objects[ object_index ];
            return true;
        }

        [[nodiscard]] double prepared_game_time( ) const {
            std::lock_guard lock( m_mtx );
            return m_prepared_game_time;
        }

        [[nodiscard]] accuracy_telemetry_t accuracy_telemetry( ) const {
            std::lock_guard lock( m_mtx );
            accuracy_telemetry_t t{};
            t.active = m_autobot_accuracy_mode && m_in_play;
            t.perfect_play = m_autobot_perfect_play;
            t.target = m_autobot_perfect_play ? 100.f : m_autobot_target_accuracy;
            t.objects = m_acc_objects;
            t.grade_300 = m_acc_grade_300;
            t.grade_100 = m_acc_grade_100;
            t.grade_50 = m_acc_grade_50;
            t.controlled_100 = m_acc_controlled_100;
            t.controlled_50 = m_acc_controlled_50;
            t.intentional_miss = 0;
            t.unsafe_at_100 = m_acc_unsafe_at_100;
            t.timing_clamp_count = m_acc_timing_clamps;
            t.dense_groups = m_acc_dense_groups;
            t.safe_300_margin_ms = m_acc_safe_300_margin_ms;
            t.predicted = m_acc_objects
                ? static_cast<float>( ( 1.0 - m_acc_loss / static_cast<double>( m_acc_objects ) ) * 100.0 )
                : 100.f;
            const double f = std::clamp( m_autobot_target_accuracy, 80.f, 99.99f ) / 100.0;
            const double deficit = ( 1.0 - f ) * static_cast<double>( m_acc_objects ) - m_acc_loss;
            t.debt = m_autobot_perfect_play ? 0.f : static_cast<float>( deficit );
            return t;
        }

        [[nodiscard]] smart_telemetry_t smart_telemetry( ) const {
            std::lock_guard lock( m_mtx );
            smart_telemetry_t t{};
            t.active = m_smart_active && m_in_play;
            t.physical = m_smart_physical;
            t.corrected = m_smart_corrected;
            t.fallback = m_smart_fallback;
            t.suppressed_late = m_smart_suppressed_late;
            t.last_target_offset_ms = m_smart_last_target_offset;
            return t;
        }

#ifdef TENSHIZ_RELAX_TESTING
        void set_input_sink_for_tests( std::function<void( WORD, bool )> sink ) {
            m_test_input_sink = std::move( sink );
        }
        void flush_normal_at_for_tests( const osu::game_snapshot_t& game, double time ) {
            flush_queue_at( game, time );
        }
        void release_for_tests( const osu::game_snapshot_t& game ) { release_all_keys( game ); }
        void seed_for_tests( uint32_t seed ) {
            std::lock_guard lock( m_mtx );
            m_rng.seed( seed );
        }
#endif

        void on_leave_play( const osu::game_snapshot_t& game ) {
            std::lock_guard lock( m_mtx );
            leave_play( game );
        }

    private:
        void leave_play( const osu::game_snapshot_t& game ) {
            release_all_keys( game );
            m_click_queue.clear( );
            m_scheduled_objects.clear( );
            m_last_hit_obj_idx = -1;
            m_scheduled_through_idx = -1;
            m_last_click_time = -99999;
            m_in_play = false;
            reset_tap_controller( );
            m_last_audio_time = 0;
            m_last_audio_sync_time = 0.0;
            m_prepared_game_time = 0.0;
            m_last_jitter = 0.f;
            reset_timing_drift( );
            reset_accuracy_controller( );
            reset_smart_state( );
            m_live_session = false;
            m_live_last_dispatch = INT32_MIN;
            m_live_good_wall = 0;
            m_live_good_time = 0;
            m_live_stats = {};
        }

    public:

        void update( const osu::game_snapshot_t& game, const osu::beatmap_data_t& map ) {
            std::lock_guard lock( m_mtx );
            m_autobot_accuracy_mode = false;
            m_autobot_perfect_play = false;
            update_locked( game, map, false, true );
        }

        // Autobot shares this exact scheduler and key controller.  Preparation is
        // separated from the one output flush so movement can be emitted first at
        // the final scheduled timestamp.  Standalone Relax still uses update().
        void prepare_for_autobot( const osu::game_snapshot_t& game, const osu::beatmap_data_t& map,
                                  float target_accuracy = 99.99f, bool perfect_play = false ) {
            std::lock_guard lock( m_mtx );
            m_autobot_accuracy_mode = true;
            m_autobot_perfect_play = perfect_play;
            m_autobot_target_accuracy = std::clamp( target_accuracy, 80.f, 99.99f );
            update_locked( game, map, true, false );
            if ( m_in_play )
                m_prepared_game_time = get_interpolated_game_time( game );
        }

        void flush_for_autobot( const osu::game_snapshot_t& game ) {
            std::lock_guard lock( m_mtx );
            if ( m_in_play )
                flush_queue_at( game, m_prepared_game_time );
        }

    private:
        void update_locked( const osu::game_snapshot_t& game, const osu::beatmap_data_t& map,
                            bool force_enabled, bool flush_output ) {
            if ( ( !enabled && !force_enabled ) || game.cur_state != osu::game_state_t::play ||
                 !map.loaded || map.objects.empty( ) ) {
                if ( m_in_play ) {
                    m_smart_active = false;
                    leave_play( game );
                }
                return;
            }

            // Smart Relax never overrides Autobot's shared schedule.
            const bool want_smart = enabled && !m_autobot_accuracy_mode &&
                relax_mode == static_cast<int>( relax_mode_t::smart );

            m_in_play = true;
            const int game_time = game.cur_time;

            if ( game_time < m_last_game_time - 200 ) {
                reset_state( game );
            }

            const int applied_mode = m_autobot_accuracy_mode ? 0 : relax_mode;
            if ( m_smart_applied_mode != applied_mode ) {
                m_smart_applied_mode = applied_mode;
                reset_state( game );
            }
            m_smart_active = want_smart;

            if ( m_live_session && ( !live_hit_feedback || m_smart_active || m_autobot_accuracy_mode ) )
                reset_state( game );

            if ( !m_smart_active && !m_autobot_accuracy_mode && live_hit_feedback && game.live_playfield &&
                 !map.objects.empty( ) && map.objects.front( ).source_object != 0 ) {
                if(!m_live_session) { auto density_rng=m_rng; m_live_density_seed=density_rng(); }
                m_live_session = true;
            }

            resolve_keys( game, m_smart_k1, m_smart_k2 );
            m_smart_speed = game.speed_mult > 0.01f ? game.speed_mult : 1.f;
            if ( game_time != m_smart_ref_game_time ) {
                m_smart_ref_game_time = game_time;
                m_smart_ref_qpc_ms = qpc_ms( 0 );
            }

            schedule_clicks( game, map );

            m_last_game_time = game_time;

            advance_past_objects( game, map );
            purge_stale( game_time );
            if ( flush_output ) {
                const double est_gt = get_interpolated_game_time( game );
                if ( m_smart_active )
                    smart_tick( est_gt );
                flush_queue_at( game, est_gt );
            }
        }

        struct scheduled_click_t {
            int press_time = 0;
            int release_time = 0;
            int timing_variation_ms = 0;
            WORD key = 0;
            bool pressed = false;
            bool released = false;
            bool protected_hold = false;
            int object_index = -1;
            bool judgment_observed = false;
            float live_jitter_unit = 0.f;
            float live_drift_unit = 0.f;
        };

        std::vector<scheduled_click_t> m_click_queue;
        std::vector<uint64_t> m_live_sources;
        std::vector<int> m_live_start_times;
        std::vector<bool> m_live_unique_times;
        std::vector<bool> m_live_spinners;
        uint32_t m_live_density_seed=0;
        bool m_live_session = false;
        int m_live_last_dispatch = INT32_MIN;
        live_telemetry_t m_live_stats;
        uint64_t m_live_good_wall = 0;
        int m_live_good_time = 0;
        std::vector<scheduled_object_t> m_scheduled_objects;
        bool m_in_play = false;
        int m_last_game_time = 0;
        int m_last_hit_obj_idx = -1;
        int m_scheduled_through_idx = -1;
        int m_last_click_time = -99999;
        tap_style_state_t m_tap_state = tap_style_state_t::singletap;
        int m_last_key_index = -1;
        int m_recovery_notes = 0;
        float m_fatigue = 0.f;
        int m_controller_tap_style = -1;
        int m_controller_primary_key = -1;
        bool m_left_down = false;
        bool m_right_down = false;
        int m_last_audio_time = 0;
        double m_last_audio_sync_time = 0.0;
        double m_prepared_game_time = 0.0;
        float m_last_jitter = 0.f;
        float m_timing_drift = 0.f;
        float m_timing_drift_target = 0.f;
        int m_timing_drift_last_time = 0;
        int m_timing_drift_next_target_time = 0;
        bool m_timing_drift_initialized = false;
        bool m_autobot_accuracy_mode = false;
        bool m_autobot_perfect_play = false;
        float m_autobot_target_accuracy = 100.f;
        // Target Accuracy debt controller state (autobot-accuracy mode only).
        double m_acc_loss = 0.0;            // Σ accuracy loss actually planned
        uint64_t m_acc_objects = 0;         // objects scheduled (denominator)
        uint64_t m_acc_grade_300 = 0;
        uint64_t m_acc_grade_100 = 0;
        uint64_t m_acc_grade_50 = 0;
        uint64_t m_acc_controlled_100 = 0;  // controlled 100 outcomes placed
        uint64_t m_acc_controlled_50 = 0;
        uint64_t m_acc_unsafe_at_100 = 0;
        uint64_t m_acc_timing_clamps = 0;
        uint64_t m_acc_dense_groups = 0;
        float m_acc_safe_300_margin_ms = 0.f;
        int m_accuracy_cooldown = 0;
        int m_accuracy_50_cooldown = 0;
        float m_accuracy_side_bias = 0.f;

        static constexpr double k_loss_100 = 2.0 / 3.0;   // loss of one 100 vs a 300
        static constexpr double k_loss_50 = 5.0 / 6.0;    // loss of one 50 vs a 300

        // ---- Smart Relax state ------------------------------------------
        // One entry per hit object so streams cannot desync or let one
        // physical press be consumed by the wrong note.
        struct smart_object_t {
            int start_time = 0;
            bool hold = false;      // slider head / spinner: Relax owns the hold
            bool armed = false;     // the prepared fallback click may fire
            bool fired = false;     // the fallback/corrected click actually pressed
            bool resolved = false;  // no longer eligible to match a press
        };

        // How long after an object a stray physical press is still attributed
        // to it (full osu! hit window plus slack).
        static constexpr int k_smart_absorb_ms = 220;

        std::vector<smart_object_t> m_smart_objects;
        bool m_smart_active = false;
        bool m_rescue_credit = false;
        bool m_hybrid_down[2] = {false, false};
        bool m_hybrid_manual = false;
        uint64_t m_hybrid_last_input = 0;
        bool hybrid_manual_active() const {
            return m_hybrid_down[0] || m_hybrid_down[1] ||
                (m_hybrid_manual && GetTickCount64() - m_hybrid_last_input <
                    static_cast<uint64_t>(std::clamp(hybrid_delay_ms, 0, 1000)));
        }
        double m_last_manual_game_time = 0;
        uint64_t m_last_manual_wall_time = 0;
        int m_smart_applied_mode = -1;
        int m_smart_scan_idx = 0;
        WORD m_smart_k1 = 'Z';
        WORD m_smart_k2 = 'X';
        bool m_smart_phys_down[ 2 ] = { false, false };
        bool m_smart_suppress_up[ 2 ] = { false, false };
        double m_smart_ref_qpc_ms = 0.0;
        int m_smart_ref_game_time = 0;
        float m_smart_speed = 1.f;

        static double qpc_ms( int64_t counter ) {
            static LARGE_INTEGER freq{};
            static bool init = false;
            if ( !init ) { QueryPerformanceFrequency( &freq ); init = true; }
            if ( freq.QuadPart <= 0 ) return 0.0;
            if ( counter <= 0 ) {
                LARGE_INTEGER now{};
                QueryPerformanceCounter( &now );
                counter = now.QuadPart;
            }
            return static_cast<double>( counter ) / static_cast<double>( freq.QuadPart ) * 1000.0;
        }

        double smart_game_time( int64_t press_qpc ) const {
            double elapsed = qpc_ms( press_qpc ) - m_smart_ref_qpc_ms;
            if ( elapsed < 0.0 ) elapsed = 0.0;
            if ( elapsed > 500.0 ) elapsed = 500.0;
            return static_cast<double>( m_smart_ref_game_time ) +
                   elapsed * static_cast<double>( m_smart_speed );
        }

        void reset_smart_state( ) {
            m_smart_objects.clear( );
            m_hybrid_down[0] = m_hybrid_down[1] = false;
            m_hybrid_manual = false;
            m_rescue_credit = false;
            m_smart_physical = 0;
            m_smart_corrected = 0;
            m_smart_fallback = 0;
            m_smart_suppressed_late = 0;
            m_smart_last_target_offset = 0;
            m_smart_scan_idx = 0;
            m_smart_phys_down[ 0 ] = m_smart_phys_down[ 1 ] = false;
            m_smart_suppress_up[ 0 ] = m_smart_suppress_up[ 1 ] = false;
        }

        int smart_early_bound( ) const {
            return manual_offset_ms + std::min( std::clamp( early_variation_ms, -25, 0 ), -2 );
        }

        int smart_late_bound( ) const {
            return manual_offset_ms + std::max( std::clamp( late_variation_ms, 0, 25 ), 2 );
        }

        // Latest offset a Smart Relax injected hit may target.  Never exceeds
        // the configured Relax late variation bound.
        int smart_fallback_late_bound( ) const {
            return manual_offset_ms + std::min(
                std::clamp( smart_rescue_deadline_ms, 0, 25 ),
                std::clamp( late_variation_ms, 0, 25 ) );
        }

        void smart_drop_click( int object_index ) {
            m_click_queue.erase(
                std::remove_if( m_click_queue.begin( ), m_click_queue.end( ),
                    [ object_index ]( const scheduled_click_t& c ) {
                        return c.object_index == object_index && !c.pressed;
                    } ),
                m_click_queue.end( ) );
        }

        // A slider head / spinner the player hit inside the allowed range:
        // the physical press reaches the game, but the existing Relax slider
        // hold logic keeps owning the release so the key can never stick.
        void smart_adopt_hold( int object_index, int slot, double now ) {
            const WORD key = slot == 0 ? m_smart_k1 : m_smart_k2;
            for ( auto& c : m_click_queue ) {
                if ( c.object_index != object_index || c.pressed )
                    continue;
                const int hold_len = std::max( c.release_time - c.press_time, 20 );
                c.key = key;
                c.press_time = static_cast<int>( now );
                c.release_time = std::max( c.release_time, c.press_time + hold_len );
                c.pressed = true;
                ( slot == 0 ? m_left_down : m_right_down ) = true;
                return;
            }
        }

        // A resolved-but-still-listed object must stop absorbing presses once
        // the next hittable object's own timing window has opened, otherwise a
        // stream would let one note eat the press meant for the following one.
        bool smart_press_belongs_to_next( int idx, double now ) const {
            for ( int i = idx + 1; i < static_cast<int>( m_smart_objects.size( ) ); ++i ) {
                const auto& nx = m_smart_objects[ static_cast<size_t>( i ) ];
                if ( nx.resolved )
                    continue;
                return now >= static_cast<double>( nx.start_time + smart_early_bound( ) );
            }
            return false;
        }

        bool smart_match_press( int slot, double now ) {
            const int n = static_cast<int>( m_smart_objects.size( ) );
            const int early_lo = smart_early_bound( );
            const int late_pass = smart_late_bound( );
            const int capture = std::clamp( smart_early_capture_ms, 0, 400 );

            for ( int idx = m_smart_scan_idx; idx < n; ++idx ) {
                auto& o = m_smart_objects[ static_cast<size_t>( idx ) ];
                if ( o.resolved ) {
                    smart_advance_scan( );
                    continue;
                }

                const double d = now - static_cast<double>( o.start_time );

                if ( o.fired ) {
                    // The prepared fallback already hit this note.  Swallow the
                    // trailing physical press so it can neither double tap nor
                    // leak into the next object.
                    const bool absorb = d <= static_cast<double>( k_smart_absorb_ms ) &&
                        !smart_press_belongs_to_next( idx, now );
                    o.resolved = true;
                    smart_advance_scan( );
                    if ( absorb ) {
                        ++m_smart_suppressed_late;
                        m_smart_suppress_up[ slot ] = true;
                        return true;
                    }
                    continue;
                }

                if ( d > static_cast<double>( late_pass ) ) {
                    o.resolved = true;
                    smart_drop_click( idx );
                    smart_advance_scan( );
                    continue;
                }

                if ( d >= static_cast<double>( early_lo ) ) {
                    // Inside the allowed range: the real press wins and the
                    // prepared fallback for this object is cancelled.
                    o.resolved = true;
                    o.armed = false;
                    smart_advance_scan( );
                    ++m_smart_physical;
                    if ( o.hold ) {
                        smart_adopt_hold( idx, slot, now );
                        m_smart_suppress_up[ slot ] = true;
                    }
                    else {
                        smart_drop_click( idx );
                    }
                    return false;
                }

                if ( d >= -static_cast<double>( capture ) ) {
                    if ( smart_early_correction ) {
                        // Clearly meant for this note but too early: swallow it
                        // and let the prepared in-range click carry the hit.
                        o.armed = true;
                        o.resolved = true;
                        smart_advance_scan( );
                        ++m_smart_corrected;
                        m_smart_suppress_up[ slot ] = true;
                        return true;
                    }
                    o.resolved = true;
                    o.armed = false;
                    smart_drop_click( idx );
                    smart_advance_scan( );
                    ++m_smart_physical;
                    return false;
                }

                // Too early to be intended for this note - leave it alone.
                return false;
            }
            return false;
        }

        void smart_advance_scan( ) {
            while ( m_smart_scan_idx < static_cast<int>( m_smart_objects.size( ) ) &&
                    m_smart_objects[ static_cast<size_t>( m_smart_scan_idx ) ].resolved )
                ++m_smart_scan_idx;
        }

        // The fallback is scheduled ahead of time by the normal Relax timing
        // generator, so this only retires objects whose window has passed.
        void smart_tick( double now ) {
            const int n = static_cast<int>( m_smart_objects.size( ) );
            for ( int i = m_smart_scan_idx; i < n; ++i ) {
                auto& o = m_smart_objects[ static_cast<size_t>( i ) ];
                if ( o.resolved )
                    continue;
                if ( static_cast<double>( o.start_time ) > now + 5.0 )
                    break;
                const bool expired =
                    now > static_cast<double>( o.start_time + k_smart_absorb_ms );
                if ( o.fired ) {
                    if ( expired || smart_press_belongs_to_next( i, now ) )
                        o.resolved = true;
                    continue;
                }
                if ( expired ) {
                    o.resolved = true;
                    smart_drop_click( i );
                }
            }
            smart_advance_scan( );
        }

        bool smart_click_armed( const scheduled_click_t& c, double now ) const {
            if ( c.object_index < 0 ||
                 c.object_index >= static_cast<int>( m_smart_objects.size( ) ) )
                return false;
            const auto& object = m_smart_objects[ static_cast<size_t>( c.object_index ) ];
            if (!object.armed) return false;
            // An early correction already has a real manual press behind it.
            if (object.resolved) return true;
            const double speed = std::max(double(m_smart_speed), 0.01);
            const double age = (now - m_last_manual_game_time) / speed;
            return smart_miss_rescue && m_rescue_credit &&
                age >= 0 && age <= 350 &&
                GetTickCount64() - m_last_manual_wall_time <= 350 &&
                now - c.press_time <= 20 * speed;
        }

        // Records that a Smart Relax owned click actually fired.
        void smart_note_fired( const scheduled_click_t& c ) {
            if ( c.object_index < 0 ||
                 c.object_index >= static_cast<int>( m_smart_objects.size( ) ) )
                return;
            auto& o = m_smart_objects[ static_cast<size_t>( c.object_index ) ];
            if ( o.fired )
                return;
            o.fired = true;
            m_smart_last_target_offset = c.press_time - o.start_time;
            if ( !o.resolved ) {
                m_rescue_credit = false;
                ++m_smart_fallback;
            }
        }

        uint32_t m_smart_physical = 0;
        uint32_t m_smart_corrected = 0;
        uint32_t m_smart_fallback = 0;
        uint32_t m_smart_suppressed_late = 0;
        int m_smart_last_target_offset = 0;

        c_nt_input m_nt;
#ifdef TENSHIZ_RELAX_TESTING
        std::function<void( WORD, bool )> m_test_input_sink;
#endif

        mutable std::mutex m_mtx;

        std::mt19937 m_rng{ []() -> uint32_t {
            try { return std::random_device{}( ); }
            catch ( ... ) { return static_cast<uint32_t>( std::chrono::high_resolution_clock::now( ).time_since_epoch( ).count( ) ); }
        }( ) };
        std::normal_distribution<float> m_norm{ 0.f, 1.f };
        std::uniform_real_distribution<float> m_unit{ 0.f, 1.f };
        std::uniform_real_distribution<float> m_sub_ms{ -1.49f, 1.49f };

        float generate_hold( float center, float spread ) {
            const float z = m_norm( m_rng );

            const float skew = 0.3f;
            const float shaped = z + skew * ( z * z - 1.f );

            float hold = center + shaped * spread;
            hold += ( m_unit( m_rng ) - 0.5f ) * 2.f;

            return std::clamp( hold, hold_floor, hold_ceiling );
        }

        float hit_jitter( const osu::beatmap_data_t& map ) {
            const float sigma = ur / 10.f;
            float new_jitter = m_norm( m_rng ) * ( sigma * 2.2f ) + m_sub_ms( m_rng );
            float jitter = 0.6f * m_last_jitter + 0.4f * new_jitter;
            m_last_jitter = jitter;

            float actual_od = map.od;
            if ( map.hr ) actual_od = std::min( 10.f, actual_od * 1.4f );
            if ( map.ez ) actual_od = actual_od * 0.5f;

            const float hit_window_300 = 79.5f - 6.f * actual_od;
            const float max_deviation = std::max( 5.f, hit_window_300 - 1.f );

            return std::clamp( jitter, -max_deviation, max_deviation );
        }

        float rapid_hit_jitter( const osu::beatmap_data_t& map ) {
            const float sigma = ur / 10.f;
            const float new_jitter = m_norm( m_rng ) * ( sigma * 2.2f ) + m_sub_ms( m_rng );
            // Match the regular AR(1) filter's steady-state spread without
            // inheriting any part of the preceding stream note's error.
            const float jitter = 0.5f * new_jitter;
            m_last_jitter = jitter;

            float actual_od = map.od;
            if ( map.hr ) actual_od = std::min( 10.f, actual_od * 1.4f );
            if ( map.ez ) actual_od = actual_od * 0.5f;

            const float hit_window_300 = 79.5f - 6.f * actual_od;
            const float max_deviation = std::max( 5.f, hit_window_300 - 1.f );
            return std::clamp( jitter, -max_deviation, max_deviation );
        }

        void reset_timing_drift( ) {
            m_timing_drift = 0.f;
            m_timing_drift_target = 0.f;
            m_timing_drift_last_time = 0;
            m_timing_drift_next_target_time = 0;
            m_timing_drift_initialized = false;
        }

        float slow_timing_bias( int object_time ) {
            const float limit = static_cast<float>( std::clamp( timing_drift_ms, 0, 8 ) );
            if ( limit <= 0.f ) {
                reset_timing_drift( );
                return 0.f;
            }

            if ( !m_timing_drift_initialized ) {
                m_timing_drift_initialized = true;
                m_timing_drift_last_time = object_time;
                m_timing_drift_target = ( m_unit( m_rng ) * 2.f - 1.f ) * limit;
                m_timing_drift_next_target_time = object_time +
                    static_cast<int>( std::lround( 2000.f + m_unit( m_rng ) * 4000.f ) );
                return 0.f;
            }

            m_timing_drift_target = std::clamp( m_timing_drift_target, -limit, limit );
            m_timing_drift = std::clamp( m_timing_drift, -limit, limit );

            const int elapsed_ms = std::max( object_time - m_timing_drift_last_time, 0 );
            constexpr float response_ms = 2500.f;
            const float alpha = 1.f - std::exp(
                -static_cast<float>( elapsed_ms ) / response_ms );
            m_timing_drift += ( m_timing_drift_target - m_timing_drift ) * alpha;
            m_timing_drift = std::clamp( m_timing_drift, -limit, limit );
            m_timing_drift_last_time = object_time;

            if ( object_time >= m_timing_drift_next_target_time ) {
                m_timing_drift_target = ( m_unit( m_rng ) * 2.f - 1.f ) * limit;
                m_timing_drift_next_target_time = object_time +
                    static_cast<int>( std::lround( 2000.f + m_unit( m_rng ) * 4000.f ) );
            }

            return m_timing_drift;
        }

        static void resolve_keys( const osu::game_snapshot_t& game, WORD& k1, WORD& k2 ) {
            k1 = static_cast<WORD>( game.left_key );
            k2 = static_cast<WORD>( game.right_key );
            if ( !k1 ) k1 = 'Z';
            if ( !k2 ) k2 = 'X';
        }

        void reset_accuracy_controller( ) {
            m_acc_loss = 0.0; m_acc_objects = 0; m_acc_grade_300 = 0;
            m_acc_grade_100 = 0; m_acc_grade_50 = 0;
            m_acc_controlled_100 = 0; m_acc_controlled_50 = 0;
            m_acc_unsafe_at_100 = 0;
            m_acc_timing_clamps = 0; m_acc_dense_groups = 0; m_acc_safe_300_margin_ms = 0.f;
            m_accuracy_cooldown = 0; m_accuracy_50_cooldown = 0;
            m_accuracy_side_bias = 0.f;
        }

        void reset_state( const osu::game_snapshot_t& game ) {
            release_all_keys( game );
            m_click_queue.clear( );
            m_scheduled_objects.clear( );
            m_last_hit_obj_idx = -1;
            m_scheduled_through_idx = -1;
            m_last_click_time = -99999;
            reset_tap_controller( );
            m_last_audio_time = 0;
            m_last_audio_sync_time = 0.0;
            m_last_jitter = 0.f;
            reset_timing_drift( );
            reset_accuracy_controller( );
            reset_smart_state( );
            m_live_session = false;
            m_live_last_dispatch = INT32_MIN;
            m_live_good_wall = 0;
            m_live_good_time = 0;
            m_live_stats = {};
        }

        void release_all_keys( const osu::game_snapshot_t& game ) {
            WORD k1 = 0, k2 = 0;
            resolve_keys( game, k1, k2 );
            if ( m_live_session ) {
                live_key( k1, false, m_left_down );
                live_key( k2, false, m_right_down );
                return;
            }
            if ( m_left_down ) {
                release_key( k1, m_left_down );
            }
            if ( m_right_down ) {
                release_key( k2, m_right_down );
            }
        }

        void press_key( WORD vk, bool& down ) {
            if ( !vk || down ) return;
#ifdef TENSHIZ_RELAX_TESTING
            if ( m_test_input_sink ) { m_test_input_sink( vk, true ); down = true; return; }
#endif
            if ( m_nt.available( ) ) m_nt.press( vk );
            else input::press_vk( vk );
            down = true;
        }

        void release_key( WORD vk, bool& down ) {
            if ( !vk || !down ) return;
#ifdef TENSHIZ_RELAX_TESTING
            if ( m_test_input_sink ) { m_test_input_sink( vk, false ); down = false; return; }
#endif
            if ( m_nt.available( ) ) m_nt.release( vk );
            else input::release_vk( vk );
            down = false;
        }

        struct tap_pattern_t {
            int notes = 1;
            float average_interval_ms = 9999.f;
            bool clearly_fast = false;
        };

        void reset_tap_controller( ) {
            m_tap_state = tap_style_state_t::singletap;
            m_last_key_index = -1;
            m_recovery_notes = 0;
            m_fatigue = 0.f;
            m_controller_tap_style = -1;
            m_controller_primary_key = -1;
        }

        float comfortable_interval_ms( ) const {
            const int speed = std::clamp( singletap_speed_bpm, 100, 400 );
            return 60000.f / static_cast<float>( speed );
        }

        int tolerated_burst_notes( ) const {
            static constexpr int notes_by_level[] = { 2, 4, 6 };
            return notes_by_level[ std::clamp( (burst_tolerance+25)/50, 0, 2 ) ];
        }

        tap_pattern_t analyze_tap_pattern( const osu::beatmap_data_t& map, int object_index ) const {
            tap_pattern_t result{};
            const float comfort_ms = comfortable_interval_ms( );
            const float transition_interval = comfort_ms * 1.12f;
            float interval_sum = 0.f;
            int interval_count = 0;

            // Seven intervals covers the current object plus up to seven upcoming heads.
            for ( int look = 0; look < 7; ++look ) {
                const int current = object_index + look;
                const int next = current + 1;
                if ( next >= static_cast<int>( map.objects.size( ) ) )
                    break;

                const auto& a = map.objects[ static_cast<size_t>( current ) ];
                const auto& b = map.objects[ static_cast<size_t>( next ) ];
                if ( ( a.type & 8 ) != 0 || ( b.type & 8 ) != 0 )
                    break;

                const int delta = b.start_time - a.start_time;
                if ( delta <= 0 || static_cast<float>( delta ) > transition_interval )
                    break;

                interval_sum += static_cast<float>( delta );
                interval_count++;
            }

            if ( interval_count > 0 ) {
                result.notes = interval_count + 1;
                result.average_interval_ms = interval_sum / static_cast<float>( interval_count );
                result.clearly_fast = result.average_interval_ms <= comfort_ms * 0.88f;
            }
            return result;
        }

        void recover_tap_stamina( const osu::beatmap_data_t& map, int object_index, int previous_interval_ms ) {
            if ( object_index <= 0 || previous_interval_ms <= 0 )
                return;

            const float endurance = std::clamp( stamina, 0.f, 100.f ) / 100.f;
            const float comfort_ms = comfortable_interval_ms( );
            const float extra_gap = std::max( 0.f, static_cast<float>( previous_interval_ms ) - comfort_ms * 0.75f );
            m_fatigue -= extra_gap * ( 0.00022f + endurance * 0.00012f );

            const auto& previous = map.objects[ static_cast<size_t>( object_index - 1 ) ];
            if ( ( previous.type & 8 ) != 0 || ( ( previous.type & 2 ) != 0 && previous.end_time > previous.start_time ) ) {
                const int held_rest = std::clamp( previous.end_time - previous.start_time, 0, previous_interval_ms );
                m_fatigue -= static_cast<float>( held_rest ) * ( 0.00035f + endurance * 0.00015f );
            }

            m_fatigue = std::clamp( m_fatigue, 0.f, 1.f );
            const int reset_gap = std::max( 650, static_cast<int>( std::round( comfort_ms * 3.f ) ) );
            if ( previous_interval_ms >= reset_gap || ( previous.type & 8 ) != 0 ) {
                m_fatigue = 0.f;
                m_tap_state = tap_style_state_t::singletap;
                m_recovery_notes = 0;
            }
        }

        void apply_tap_load( int previous_interval_ms, bool alternating ) {
            if ( previous_interval_ms <= 0 || previous_interval_ms > 2000 )
                return;

            const float endurance = std::clamp( stamina, 0.f, 100.f ) / 100.f;
            const float rate_ratio = comfortable_interval_ms( ) /
                static_cast<float>( std::max( previous_interval_ms, 1 ) );

            if ( alternating ) {
                m_fatigue -= 0.025f + endurance * 0.020f;
            }
            else if ( rate_ratio <= 0.72f ) {
                m_fatigue -= ( 0.72f - rate_ratio ) * ( 0.035f + endurance * 0.025f );
            }
            else {
                const float demand = rate_ratio - 0.72f;
                const float capacity = 0.55f + endurance * 0.75f;
                m_fatigue += demand * 0.11f / capacity;
            }
            m_fatigue = std::clamp( m_fatigue, 0.f, 1.f );
        }

        int choose_tap_key( const osu::beatmap_data_t& map, int object_index, int previous_interval_ms ) {
            const int primary = std::clamp( primary_key, 0, 1 );
            const int configured_style = std::clamp( tap_style, 0, 1 );
            if ( configured_style != m_controller_tap_style || primary != m_controller_primary_key ) {
                m_tap_state = tap_style_state_t::singletap;
                m_last_key_index = -1;
                m_recovery_notes = 0;
                m_fatigue = 0.f;
                m_controller_tap_style = configured_style;
                m_controller_primary_key = primary;
            }
            recover_tap_stamina( map, object_index, previous_interval_ms );

            if ( static_cast<tap_style_t>( configured_style ) == tap_style_t::alternate ) {
                m_tap_state = tap_style_state_t::alternate;
            }
            else {
                const auto& obj = map.objects[ static_cast<size_t>( object_index ) ];
                if ( ( obj.type & 8 ) != 0 ) {
                    m_tap_state = tap_style_state_t::singletap;
                    m_recovery_notes = 0;
                }
                else {
                    const tap_pattern_t pattern = analyze_tap_pattern( map, object_index );
                    const bool dense_pattern = pattern.notes >= 2;
                    const bool sustained = pattern.notes > tolerated_burst_notes( );
                    const float endurance = std::clamp( stamina, 0.f, 100.f ) / 100.f;
                    const bool fatigued = m_fatigue >= 0.45f + endurance * 0.45f;
                    const int recovery_gap = std::max( 360,
                        static_cast<int>( std::round( comfortable_interval_ms( ) * 1.65f ) ) );

                    switch ( m_tap_state ) {
                        case tap_style_state_t::singletap:
                            if ( dense_pattern && pattern.clearly_fast )
                                m_tap_state = sustained ? tap_style_state_t::alternate : tap_style_state_t::burst;
                            else if ( dense_pattern && ( sustained || fatigued ) )
                                m_tap_state = tap_style_state_t::alternate;
                            break;

                        case tap_style_state_t::burst:
                        case tap_style_state_t::alternate:
                            if ( !dense_pattern ) {
                                m_tap_state = tap_style_state_t::recovery;
                                m_recovery_notes = 1;
                            }
                            else if ( m_tap_state == tap_style_state_t::burst && sustained ) {
                                m_tap_state = tap_style_state_t::alternate;
                            }
                            break;

                        case tap_style_state_t::recovery:
                            if ( dense_pattern ) {
                                m_tap_state = sustained ? tap_style_state_t::alternate : tap_style_state_t::burst;
                            }
                            else if ( previous_interval_ms >= recovery_gap ) {
                                m_tap_state = tap_style_state_t::singletap;
                                m_recovery_notes = 0;
                            }
                            else if ( m_recovery_notes > 0 ) {
                                m_recovery_notes--;
                            }
                            else {
                                m_tap_state = tap_style_state_t::singletap;
                            }
                            break;
                    }
                }
            }

            const bool alternating = m_tap_state != tap_style_state_t::singletap;
            int chosen = primary;
            if ( alternating && m_last_key_index >= 0 )
                chosen = 1 - m_last_key_index;

            apply_tap_load( previous_interval_ms, alternating );
            m_last_key_index = chosen;
            return chosen;
        }

        void advance_past_objects( const osu::game_snapshot_t& game, const osu::beatmap_data_t& map ) {
            const int gt = game.cur_time;
            while ( m_last_hit_obj_idx + 1 < static_cast<int>( map.objects.size( ) ) ) {
                const auto& obj = map.objects[ static_cast<size_t>( m_last_hit_obj_idx + 1 ) ];
                if ( obj.start_time < gt - 50 )
                    m_last_hit_obj_idx++;
                else
                    break;
            }
        }

        void purge_stale( int game_time ) {
            // Smart Relax holds an unpressed click until smart_tick retires
            // its object.
            const int grace = m_live_session ? 400 : m_smart_active ? 50 + k_smart_absorb_ms + 40 : 50;
            m_click_queue.erase(
                std::remove_if( m_click_queue.begin( ), m_click_queue.end( ),
                    [ game_time, grace ]( const scheduled_click_t& c ) {
                        return !c.pressed && c.press_time < game_time - grace;
                    } ),
                m_click_queue.end( ) );
        }

        void schedule_clicks( const osu::game_snapshot_t& game, const osu::beatmap_data_t& map ) {
            WORD k1 = 0, k2 = 0;
            resolve_keys( game, k1, k2 );
            if ( m_scheduled_objects.size( ) < map.objects.size( ) )
                m_scheduled_objects.resize( map.objects.size( ) );
            if ( m_smart_objects.size( ) < map.objects.size( ) )
                m_smart_objects.resize( map.objects.size( ) );
            m_live_sources.resize( map.objects.size( ) );
            m_live_start_times.resize( map.objects.size( ) );
            m_live_unique_times.resize( map.objects.size( ) );
            m_live_spinners.resize( map.objects.size( ) );

            int current_stream_length = 0;
            int last_obj_start_time = -99999;
            if ( m_scheduled_through_idx >= 0 && m_scheduled_through_idx < static_cast<int>( map.objects.size( ) ) ) {
                last_obj_start_time = map.objects[ static_cast<size_t>( m_scheduled_through_idx ) ].start_time;
            }

            for ( int i = m_scheduled_through_idx + 1; i < static_cast<int>( map.objects.size( ) ); ++i ) {
                const auto& obj = map.objects[ static_cast<size_t>( i ) ];
                m_live_sources[ i ] = obj.source_object;
                m_live_start_times[ i ] = obj.start_time;
                m_live_unique_times[i] = (i==0 || map.objects[i-1].start_time!=obj.start_time) &&
                    (i+1==static_cast<int>(map.objects.size()) || map.objects[i+1].start_time!=obj.start_time);
                m_live_spinners[ i ] = ( obj.type & 8 ) != 0;

                const int prev_interval = obj.start_time - last_obj_start_time;
                if ( prev_interval > 0 && prev_interval < 100 ) {
                    current_stream_length++;
                }
                else {
                    current_stream_length = 0;
                }
                last_obj_start_time = obj.start_time;
                int next_object_interval = 99999;
                if ( i + 1 < static_cast<int>( map.objects.size( ) ) )
                    next_object_interval = map.objects[ static_cast<size_t>( i + 1 ) ].start_time - obj.start_time;
                const bool ordinary_circle = ( obj.type & 1 ) != 0 &&
                    ( obj.type & 2 ) == 0 && ( obj.type & 8 ) == 0;
                const bool rapid_sequence = ordinary_circle &&
                    !m_autobot_accuracy_mode && !m_smart_active &&
                    ( ( prev_interval > 0 && prev_interval <= 150 ) ||
                      ( next_object_interval > 0 && next_object_interval <= 150 ) );
                const float jitter_ms = rapid_sequence
                    ? rapid_hit_jitter( map )
                    : hit_jitter( map );
                float natural_error_ms = jitter_ms;
                int applied_variation = static_cast<int>( std::round( natural_error_ms ) );
                if ( timing_variation ) {
                    const int early = std::clamp( early_variation_ms, -25, 0 );
                    const int late = std::clamp( late_variation_ms, 0, 25 );
                    natural_error_ms += slow_timing_bias( obj.start_time );
                    applied_variation = static_cast<int>( std::round( natural_error_ms ) );
                    applied_variation = std::clamp( applied_variation, early, late );
                }

                const int base_press_time = obj.start_time + manual_offset_ms;
                int press_time = base_press_time + applied_variation;
                int accuracy_timing_ms = 0;
                int simulated_grade = 300;
                float accuracy_edge_bias = 0.f;

                // Target Accuracy feeds the existing shared schedule (there is no
                // second timing engine).  Normalized score-loss debt chooses 300/100/
                // occasional 50 outcomes, then the existing ordering guard validates
                // the final press.  Standalone Relax never enters this branch.
                const bool circle = (obj.type & 1) != 0 && (obj.type & 2) == 0 && (obj.type & 8) == 0;
                bool controlled_100 = false;
                bool controlled_50 = false;
                float actual_od = map.od;
                if ( map.hr ) actual_od = std::min( 10.f, actual_od * 1.4f );
                if ( map.ez ) actual_od *= 0.5f;
                const float window_300 = std::max( 5.f, 79.5f - 6.f * actual_od );
                const float window_100 = std::max( window_300 + 4.f, 139.5f - 8.f * actual_od );
                const float window_50 = std::max( window_100 + 4.f, 199.5f - 10.f * actual_od );
                if ( m_autobot_accuracy_mode && !m_autobot_perfect_play ) {
                    int next_start_gap = 100000;
                    if ( i + 1 < static_cast<int>( map.objects.size( ) ) )
                        next_start_gap = map.objects[ static_cast<size_t>( i + 1 ) ].start_time - obj.start_time;
                    if ( m_accuracy_cooldown > 0 ) --m_accuracy_cooldown;
                    if ( m_accuracy_50_cooldown > 0 ) --m_accuracy_50_cooldown;

                    const double f = std::clamp( m_autobot_target_accuracy, 80.f, 99.99f ) / 100.0;
                    const double desired_after = ( 1.0 - f ) * static_cast<double>( m_acc_objects + 1 );
                    const double deficit = desired_after - m_acc_loss;
                    const float early_room = m_last_click_time > -99999
                        ? static_cast<float>( obj.start_time - ( m_last_click_time + 2 ) ) : 100000.f;
                    const float late_room = static_cast<float>( next_start_gap - 2 );
                    m_accuracy_side_bias = std::clamp( m_accuracy_side_bias + ( m_unit( m_rng ) - 0.5f ) * 0.16f, -0.7f, 0.7f );

                    auto choose_offset = [&]( float lo, float hi ) -> int {
                        const bool early_ok = early_room >= lo;
                        const bool late_ok = late_room >= lo;
                        if ( !early_ok && !late_ok ) return 0;
                        bool early = m_unit( m_rng ) < 0.5f + 0.28f * m_accuracy_side_bias;
                        if ( early && !early_ok ) early = false;
                        else if ( !early && !late_ok ) early = true;
                        const float room = early ? early_room : late_room;
                        const float upper = std::max( lo, std::min( hi, room ) );
                        const float magnitude = lo + m_unit( m_rng ) * ( upper - lo );
                        return static_cast<int>( std::lround( ( early ? -1.f : 1.f ) * magnitude ) );
                    };

                    const bool sparse_for_50 = prev_interval >= 230 && next_start_gap >= 230;
                    const bool allow_50 = m_autobot_target_accuracy <= 87.5f && m_acc_objects >= 18 &&
                        m_accuracy_50_cooldown <= 0 && m_acc_controlled_50 * 25 <= m_acc_objects;
                    int signed_offset = 0;
                    if ( circle && allow_50 && deficit >= k_loss_50 * 0.36 && sparse_for_50 ) {
                        signed_offset = choose_offset( window_100 + 5.f, window_50 - 6.f );
                        if ( signed_offset != 0 ) {
                            press_time = obj.start_time + signed_offset;
                            simulated_grade = 50;
                            accuracy_edge_bias = 0.24f + m_unit( m_rng ) * 0.14f;
                            controlled_50 = true;
                            m_accuracy_50_cooldown = 14;
                            m_accuracy_cooldown = 2;
                        }
                    }
                    if ( !controlled_50 && circle && deficit >= k_loss_100 * 0.5 && m_accuracy_cooldown <= 0 ) {
                        bool dense_soon = false;
                        const bool dense_now = std::min( prev_interval, next_start_gap ) < 185;
                        for ( int look = 1; !dense_now && look <= 3 && i + look < static_cast<int>( map.objects.size( ) ); ++look ) {
                            const int gap = map.objects[ static_cast<size_t>( i + look ) ].start_time -
                                map.objects[ static_cast<size_t>( i + look - 1 ) ].start_time;
                            if ( gap > 0 && gap < 185 ) { dense_soon = true; break; }
                        }
                        const bool reserve_for_dense = !dense_now && dense_soon && deficit < k_loss_100 * 1.15;
                        if ( !reserve_for_dense ) signed_offset = choose_offset( window_300 + 4.f, window_100 - 5.f );
                        if ( signed_offset != 0 ) {
                            press_time = obj.start_time + signed_offset;
                            simulated_grade = 100;
                            accuracy_edge_bias = 0.15f + m_unit( m_rng ) * 0.14f;
                            controlled_100 = true;
                            m_accuracy_cooldown = m_autobot_target_accuracy < 90.f ? 1 :
                                                  m_autobot_target_accuracy < 96.f ? 2 : 3;
                        }
                    }
                }

                // Smart Relax hits (fallback or early-corrected) always land
                // inside the configured Relax variation range; the Rescue
                // Deadline only tightens the late side, it can never widen it.
                if ( m_smart_active ) {
                    const int lo = obj.start_time + smart_early_bound( );
                    const int hi = std::max( lo, obj.start_time + smart_fallback_late_bound( ) );
                    press_time = std::clamp( press_time, lo, hi );
                    applied_variation = press_time - base_press_time;
                }

                // Standalone Normal Relax keeps every target in its own object-local
                // variation band.  A collision may move this object only as far as
                // needed for ordering; it never changes the next object's baseline.
                // With increasing beatmap timestamps the configured bands always
                // contain a strictly ordered solution.  Equal timestamps are the
                // only physically impossible integer-millisecond case, where ordering
                // remains authoritative and may exceed the local upper bound.
                if ( !m_autobot_accuracy_mode && !m_smart_active && timing_variation ) {
                    const int lower = base_press_time + std::clamp( early_variation_ms, -25, 0 );
                    const int upper = base_press_time + std::clamp( late_variation_ms, 0, 25 );
                    press_time = std::clamp( press_time, lower, upper );
                    if ( m_last_click_time > -99999 && press_time <= m_last_click_time )
                        press_time = std::max( press_time, m_last_click_time + 1 );
                    applied_variation = press_time - base_press_time;
                }
                else if ( (timing_variation||m_autobot_accuracy_mode||m_smart_active) && m_last_click_time > -99999 ) {
                    if ( base_press_time > m_last_click_time ) {
                        // If the existing scheduler order was valid, variation may approach
                        // the previous hit but may never cross it.
                        press_time = std::max( press_time, m_last_click_time + 1 );
                    }
                    else if ( press_time < base_press_time ) {
                        // Do not let an early bounded UR value worsen an order collision
                        // that was already present in the legacy timing path.
                        press_time = base_press_time;
                    }
                    applied_variation = press_time - base_press_time;
                }

                // Perfect Play is a separate reliability policy, not target=100.
                // Its usable band models frame/update, interpolation, integer, and
                // dispatch uncertainty, then stays in the central third of the
                // actual 300 window.  The normal UR/base character may move inside
                // that band but can never ride the scoring boundary.
                if ( m_autobot_accuracy_mode && m_autobot_perfect_play ) {
                    const float speed = std::clamp( game.speed_mult, 0.5f, 2.f );
                    const float uncertainty = 2.f + ( 1000.f / 240.f ) * speed + 1.f + 1.f;
                    const int safe_half = std::max( 2, static_cast<int>( std::floor(
                        std::min( window_300 * 0.35f, window_300 - uncertainty - 2.f ) ) ) );
                    m_acc_safe_300_margin_ms = std::max( m_acc_safe_300_margin_ms,
                        window_300 - static_cast<float>( safe_half ) );
                    int lower = obj.start_time - safe_half;
                    int upper = obj.start_time + safe_half;
                    bool dense_group = false;
                    for ( int look = 1; look <= 5 && i + look < static_cast<int>( map.objects.size( ) ); ++look ) {
                        const int gap = map.objects[ static_cast<size_t>( i + look ) ].start_time -
                            map.objects[ static_cast<size_t>( i + look - 1 ) ].start_time;
                        if ( gap > 0 && gap < 185 ) dense_group = true;
                    }
                    if ( dense_group ) {
                        ++m_acc_dense_groups;
                        if ( i + 1 < static_cast<int>( map.objects.size( ) ) ) {
                            const int next_start = map.objects[ static_cast<size_t>( i + 1 ) ].start_time;
                            if ( next_start > obj.start_time ) upper = std::min( upper, next_start - 1 );
                        }
                    }
                    if ( m_last_click_time > -99999 ) lower = std::max( lower, m_last_click_time + 1 );
                    const int before = press_time;
                    if ( lower <= upper ) {
                        press_time = std::clamp( press_time, lower, upper );
                        if ( press_time != before ) ++m_acc_timing_clamps;
                    } else {
                        ++m_acc_unsafe_at_100;
                        press_time = std::max( obj.start_time, m_last_click_time + 1 );
                    }
                    applied_variation = press_time - base_press_time;
                }

                // Collision protection is authoritative.  Reclassify from the
                // final scheduled press so telemetry/debt can never claim a grade
                // that the actual timestamp no longer produces.
                if ( m_autobot_accuracy_mode ) {
                    const float final_error = std::abs( static_cast<float>( press_time - obj.start_time ) );
                    simulated_grade = final_error <= window_300 ? 300 :
                                      final_error <= window_100 ? 100 :
                                      final_error <= window_50 ? 50 : 0;
                    controlled_100 = controlled_100 && simulated_grade == 100;
                    controlled_50 = controlled_50 && simulated_grade == 50;
                    accuracy_timing_ms = ( controlled_100 || controlled_50 ) ? press_time - obj.start_time : 0;
                }

                m_acc_loss += simulated_grade == 100 ? k_loss_100 : simulated_grade == 50 ? k_loss_50 :
                              simulated_grade == 0 ? 1.0 : 0.0;
                m_acc_objects++;
                if ( simulated_grade == 300 ) m_acc_grade_300++;
                if ( simulated_grade == 100 ) m_acc_grade_100++;
                if ( simulated_grade == 50 ) m_acc_grade_50++;
                if ( controlled_100 ) m_acc_controlled_100++;
                if ( controlled_50 ) m_acc_controlled_50++;

                const bool is_slider = ( obj.type & 2 ) != 0;
                const bool is_spinner = ( obj.type & 8 ) != 0;
                const int slider_dur = obj.end_time - obj.start_time;
                const int natural_hold = ( ( is_slider && slider_dur >= 120 ) || is_spinner ) ? slider_dur : 0;
                int hold_dur = 0;

                int next_interval = 9999;
                if ( i + 1 < static_cast<int>( map.objects.size( ) ) ) {
                    next_interval = map.objects[ static_cast<size_t>( i + 1 ) ].start_time - obj.start_time;
                    if ( next_interval < 0 ) next_interval = 0;
                }

                int chosen_key_index = choose_tap_key( map, i, prev_interval );
                auto key_busy_at = [&]( WORD key, int at ) {
                    for ( const auto& queued : m_click_queue )
                        if ( queued.key == key && !queued.released && queued.release_time >= at ) return true;
                    return false;
                };
                WORD chosen = chosen_key_index == 1 ? k2 : k1;
                const int alternate_index = 1 - chosen_key_index;
                const WORD alternate = alternate_index == 1 ? k2 : k1;
                auto free_ordinary_hold = [&]( WORD key, int at ) {
                    for ( auto& queued : m_click_queue ) {
                        if ( queued.key != key || queued.released || queued.release_time < at || queued.protected_hold ) continue;
                        queued.release_time = at - 1;
                        if ( queued.object_index >= 0 && queued.object_index < static_cast<int>( m_scheduled_objects.size( ) ) )
                            m_scheduled_objects[ static_cast<size_t>( queued.object_index ) ].release_time = queued.release_time;
                    }
                };
                if ( key_busy_at( chosen, press_time ) && !key_busy_at( alternate, press_time ) ) {
                    chosen_key_index = alternate_index;
                    chosen = alternate;
                    m_last_key_index = chosen_key_index;
                } else if ( key_busy_at( chosen, press_time ) && key_busy_at( alternate, press_time ) ) {
                    free_ordinary_hold( chosen, press_time );
                    if ( key_busy_at( chosen, press_time ) ) free_ordinary_hold( alternate, press_time );
                    if ( key_busy_at( chosen, press_time ) && !key_busy_at( alternate, press_time ) ) {
                        chosen_key_index = alternate_index;
                        chosen = alternate;
                        m_last_key_index = chosen_key_index;
                    }
                }
                if ( m_autobot_perfect_play && key_busy_at( chosen, press_time ) )
                    ++m_acc_unsafe_at_100;
                if ( !chosen ) continue;

                if ( natural_hold > 0 ) {
                    const float tail = ( m_unit( m_rng ) * 2.f - 1.f ) * 5.f + m_sub_ms( m_rng );
                    hold_dur = natural_hold + static_cast<int>( tail );
                    if ( hold_dur < 15 ) hold_dur = 15;
                }
                else {
                    float center = chosen_key_index == 1 ? k2_hold_center : k1_hold_center;
                    float spread = chosen_key_index == 1 ? k2_hold_spread : k1_hold_spread;

                    if ( next_interval < 250 ) {
                        float max_allowed_center = static_cast<float>( next_interval ) * 0.52f;
                        float dynamic_floor = 34.f + m_unit( m_rng ) * 6.f;
                        if ( max_allowed_center < dynamic_floor ) {
                            max_allowed_center = dynamic_floor;
                        }
                        if ( max_allowed_center < center ) {
                            spread = spread * std::sqrt( max_allowed_center / center );
                            center = max_allowed_center;
                        }
                    }
                    float map_drift = ( static_cast<float>( i ) / static_cast<float>( map.objects.size( ) ) ) * 7.f;
                    float stream_drift = std::min( static_cast<float>( current_stream_length ) * 1.3f, 20.f );
                    float total_drift = map_drift + stream_drift;

                    center += total_drift;
                    float safe_cap = static_cast<float>( next_interval ) * 0.85f;
                    if ( center > safe_cap ) {
                        center = safe_cap;
                    }

                    hold_dur = static_cast<int>( generate_hold( center, spread ) );
                }

                if ( is_slider ) {
                    // Compensate from the final scheduled head press so the existing real
                    // slider endpoint remains the minimum release target.
                    const int k_tail_safety_ms = m_autobot_perfect_play ? 36 : 24;
                    const int needed_hold = obj.end_time - press_time + k_tail_safety_ms;
                    const int floor_hold = slider_dur - 30;
                    if ( hold_dur < floor_hold ) hold_dur = floor_hold;
                    if ( hold_dur < needed_hold ) hold_dur = needed_hold;
                }

                const int release_time = press_time + hold_dur;

                m_click_queue.push_back( {
                    press_time, release_time, applied_variation, chosen, false, false,
                    is_slider || is_spinner, i } );
                auto& live_click = m_click_queue.back();
                // Keep each sample stable while allowing pending notes to use
                // edited settings without restarting or releasing active holds.
                const float sigma = std::max(ur,0.f)*.1f;
                live_click.live_jitter_unit = sigma > .001f ? jitter_ms/sigma :
                    std::sin(static_cast<float>(i+1)*12.9898f)*1.4f;
                live_click.live_drift_unit = timing_variation && timing_drift_ms > 0 ?
                    (natural_error_ms-jitter_ms)/static_cast<float>(timing_drift_ms) :
                    std::sin(static_cast<float>(obj.start_time)*.0004f);
                m_scheduled_objects[ static_cast<size_t>( i ) ] = {
                    true, press_time, release_time, applied_variation,
                    accuracy_timing_ms, simulated_grade, accuracy_edge_bias,
                    chosen_key_index, m_tap_state };
                {
                    // Smart Relax bookkeeping.  Spinners are never matched against
                    // a physical tap and keep the existing automatic behavior; a
                    // slider only ever gets its head treated as a tap.
                    auto& so = m_smart_objects[ static_cast<size_t>( i ) ];
                    so.start_time = obj.start_time;
                    so.hold = natural_hold > 0;
                    so.fired = false;
                    // Miss Rescue prepares a fallback hit for every upcoming
                    // hittable object; a valid physical press cancels it.
                    so.armed = is_spinner || !m_smart_active || smart_miss_rescue;
                    so.resolved = is_spinner;
                }
                m_last_click_time = press_time;
                m_scheduled_through_idx = i;
            }
        }

        inline double get_time_ms( ) {
            static const auto start = std::chrono::high_resolution_clock::now( );
            return std::chrono::duration<double, std::milli>( std::chrono::high_resolution_clock::now( ) - start ).count( );
        }

        double get_interpolated_game_time( const osu::game_snapshot_t& game ) {
            const double now = get_time_ms( );
            const int gt = game.cur_time;
            
            if ( gt != m_last_audio_time ) {
                m_last_audio_time = gt;
                m_last_audio_sync_time = now;
            }
            
            double elapsed = now - m_last_audio_sync_time;
            const double speed = game.speed_mult > 0.01f ? game.speed_mult : 1.f;
            
            if ( elapsed > 30.0 / speed ) {
                elapsed = 0.0;
            }
            
            return static_cast<double>( gt ) + elapsed * speed;
        }

        void flush_queue( const osu::game_snapshot_t& game ) {
            flush_queue_at( game, get_interpolated_game_time( game ) );
        }

        void flush_queue_at( const osu::game_snapshot_t& game, double est_gt ) {
            if (relax_mode == 2 && !m_autobot_accuracy_mode && hybrid_manual_active()) {
                // Notes due during manual play must never be replayed on takeover.
                live_key(m_smart_k1, false, m_left_down);
                live_key(m_smart_k2, false, m_right_down);
                m_click_queue.erase(std::remove_if(m_click_queue.begin(), m_click_queue.end(),
                    [est_gt](const auto& c) { return c.pressed || c.released || c.press_time <= est_gt; }),
                    m_click_queue.end());
                return;
            }
            if ( m_live_session && live_hit_feedback && !m_smart_active && !m_autobot_accuracy_mode ) {
                flush_live_queue_at( game, est_gt );
                return;
            }
            WORD k1 = 0, k2 = 0;
            resolve_keys( game, k1, k2 );

            for ( auto& c : m_click_queue ) {
                if ( !c.pressed && est_gt >= static_cast<double>( c.press_time ) ) {
                    // In Smart mode a scheduled press only fires once its object
                    // has been armed by an early correction or a miss rescue.
                    if ( m_smart_active && !smart_click_armed( c, est_gt ) )
                        continue;
                    if ( m_smart_active )
                        smart_note_fired( c );
                    bool& ref = ( c.key == k1 ) ? m_left_down : m_right_down;
                    press_key( c.key, ref );
                    c.pressed = true;
                    continue;
                }
                if ( c.pressed && !c.released && est_gt >= static_cast<double>( c.release_time ) ) {
                    bool& ref = ( c.key == k1 ) ? m_left_down : m_right_down;
                    release_key( c.key, ref );
                    c.released = true;
                }
            }

            m_click_queue.erase(
                std::remove_if( m_click_queue.begin( ), m_click_queue.end( ),
                    []( const scheduled_click_t& c ) { return c.released; } ),
                m_click_queue.end( ) );
        }

        bool live_key( WORD key, bool down, bool& state ) {
            if ( state == down ) return true;
#ifdef TENSHIZ_RELAX_TESTING
            if ( m_test_input_sink ) { m_test_input_sink( key, down ); state = down; return true; }
#endif
            INPUT event{};
            event.type = INPUT_KEYBOARD;
            event.ki.wVk = key;
            event.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
            event.ki.dwExtraInfo = 0xDEADC0DE;
            if ( input::send_inputs( 1, &event, sizeof event ) != 1 ) { ++m_live_stats.send_failures; return false; }
            state = down;
            return true;
        }

        void flush_live_queue_at( const osu::game_snapshot_t& game, double ) {
            WORD k1 = 0, k2 = 0;
            resolve_keys( game, k1, k2 );
            const auto& frame = game.live_playfield;
            const int time = game.cur_time;
            if(frame && frame->drawable_ruleset==game.drawable_ruleset && std::abs(time-frame->game_time)<=32)
                m_live_stats.visible_heads=visible_note_heads(*frame);
#ifndef TENSHIZ_RELAX_TESTING
            const HWND hwnd = input::target_window( );
            if ( !hwnd || hwnd != GetForegroundWindow( ) || IsIconic( hwnd ) ) {
                live_key( k1, false, m_left_down ); live_key( k2, false, m_right_down );
                return;
            }
#endif
            if ( !frame || frame->drawable_ruleset != game.drawable_ruleset ||
                 std::abs( time - frame->game_time ) > 32 ) {
                // A transient read failure is not a new key edge. Preserve an
                // existing tap until its release time, just like slider holds.
                const bool active_hold = std::any_of(m_click_queue.begin(),m_click_queue.end(),
                    [time](const auto& c){return c.pressed && !c.released && time < c.release_time;});
                if ( active_hold && GetTickCount64()-m_live_good_wall <= 50 && std::abs(time-m_live_good_time) <= 32 ) return;
                live_key( k1, false, m_left_down ); live_key( k2, false, m_right_down );
                return;
            }
            if ( frame->paused || frame->replay ) {
                live_key(k1,false,m_left_down); live_key(k2,false,m_right_down); return;
            }
            m_live_good_wall = GetTickCount64();
            m_live_good_time = time;
            // Keep duration-based slider holds. Only head decisions use live
            // hover/judgment feedback; no estimated slider-follow geometry.
            for ( auto& c : m_click_queue ) {
                bool density_100 = false;
                const auto source = m_live_sources[ c.object_index ];
                auto it = std::find_if( frame->objects.begin( ), frame->objects.end( ),
                    [source]( const auto& o ) { return o.source_object == source; } );
                // Managed objects may relocate during a long map. A unique
                // beatmap timestamp is a stable identity when a pointer moves.
                if (it==frame->objects.end() && m_live_unique_times[c.object_index])
                    it=std::find_if(frame->objects.begin(),frame->objects.end(),[&](const auto& o){return o.start_time==m_live_start_times[c.object_index];});
                if ( it != frame->objects.end() && it->judged && !c.judgment_observed ) {
                    c.judgment_observed = true;
                    if ( it->hit ) ++m_live_stats.confirmed_hits;
                    else ++m_live_stats.confirmed_misses;
                    if ( it->result >= 5 ) ++m_live_stats.great;
                    else if ( it->result == 3 ) ++m_live_stats.ok;
                    else if ( it->result == 2 ) ++m_live_stats.meh;
                    if (it->result_offset_valid) {
                        m_live_stats.last_result_offset = it->result_offset;
                        ++m_live_stats.result_offset_samples;
                    }
                }
                if ( c.pressed && time >= c.release_time ) c.released = true;
                if ( c.pressed || c.released ) continue;
                if ( it == frame->objects.end( ) ) {
                    if ( time < c.press_time ) continue;
                    // Spinners have no tappable head and retain their old hold.
                    if ( !m_live_spinners[ c.object_index ] ) continue;
                }
                else {
                    if ( it->judged ) {
                        if ( it->slider && it->hit && time < c.release_time ) c.pressed = true;
                        else c.released = true;
                        continue;
                    }
                    if ( time > it->start_time + it->late_window ) { c.released = true; continue; }
                    const int margin = std::max(0,static_cast<int>(it->great_window)-4);
                    int offset = -12 + manual_offset_ms;
                    if ( timing_variation ) {
                        const int early = std::clamp(early_variation_ms,-25,0);
                        const int late = std::clamp(late_variation_ms,0,25);
                        const float sample = c.live_jitter_unit*std::max(ur,0.f)*.1f +
                            c.live_drift_unit*static_cast<float>(std::clamp(timing_drift_ms,0,8));
                        offset = manual_offset_ms + std::clamp(static_cast<int>(std::lround(sample)),early,late);
                    }
                    int target = it->start_time + std::clamp(offset,-margin,margin);
                    if(density_tolerance<100 && it->ok_window>it->great_window) {
                        const float speed=std::clamp(game.speed_mult,.25f,4.f);
                        const float chance=density_100_chance(m_live_stats.visible_heads,density_tolerance);
                        const int early_room=static_cast<int>(std::floor(it->ok_window))-static_cast<int>(std::ceil(4.f*speed));
                        const int early_error=static_cast<int>(std::ceil(it->great_window+10.f*speed));
                        const int latest_early=it->start_time-static_cast<int>(std::ceil(it->great_window+4.f*speed));
                        const bool earlier_pending=std::any_of(frame->objects.begin(),frame->objects.end(),[&](const auto& o){
                            return o.start_time<it->start_time && !o.judged && time<=o.start_time+o.late_window;
                        });
                        // Crowded visible heads can use an early 100 opportunity.
                        // Normal timing and slider-tail holds are never delayed.
                        if(density_roll(c.object_index,it->start_time,m_live_density_seed)<chance && early_error<=early_room &&
                           !earlier_pending && time>=it->start_time-early_error && time<=latest_early) {
                            target=it->start_time-early_error;
                            density_100=true;
                        }
                    }
                    if ( time < target || !it->hovered ) continue;
                }
                if ( time == m_live_last_dispatch ) continue;
                if ( !live_key( k1, false, m_left_down ) || !live_key( k2, false, m_right_down ) ) continue;
                bool& down = c.key == k1 ? m_left_down : m_right_down;
                if ( !live_key( c.key, true, down ) ) continue;
                c.pressed = true;
                // A delayed acquisition must still get a usable ordinary tap.
                c.release_time = std::max( c.release_time, time + 14 );
                m_live_last_dispatch = time;
                ++m_live_stats.presses;
                if(density_100) ++m_live_stats.density_100_presses;
                m_live_stats.last_offset = time - m_live_start_times[c.object_index];
                m_last_hit_obj_idx = std::max( m_last_hit_obj_idx, c.object_index );
                break; // wait for another sampled game time before the next head
            }
            const bool hold = std::any_of( m_click_queue.begin( ), m_click_queue.end( ),
                []( const auto& c ) { return c.pressed && !c.released; } );
            if ( !hold ) {
                live_key( k1, false, m_left_down ); live_key( k2, false, m_right_down );
            }
            else if ( !m_left_down && !m_right_down ) {
                // Only a duration hold may need restoring after a pause or
                // lost focus. Re-arming an ordinary tap hits a different note.
                const auto it = std::find_if( m_click_queue.begin( ), m_click_queue.end( ),
                    []( const auto& c ) { return c.pressed && !c.released && c.protected_hold; } );
                if ( it != m_click_queue.end() ) {
                    bool& down = it->key == k1 ? m_left_down : m_right_down;
                    live_key( it->key, true, down );
                }
            }
            m_click_queue.erase( std::remove_if( m_click_queue.begin( ), m_click_queue.end( ),
                []( const auto& c ) { return c.released; } ), m_click_queue.end( ) );
        }

    };

}





