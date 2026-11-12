#pragma once

#include <core/autobot/motion_core.hxx>
#include <impl/struct/game_snapshot.hxx>
#include <limits>

namespace relax {

    // Retired experiment; not used by production Relax. Retained only for
    // historical regression tests. The production path is in relax.hxx.
    // External adaptation of the rules documented by ppy/osu's OsuModRelax:
    // https://github.com/ppy/osu/blob/2026.921.0-lazer/osu.Game.Rulesets.Osu/Mods/OsuModRelax.cs
    // Unlike the in-process mod, we only know that a press was emitted, not
    // whether the game judged it. Cursor geometry uses the existing projection.
    class normal_relax_t {
    public:
        struct output_t {
            bool release = false;
            bool press = false;
            int key_index = 1;
            int object_index = -1;
        };

        void reset( ) { *this = normal_relax_t{}; }
        void suspend( ) {
            m_down = false;
            for ( auto& state : m_objects ) state.tracking = false;
        }
        bool down( ) const { return m_down; }
        int last_object( ) const { return m_last_object; }

        output_t update( const osu::game_snapshot_t& game, const osu::beatmap_data_t& map,
                         float cursor_x, float cursor_y ) {
            output_t out{};
            const double time = game.cur_time;
            if ( m_objects.size( ) != map.objects.size( ) || time < m_last_time ) {
                out.release = m_down;
                reset( );
                m_objects.resize( map.objects.size( ) );
            }
            m_last_time = time;
            const bool hr = ( game.cur_mod_state & 16 ) != 0 && !map.hr;
            const bool ez = ( game.cur_mod_state & 2 ) != 0 && !map.ez;
            const float cs = hr ? std::min( 10.f, map.cs * 1.3f ) : ez ? map.cs * .5f : map.cs;
            const float od = hr ? std::min( 10.f, map.od * 1.4f ) : ez ? map.od * .5f : map.od;
            const float radius = std::max( 4.f, 54.4f - 4.48f * cs );
            const double late_window = std::floor( 200.0 - 10.0 * od ) - .5;
            const autobot::point_t cursor{ cursor_x, cursor_y };
            bool hold = false;
            int hit = -1;

            // Skip completed objects without making a long slider block later notes.
            while ( m_first < map.objects.size( ) ) {
                const auto& o = map.objects[ m_first ];
                const bool duration = ( o.type & ( osu::slider | osu::spinner ) ) != 0;
                if ( duration ? time > o.end_time :
                     ( m_objects[ m_first ].pressed || time > o.start_time + late_window ) )
                    ++m_first;
                else break;
            }

            for ( size_t i = m_first; i < map.objects.size( ); ++i ) {
                const auto& o = map.objects[ i ];
                if ( time < o.start_time - 12.0 ) break;
                auto& state = m_objects[ i ];
                const bool slider = ( o.type & osu::slider ) != 0;
                const bool spinner = ( o.type & osu::spinner ) != 0;
                if ( ( slider || spinner ) && time > o.end_time ) continue;
                if ( spinner ) {
                    hold = true;
                    continue;
                }

                // Match the projection already used for the decoded beatmap.
                const float stack = -6.f * o.stack_index;
                const autobot::point_t head{ o.x + stack, ( hr ? 384.f - o.y : o.y ) + stack };
                if ( hit < 0 && time > m_last_hit_time && !state.pressed && time <= o.start_time + late_window &&
                     autobot::length_sq( cursor - head ) <= radius * radius )
                    hit = static_cast<int>( i );

                if ( slider ) {
                    if ( !state.path_ready ) {
                        state.path = autobot::build_slider_path( o, hr );
                        state.path_ready = true;
                    }
                    if ( state.path.empty( ) ) {
                        // With no geometry we cannot infer the moving follow
                        // area from the stationary head. Retain an acquired
                        // slider through its known tail rather than dropping it.
                        state.tracking = state.pressed || hit == static_cast<int>( i );
                    }
                    else {
                        const auto ball = autobot::eval_slider_ball( state.path, o, game.cur_time ) +
                            autobot::point_t{ stack, stack };
                        const float follow = radius * ( state.tracking ? 2.4f : 1.f );
                        state.tracking = autobot::length_sq( cursor - ball ) <= follow * follow;
                    }
                    hold |= state.tracking;
                }
            }

            if ( hit >= 0 ) {
                // A new edge is mandatory even while holding an overlapping slider.
                out.release |= m_down;
                m_down = false;
                m_objects[ hit ].pressed = true;
                m_last_object = hit;
                m_last_hit_time = time;
                out.object_index = hit;
            }
            if ( hit >= 0 || hold ) {
                if ( !m_down ) {
                    out.press = true;
                    out.key_index = m_next_key;
                    m_next_key ^= 1;
                    m_down = true;
                    m_last_press = time;
                }
            }
            else if ( m_down && time - m_last_press > 50.0 ) {
                out.release = true;
                m_down = false;
            }
            return out;
        }

    private:
        struct object_state_t {
            bool pressed = false;
            bool tracking = false;
            bool path_ready = false;
            autobot::slider_path_t path;
        };
        std::vector<object_state_t> m_objects;
        size_t m_first = 0;
        bool m_down = false;
        int m_next_key = 1;
        int m_last_object = -1;
        double m_last_press = 0;
        // The memory poll can run many times within one game-clock sample.
        // Do not dump several heads into the game's input queue at that time.
        double m_last_hit_time = -std::numeric_limits<double>::infinity( );
        double m_last_time = -std::numeric_limits<double>::infinity( );
    };
}
