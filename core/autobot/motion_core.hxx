#pragma once

// ============================================================================
//  motion_core.hxx  --  Autobot movement engine (pure, host-independent).
//
//  This header contains the WHOLE movement mathematics of the Autobot:
//
//      MotionState  +  MovementPlan  +  current time  =  next cursor position.
//
//  It intentionally depends only on <impl/struct/osu_types.hxx> and the C++
//  standard library -- there is no Windows, no cursor output, no Relax, no
//  playfield projection in here.  That makes the engine deterministic and
//  unit-testable off-line (see tests/motion_tests.cpp).  The Windows / Relax /
//  projection integration lives in autobot.hxx, which owns exactly one
//  motion_engine_t and feeds it snapshots.
//
//  Design invariants (enforced here and by the tests):
//    * There is ONE MotionState and ONE active MovementPlan.
//    * A new plan always begins from the CURRENT MotionState (p, v, a).
//    * A plan is only rebuilt on a meaningful identity change, never per frame.
//    * Acquisition retains exact timed p/v/a endpoints. Hidden targets cannot
//      influence motion; visible intent commits coherently. Sliders retain safe
//      hand regions; spinner angle integrates bounded real-time RPM and one
//      authoritative session direction.
//    * Every produced position is finite and its per-frame displacement is
//      bounded by the kinematic budget; a violation is reported, not hidden.
// ============================================================================

#include <impl/struct/osu_types.hxx>
#include <core/autobot/spinner_settings.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace autobot {

    constexpr float k_pi = 3.14159265358979323846f;

    // ---- 2D vector --------------------------------------------------------
    struct point_t { float x = 0.f; float y = 0.f; };
    inline point_t operator+( point_t a, point_t b ) { return { a.x + b.x, a.y + b.y }; }
    inline point_t operator-( point_t a, point_t b ) { return { a.x - b.x, a.y - b.y }; }
    inline point_t operator*( point_t a, float s ) { return { a.x * s, a.y * s }; }
    inline point_t operator/( point_t a, float s ) { return s != 0.f ? point_t{ a.x / s, a.y / s } : point_t{}; }
    inline point_t& operator+=( point_t& a, point_t b ) { a.x += b.x; a.y += b.y; return a; }
    inline float dot( point_t a, point_t b ) { return a.x * b.x + a.y * b.y; }
    inline float length_sq( point_t a ) { return dot( a, a ); }
    inline float length( point_t a ) { return std::sqrt( length_sq( a ) ); }
    inline point_t normalized( point_t a ) { const float n = length( a ); return n > 1e-4f ? a / n : point_t{}; }
    inline point_t limit( point_t a, float m ) { const float n = length( a ); return ( n > m && n > 1e-4f ) ? a * ( m / n ) : a; }
    inline bool finite( point_t a ) { return std::isfinite( a.x ) && std::isfinite( a.y ); }
    inline float saturate( float v ) { return std::clamp( v, 0.f, 1.f ); }
    inline float smoothstep( float a, float b, float x ) {
        if ( b <= a ) return x >= b ? 1.f : 0.f;
        const float t = saturate( ( x - a ) / ( b - a ) );
        return t * t * ( 3.f - 2.f * t );
    }
    // Real controller time: retain the stall cap, but never manufacture a
    // millisecond on high-rate callbacks.  Artificial minimum dt accumulated
    // spinner phase faster than wall-clock time on uncapped clients.
    inline float sanitize_controller_dt( float real_dt ) {
        if ( !std::isfinite( real_dt ) || real_dt <= 0.f ) return 0.000001f;
        return std::min( real_dt, 0.025f );
    }

    inline uint32_t mix_seed( uint32_t x ) {
        x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
        return x ? x : 0x9e3779b9u;
    }

    // ---- MotionState : the single authoritative cursor state --------------
    struct motion_state_t {
        point_t position{};
        point_t velocity{};
        point_t acceleration{};
        bool initialized = false;
    };

    // ---- MovementPlan : the single authoritative movement intent ----------
    //
    // Exactly one plan owns the cursor at a time.  A trajectory plan (gameplay
    // / recovery / decorative segment / slider approach) is a timed quintic
    // from (p0,v0,a0) to (p1,v1,0).  Slider-follow and spinner plans are
    // evaluated procedurally against geometry but still carry their captured
    // entry state so the transition in is continuous.
    enum class plan_type_t : uint8_t {
        none, gameplay, recovery, slider, spinner, startup, break_decor, micro_gap
    };

    inline const char* plan_type_name( plan_type_t t ) {
        switch ( t ) {
        case plan_type_t::gameplay:    return "GAMEPLAY";
        case plan_type_t::recovery:    return "RECOVERY";
        case plan_type_t::slider:      return "SLIDER";
        case plan_type_t::spinner:     return "SPINNER";
        case plan_type_t::startup:     return "STARTUP";
        case plan_type_t::break_decor: return "BREAK";
        case plan_type_t::micro_gap:   return "MICRO";
        default:                       return "NONE";
        }
    }

    enum class decor_motif_t : uint8_t {
        uneven_circle, irregular_figure_eight, wide_sweep, lazy_loop, broad_wave,
        small_fidget, medium_fidget, loose_fidget, directional_wiggle, curved_fidget, count
    };

    inline const char* decor_motif_name( decor_motif_t m ) {
        switch ( m ) {
        case decor_motif_t::uneven_circle:         return "uneven-circle";
        case decor_motif_t::irregular_figure_eight:return "irregular-eight";
        case decor_motif_t::wide_sweep:            return "wide-sweep";
        case decor_motif_t::lazy_loop:             return "lazy-loop";
        case decor_motif_t::broad_wave:            return "broad-wave";
        case decor_motif_t::small_fidget:          return "small-fidget";
        case decor_motif_t::medium_fidget:         return "medium-fidget";
        case decor_motif_t::loose_fidget:          return "loose-fidget";
        case decor_motif_t::directional_wiggle:    return "directional-wiggle";
        case decor_motif_t::curved_fidget:         return "curved-fidget";
        default:                              return "none";
        }
    }

    inline bool decor_is_dance( decor_motif_t m ) {
        return static_cast<int>( m ) >= 0 && static_cast<int>( m ) < 5;
    }

    enum class slider_strategy_t : uint8_t { track, blend, anchor };
    inline const char* slider_strategy_name( slider_strategy_t s ) {
        switch ( s ) {
        case slider_strategy_t::anchor: return "ANCHOR";
        case slider_strategy_t::blend:  return "BLEND";
        default:                        return "TRACK";
        }
    }

    enum class slider_exit_strategy_t : uint8_t { aligned_carry, direct_peel, curved_departure, reversal_brake };
    inline const char* slider_exit_strategy_name( slider_exit_strategy_t s ) {
        switch ( s ) {
        case slider_exit_strategy_t::aligned_carry:  return "ALIGNED CARRY";
        case slider_exit_strategy_t::direct_peel:    return "DIRECT PEEL";
        case slider_exit_strategy_t::reversal_brake: return "REVERSAL BRAKE";
        default:                                      return "CURVED DEPARTURE";
        }
    }

    enum class micro_gesture_t : uint8_t { none, irregular_shake, curved_wiggle, small_loop, gentle_drift, diagonal_move, small_oval };
    inline const char* micro_gesture_name( micro_gesture_t g ) {
        switch ( g ) {
        case micro_gesture_t::irregular_shake: return "irregular shake";
        case micro_gesture_t::curved_wiggle:   return "curved wiggle";
        case micro_gesture_t::small_loop:      return "small loop";
        case micro_gesture_t::gentle_drift:    return "gentle drift";
        case micro_gesture_t::diagonal_move:   return "diagonal move";
        case micro_gesture_t::small_oval:      return "small oval";
        default:                               return "none";
        }
    }

    struct plan_t {
        uint64_t id = 0;
        plan_type_t type = plan_type_t::none;
        int object_index = -1;      // gameplay/slider/spinner target; -1 for decorative
        bool valid = false;

        // control-time window (game milliseconds)
        double t_start = 0.0;
        double t_end = 0.0;         // scheduled arrival / segment end

        // captured initial state (taken from the CURRENT MotionState)
        point_t p0{}, v0{}, a0{};
        // trajectory endpoint (p1 = target, v1 = desired arrival velocity)
        point_t p1{}, v1{}, a1{};

        // slider-follow blend: offset between the incoming cursor and the ball
        point_t entry_offset{};
        point_t slider_anchor{}, slider_offset{};
        float slider_safe_radius = 0.f;
        float slider_anchor_blend = 0.f;
        float slider_local_speed = 0.f;
        float slider_anchor_max_distance = 0.f;
        bool compact_slider = false;
        bool ultra_short_slider = false;
        float slider_ball_path_length = 0.f;
        bool slider_whole_hold = false;
        float slider_hand_allowance = 0.f, slider_motion_pressure = 0.f, slider_economy = 0.f;
        bool slider_exit_entry = false;
        float entry_response_seconds = 0.f;
        bool waiting_for_perception = false;
        float continuation_response = 0.12f;
        float initial_commitment = 1.f, commitment_softness = 0.f;
        bool spinner_entry = false;

        // Optional safe deviation of the ONE gameplay acquisition trajectory.
        // The sixth-order bump and its first two derivatives are zero at both
        // endpoints, so it never creates a second destination or settle trip.
        bool micro_modifier = false;
        point_t micro_direction{};
        float micro_amplitude = 0.f;
        micro_gesture_t micro_gesture = micro_gesture_t::none;

        float max_speed = 1800.f;
        float max_accel = 14000.f;

        // plan identity used for reconciliation (see needs_replan).
        double arrival_key = 0.0;   // quantised scheduled arrival
    };

    // ---- quintic (minimum-jerk-style) Hermite evaluation ------------------
    //
    // Given endpoints and a duration, produces a C2-continuous curve with an
    // exact timed arrival.  `u` is normalised time in [0,1]; the duration is
    // needed to scale the velocity / acceleration terms back to real units.
    struct kinematic_t { point_t position, velocity, acceleration; };

    inline kinematic_t eval_quintic( const point_t& p0, const point_t& v0, const point_t& a0,
                                     const point_t& p1, const point_t& v1, const point_t& a1,
                                     float duration, float u ) {
        kinematic_t k{ p0, v0, a0 };
        if ( duration <= 1e-5f ) { k.position = p1; k.velocity = v1; k.acceleration = a1; return k; }
        u = saturate( u );
        const point_t c0 = p0;
        const point_t c1 = v0 * duration;
        const point_t c2 = a0 * ( 0.5f * duration * duration );
        const point_t d  = p1 - c0 - c1 - c2;
        const point_t dv = v1 * duration - c1 - c2 * 2.f;
        const point_t da = a1 * ( duration * duration ) - c2 * 2.f;
        const point_t c3 = d * 10.f - dv * 4.f + da * 0.5f;
        const point_t c4 = d * -15.f + dv * 7.f - da;
        const point_t c5 = d * 6.f - dv * 3.f + da * 0.5f;
        const float u2 = u * u, u3 = u2 * u, u4 = u3 * u, u5 = u4 * u;
        k.position = c0 + c1 * u + c2 * u2 + c3 * u3 + c4 * u4 + c5 * u5;
        k.velocity = ( c1 + c2 * ( 2.f * u ) + c3 * ( 3.f * u2 ) + c4 * ( 4.f * u3 ) + c5 * ( 5.f * u4 ) ) / duration;
        k.acceleration = ( c2 * 2.f + c3 * ( 6.f * u ) + c4 * ( 12.f * u2 ) + c5 * ( 20.f * u3 ) ) / ( duration * duration );
        return k;
    }

    inline point_t critically_damped_position( point_t position, point_t velocity, point_t target,
                                                float dt, float response_seconds ) {
        const float omega = 2.f / std::max( response_seconds, 0.012f );
        const float decay = std::exp( -omega * std::max( dt, 0.f ) );
        const point_t error = position - target;
        const point_t coefficient = velocity + error * omega;
        return target + ( error + coefficient * dt ) * decay;
    }

    inline kinematic_t eval_acquisition_curve( const plan_t& p, float duration, float u ) {
        if(p.spinner_entry) {
            plan_t base=p;base.spinner_entry=false;base.v1={};base.a1={};
            kinematic_t k=eval_acquisition_curve(base,duration,u);
            const float response=std::max(0.001f,std::min(0.12f,duration*0.4f));
            const float q=std::max(0.f,(1.f-u)*duration/response);
            if(q>=1.f) return k;
            const float q2=q*q,q3=q2*q,q4=q3*q,q5=q4*q;
            const float fv=q-6*q3+8*q4-3*q5,fv1=1-18*q2+32*q3-15*q4,fv2=-36*q+96*q2-60*q3;
            const float fa=.5f*(q2-3*q3+3*q4-q5),fa1=q-4.5f*q2+6*q3-2.5f*q4,fa2=1-9*q+18*q2-10*q3;
            k.position+=p.v1*(-response*fv)+p.a1*(response*response*fa);
            k.velocity+=p.v1*fv1-p.a1*(response*fa1);
            k.acceleration+=p.v1*(-fv2/response)+p.a1*fa2;
            return k;
        }
        if ( !p.slider_exit_entry ) {
            kinematic_t k = eval_quintic(p.p0,p.v0,p.a0,p.p1,p.v1,p.a1,duration,u);
            if ( p.commitment_softness <= 0.f || duration <= 0.001f ) return k;
            // Soft commitment is a fixed endpoint-jerk profile in this same
            // trajectory, not delayed input, a moving target, or output filtering.
            // Both bases have identical p/v/a endpoints. Urgent acquisitions use
            // the original quintic unchanged; spare-time perception can ease in.
            const point_t c1=p.v0*duration,c2=p.a0*(0.5f*duration*duration);
            const point_t d=p.p1-p.p0-c1-c2,dv=p.v1*duration-c1-c2*2.f;
            const point_t da=p.a1*(duration*duration)-c2*2.f;
            const point_t c3=d*10.f-dv*4.f+da*0.5f,c4=d*-15.f+dv*7.f-da;
            const point_t c5=d*6.f-dv*3.f+da*0.5f;
            const point_t j0=c3*6.f,j1=c3*6.f+c4*24.f+c5*60.f;
            const float q=saturate(u),q2=q*q,q3=q2*q,q4=q3*q,q5=q4*q,q6=q5*q,q7=q6*q;
            const float h0=(q3-4*q4+6*q5-4*q6+q7)/6.f,h1=(-q4+3*q5-3*q6+q7)/6.f;
            const float d0=(3*q2-16*q3+30*q4-24*q5+7*q6)/6.f,d1=(-4*q3+15*q4-18*q5+7*q6)/6.f;
            const float a0=(6*q-48*q2+120*q3-120*q4+42*q5)/6.f,a1=(-12*q2+60*q3-90*q4+42*q5)/6.f;
            k.position += (j0*h0+j1*h1)*-p.commitment_softness;
            k.velocity += (j0*d0+j1*d1)*(-p.commitment_softness/duration);
            k.acceleration += (j0*a0+j1*a1)*(-p.commitment_softness/(duration*duration));
            return k;
        }
        // A slider/spinner can require high terminal derivatives even though the next
        // circle has hundreds of milliseconds of slack. A single long quintic
        // spreads those derivatives across that entire interval (a0*T^2), making
        // a large excursion. Transfer them over a short physical response inside
        // the SAME acquisition curve instead. Both endpoint jets remain exact.
        kinematic_t k = eval_quintic(p.p0,{},{},p.p1,p.v1,p.a1,duration,u);
        const float response = std::max(0.001f,std::min(p.entry_response_seconds,duration*0.40f));
        const float local = u*duration/response;
        if ( local >= 1.f ) return k;
        const float q = saturate(local), q2=q*q, q3=q2*q, q4=q3*q, q5=q4*q;
        const float fv=q-6.f*q3+8.f*q4-3.f*q5;
        const float fv1=1.f-18.f*q2+32.f*q3-15.f*q4;
        const float fv2=-36.f*q+96.f*q2-60.f*q3;
        const float fa=0.5f*(q2-3.f*q3+3.f*q4-q5);
        const float fa1=q-4.5f*q2+6.f*q3-2.5f*q4;
        const float fa2=1.f-9.f*q+18.f*q2-10.f*q3;
        k.position += p.v0*(response*fv)+p.a0*(response*response*fa);
        k.velocity += p.v0*fv1+p.a0*(response*fa1);
        k.acceleration += p.v0*(fv2/response)+p.a0*fa2;
        return k;
    }

    // Closest-feasible-region approximation, not a ball/centre target. If the
    // current hand anchor already covers every sample it is returned unchanged.
    // Projection onto convex disks is deterministic and needs no random input.
    inline bool project_to_follow_region( point_t& candidate, const point_t* centers,
                                         size_t count, float radius ) {
        if ( count == 0 || radius <= 0.f ) return false;
        for ( int pass = 0; pass < 8; ++pass ) {
            bool changed = false;
            for ( size_t i = 0; i < count; ++i ) {
                const point_t delta = candidate - centers[i];
                if ( length_sq(delta) > radius * radius ) {
                    candidate = centers[i] + limit(delta, radius); changed = true;
                }
            }
            if ( !changed ) return true;
        }
        for ( size_t i = 0; i < count; ++i )
            if ( length(candidate-centers[i]) > radius + 0.05f ) return false;
        return true;
    }

    // ---- object classification (shared with the schedule) -----------------
    inline bool is_slider( const osu::hit_object_t& o ) {
        return ( o.type & static_cast<uint8_t>( osu::hit_object_type_t::slider ) ) != 0;
    }
    inline bool is_spinner( const osu::hit_object_t& o ) {
        if ( o.type & static_cast<uint8_t>( osu::hit_object_type_t::spinner ) ) return true;
        // Some maps encode a spinner as a zero-length slider near the centre.
        if ( !is_slider( o ) || !o.slider_curve_str.empty( ) || o.slider_length > 0.f ) return false;
        const float dx = o.x - 256.f, dy = o.y - 192.f;
        return o.end_time - o.start_time >= 400 && dx * dx + dy * dy < 48.f * 48.f;
    }

    // ---- deterministic per-object interior target point -------------------
    //
    // One point per object, chosen once and cached by the caller.  Depends only
    // on the object, a session seed, and the aim-spread / arrival-depth style,
    // so it is stable for the whole session (never rerolls on replan).
    inline point_t object_center( const osu::hit_object_t& o, bool hr_flip ) {
        return { o.x, hr_flip ? 384.f - o.y : o.y };
    }
    inline float hit_radius( float cs, bool hr_flip ) {
        if ( hr_flip ) cs = std::min( cs * 1.3f, 10.f );
        return std::max( 8.f, 54.4f - 4.48f * cs );
    }

    // osu! AR difficulty range, in GAME milliseconds, matching OsuHitObject's
    // DifficultyRangeInt(AR, 1800, 1200, 450). Speed mods change the wall-clock
    // duration, not the object start time or this comparison. Parsed AR already
    // includes HR/EZ; only apply HR when the host explicitly reports it unapplied.
    inline int object_preempt_ms( float ar, bool unapplied_hr = false ) {
        if ( !std::isfinite(ar) ) ar=5.f;
        if ( unapplied_hr ) ar=std::min(ar*1.4f,10.f);
        return std::max(0,static_cast<int>(ar<=5.f ? 1800.f-120.f*ar : 1200.f-150.f*(ar-5.f)));
    }
    inline point_t select_target_point( int index, const osu::hit_object_t& o, bool hr_flip,
                                         float cs, uint32_t seed, float aim_spread,
                                         float arrival_depth, float phase, float edge_bias,
                                         float pattern_scale = 1.f ) {
        const point_t center = object_center( o, hr_flip );
        if ( is_spinner( o ) )
            return center;      // spinner target handled by the orbit; centre is a safe stand-in
        if ( is_slider( o ) )
            return center;      // slider head is the object centre
        const uint32_t h0 = mix_seed( static_cast<uint32_t>( index + 1 ) * 747796405u ^ seed );
        const uint32_t h1 = mix_seed( h0 ^ 0x9e3779b9u );
        const float u = static_cast<float>( h0 & 0x00ffffffu ) / 16777216.f;
        const float v = static_cast<float>( h1 & 0x00ffffffu ) / 16777216.f;
        const float radius = hit_radius( cs, hr_flip );
        if ( edge_bias > 0.f ) {
            const float frac = edge_bias * ( 0.92f + 0.08f * std::sqrt( u ) );
            const float r = radius * std::clamp( frac, 0.f, 0.95f );
            const float ang = v * 2.f * k_pi + phase;
            return center + point_t{ std::cos( ang ), std::sin( ang ) } * r;
        }

        // Stable 2D disk sample plus a small slowly moving session hand bias.
        // pow(u,.72) is safer/interior-biased relative to a uniform-area disk,
        // while still producing visibly different hit locations.
        pattern_scale = std::clamp( pattern_scale, 0.45f, 1.f );
        const float max_fraction = ( 0.30f + 0.26f * std::clamp( aim_spread, 0.f, 1.f ) ) *
            pattern_scale * ( 0.94f + 0.06f * std::clamp( arrival_depth, 0.f, 1.f ) );
        const float object_fraction = max_fraction * std::pow( u, 0.72f );
        const float object_angle = v * 2.f * k_pi + phase * 0.31f;
        point_t offset = point_t{ std::cos( object_angle ), std::sin( object_angle ) } *
            ( radius * object_fraction * 0.95f );
        const float bias_angle = phase + static_cast<float>( o.start_time ) * 0.00034f;
        const float bias_fraction = ( 0.030f + 0.035f * std::clamp( aim_spread, 0.f, 1.f ) ) * pattern_scale;
        offset += point_t{ std::cos( bias_angle ), std::sin( bias_angle ) } * ( radius * bias_fraction );
        offset = limit( offset, radius * max_fraction );
        return center + offset;
    }

    // ---- acquisition lead : ms needed to travel to a target and settle -----
    inline float acquisition_lead_ms( float distance, float accel_style ) {
        const float accel_time = 2000.f * std::sqrt( std::max( distance, 0.f ) / ( 18000.f * std::max( accel_style, 0.2f ) ) );
        const float travel_time = distance * 1000.f / 1500.f;
        return std::clamp( std::max( accel_time, travel_time ) + 85.f, 160.f, 1400.f );
    }

    // Acquisition time must be evaluated from the state that will actually
    // begin the move.  Distance alone understates the cost of first cancelling
    // lateral/away velocity and acceleration left by a short decorative plan.
    inline float acquisition_required_ms( point_t position, point_t velocity, point_t acceleration,
                                           point_t target, float accel_style ) {
        const point_t delta = target - position;
        const float distance = length( delta );
        const point_t direction = distance > 0.01f ? delta / distance : point_t{};
        const float along_speed = dot( velocity, direction );
        const float lateral_speed = length( velocity - direction * along_speed );
        const float along_accel = dot( acceleration, direction );
        const float lateral_accel = length( acceleration - direction * along_accel );
        const float braking = std::max( -along_speed, 0.f ) * 0.10f + lateral_speed * 0.025f +
                              std::max( -along_accel, 0.f ) * 0.0030f + lateral_accel * 0.0008f;
        return std::clamp( acquisition_lead_ms( distance, accel_style ) + braking, 160.f, 1800.f );
    }

    // ---- slider geometry (bezier / catmull / perfect arc / linear) --------
    //
    // Pure geometry over the object's control-point string.  Built once and
    // cached by the caller; identical to the historical (well-tested) path
    // sampler, kept because it is correct and cleanly separable.
    struct slider_path_t {
        int start_time = -1;
        std::vector<point_t> path;
        std::vector<float> cumulative;
        float total = 0.f;
        bool empty( ) const { return path.size( ) < 2 || total <= 0.f; }
    };

    inline point_t eval_bezier( const std::vector<point_t>& c, float t ) {
        if ( c.empty( ) ) return {};
        if ( c.size( ) == 2 ) return c[ 0 ] + ( c[ 1 ] - c[ 0 ] ) * t;
        std::vector<point_t> w = c;
        for ( size_t step = 1; step < w.size( ); ++step )
            for ( size_t i = 0; i + step < w.size( ); ++i )
                w[ i ] = w[ i ] + ( w[ i + 1 ] - w[ i ] ) * t;
        return w[ 0 ];
    }
    inline bool same_point( point_t a, point_t b ) { return length_sq( a - b ) < 1e-4f; }
    inline void append_bezier( std::vector<point_t>& path, const std::vector<point_t>& c ) {
        if ( c.size( ) < 2 ) return;
        const int n = std::clamp( static_cast<int>( c.size( ) * 18 ), 18, 96 );
        for ( int i = path.empty( ) ? 0 : 1; i <= n; ++i )
            path.push_back( eval_bezier( c, static_cast<float>( i ) / n ) );
    }
    inline bool append_perfect( std::vector<point_t>& path, const std::vector<point_t>& c ) {
        if ( c.size( ) != 3 ) return false;
        const point_t a = c[ 0 ], b = c[ 1 ], d = c[ 2 ];
        const float det = 2.f * ( a.x * ( b.y - d.y ) + b.x * ( d.y - a.y ) + d.x * ( a.y - b.y ) );
        if ( std::abs( det ) < 1e-3f ) return false;
        const float aa = length_sq( a ), bb = length_sq( b ), cc = length_sq( d );
        const point_t ctr{ ( aa * ( b.y - d.y ) + bb * ( d.y - a.y ) + cc * ( a.y - b.y ) ) / det,
                           ( aa * ( d.x - b.x ) + bb * ( a.x - d.x ) + cc * ( b.x - a.x ) ) / det };
        const float s = std::atan2( a.y - ctr.y, a.x - ctr.x );
        const float m = std::atan2( b.y - ctr.y, b.x - ctr.x );
        const float f = std::atan2( d.y - ctr.y, d.x - ctr.x );
        auto pos = []( float x ) { while ( x < 0.f ) x += 2.f * k_pi; while ( x >= 2.f * k_pi ) x -= 2.f * k_pi; return x; };
        const float cf = pos( f - s ), cm = pos( m - s );
        const float sweep = cm <= cf ? cf : cf - 2.f * k_pi;
        const float r = length( a - ctr );
        const int n = std::clamp( static_cast<int>( std::abs( sweep ) * r / 5.f ), 18, 128 );
        for ( int i = 0; i <= n; ++i ) {
            const float ang = s + sweep * static_cast<float>( i ) / n;
            path.push_back( ctr + point_t{ std::cos( ang ), std::sin( ang ) } * r );
        }
        return true;
    }
    inline void append_catmull( std::vector<point_t>& path, const std::vector<point_t>& c ) {
        if ( c.size( ) < 2 ) return;
        for ( size_t s = 0; s + 1 < c.size( ); ++s ) {
            const point_t p0 = s > 0 ? c[ s - 1 ] : c[ s ];
            const point_t p1 = c[ s ], p2 = c[ s + 1 ];
            const point_t p3 = s + 2 < c.size( ) ? c[ s + 2 ] : p2;
            constexpr int n = 20;
            for ( int i = ( path.empty( ) ? 0 : 1 ); i <= n; ++i ) {
                const float t = static_cast<float>( i ) / n, t2 = t * t, t3 = t2 * t;
                path.push_back( ( p1 * 2.f + ( p2 - p0 ) * t + ( p0 * 2.f - p1 * 5.f + p2 * 4.f - p3 ) * t2 +
                                  ( p0 * -1.f + p1 * 3.f - p2 * 3.f + p3 ) * t3 ) * 0.5f );
            }
        }
    }
    inline std::vector<point_t> build_slider_controls_path( const osu::hit_object_t& o, bool hr_flip ) {
        std::vector<point_t> c;
        c.push_back( object_center( o, hr_flip ) );
        char type = 'L';
        if ( !o.slider_curve_str.empty( ) ) {
            std::stringstream ss( o.slider_curve_str );
            std::string item;
            bool first = true;
            while ( std::getline( ss, item, '|' ) ) {
                if ( first ) { if ( !item.empty( ) ) type = item[ 0 ]; first = false; continue; }
                const size_t colon = item.find( ':' );
                if ( colon == std::string::npos ) continue;
                try {
                    point_t p{ std::stof( item.substr( 0, colon ) ), std::stof( item.substr( colon + 1 ) ) };
                    if ( hr_flip ) p.y = 384.f - p.y;
                    c.push_back( p );
                } catch ( ... ) {}
            }
        }
        if ( c.size( ) < 2 || type == 'L' ) return c;
        std::vector<point_t> path; path.reserve( 128 );
        if ( type == 'P' && append_perfect( path, c ) ) return path;
        if ( type == 'C' ) { append_catmull( path, c ); return path; }
        size_t seg = 0;
        for ( size_t i = 1; i + 1 < c.size( ); ++i )
            if ( same_point( c[ i ], c[ i + 1 ] ) ) {
                append_bezier( path, std::vector<point_t>( c.begin( ) + seg, c.begin( ) + i + 1 ) );
                seg = i + 1;
            }
        append_bezier( path, std::vector<point_t>( c.begin( ) + seg, c.end( ) ) );
        return path;
    }
    inline slider_path_t build_slider_path( const osu::hit_object_t& o, bool hr_flip ) {
        slider_path_t s;
        s.start_time = o.start_time;
        s.path = build_slider_controls_path( o, hr_flip );
        if ( s.path.size( ) >= 2 && o.slider_length > 0.f ) {
            std::vector<point_t> trimmed;
            trimmed.push_back( s.path.front( ) );
            float covered = 0.f;
            for ( size_t i = 1; i < s.path.size( ) && covered < o.slider_length; ++i ) {
                const float seg = length( s.path[ i ] - s.path[ i - 1 ] );
                if ( covered + seg >= o.slider_length ) {
                    const float u = seg > 0.f ? ( o.slider_length - covered ) / seg : 0.f;
                    trimmed.push_back( s.path[ i - 1 ] + ( s.path[ i ] - s.path[ i - 1 ] ) * u );
                    covered = o.slider_length; break;
                }
                trimmed.push_back( s.path[ i ] ); covered += seg;
            }
            if ( covered < o.slider_length && trimmed.size( ) >= 2 ) {
                const point_t dir = normalized( trimmed.back( ) - trimmed[ trimmed.size( ) - 2 ] );
                trimmed.push_back( trimmed.back( ) + dir * ( o.slider_length - covered ) );
            }
            s.path = std::move( trimmed );
        }
        s.cumulative.push_back( 0.f );
        for ( size_t i = 1; i < s.path.size( ); ++i ) {
            s.total += length( s.path[ i ] - s.path[ i - 1 ] );
            s.cumulative.push_back( s.total );
        }
        return s;
    }
    // Ball position at game time gt (handles repeats / reversals).
    inline point_t eval_slider_ball( const slider_path_t& s, const osu::hit_object_t& o, int gt ) {
        if ( s.empty( ) ) return object_center( o, false );
        const float dur = static_cast<float>( std::max( o.end_time - o.start_time, 1 ) );
        const float t = saturate( static_cast<float>( gt - o.start_time ) / dur );
        const int spans = std::max( o.slider_repeat, 1 );
        const float sp = t * spans;
        int span = static_cast<int>( sp );
        float local = sp - span;
        if ( span >= spans ) local = ( spans % 2 == 1 ) ? 1.f : 0.f;
        else if ( span % 2 == 1 ) local = 1.f - local;
        const float target = local * s.total;
        for ( size_t i = 1; i < s.path.size( ); ++i )
            if ( target <= s.cumulative[ i ] ) {
                const float seg = s.cumulative[ i ] - s.cumulative[ i - 1 ];
                const float u = seg > 0.f ? ( target - s.cumulative[ i - 1 ] ) / seg : 0.f;
                return s.path[ i - 1 ] + ( s.path[ i ] - s.path[ i - 1 ] ) * u;
            }
        return s.path.back( );
    }

    // ---- spinner integrator ----------------------------------------------
    //
    // Angle / angular-velocity / radius state that begins from the incoming
    // motion (tangent direction + current radius) and ramps toward the target
    // RPM.  Never snaps to the centre or to full speed.
    // Immutable per-spinner character; no frame-time RNG.
    struct spinner_style_t {
        bool valid=false;
        spinner_rpm_range_t range{};
        float mean_radius=70.f,axis_ratio=1.3f,rotation=0.f,rotation_drift=0.f;
        point_t center_bias{},drift_direction{};
        float asymmetry=0.015f,phase=0.f,knot_seconds=2.f;
        float base_unit=0.5f,radius_personality=0.5f;
        std::array<float,24> radius_knots{},rpm_knots{},aspect_knots{};
    };
    inline spinner_style_t make_spinner_style(uint32_t seed,float real_duration) {
        spinner_style_t s{};s.valid=true;
        const auto sample=[seed](uint32_t salt) {
            return static_cast<float>(mix_seed(seed^salt)&0x00ffffffu)/16777216.f;
        };
        s.radius_personality=sample(0xa4093822u);
        s.mean_radius=42.f+61.f*s.radius_personality;
        s.axis_ratio=1.18f+1.f*sample(0x299f31d0u);
        // Reserve room for aspect/radius evolution and centroid drift. This
        // limits the chosen style, not each frame's final cursor position.
        s.mean_radius=std::min(s.mean_radius,149.f/(std::sqrt(s.axis_ratio*1.055f)*1.10f));
        s.rotation=(sample(0x13198a2eu)*2.f-1.f)*k_pi;
        s.rotation_drift=(sample(0x85a308d3u)*2.f-1.f)*0.12f;
        s.phase=sample(0x243f6a88u)*2.f*k_pi;
        const float angle=sample(0x082efa98u)*2.f*k_pi;
        s.center_bias=point_t{std::cos(angle),std::sin(angle)}*(1.5f+3.f*sample(0xec4e6c89u));
        const float drift_angle=sample(0x452821e6u)*2.f*k_pi;
        s.drift_direction=point_t{std::cos(drift_angle),std::sin(drift_angle)}*(1.f+2.f*sample(0x38d01377u));
        s.asymmetry=0.003f+0.008f*sample(0x2ffd72dbu);
        s.knot_seconds=std::max(1.5f+sample(0x3f84d5b5u)*1.2f,real_duration/22.f);
        // Independent style dominates; tight orbits have only a mild faster bias.
        s.base_unit=std::clamp(0.20f+0.60f*sample(0x9216d5d9u)+
            0.08f*(0.5f-s.radius_personality),0.15f,0.85f);
        for(size_t i=0;i<s.radius_knots.size();++i) {
            const uint32_t salt=static_cast<uint32_t>(i+1)*0x9e3779b9u;
            const float radius_delta=(sample(salt^0xc0ac29b7u)*2.f-1.f)*0.10f;
            s.radius_knots[i]=1.f+radius_delta;
            s.aspect_knots[i]=1.f+(sample(salt^0xc97c50ddu)*2.f-1.f)*0.055f;
            s.rpm_knots[i]=std::clamp(s.base_unit+(sample(salt^0xd1310ba6u)*2.f-1.f)*0.18f-
                radius_delta*0.35f,0.045f,0.955f);
        }
        return s;
    }
    inline double smoother_unit(double t) {
        t=std::clamp(t,0.0,1.0);return t*t*t*(10.0+t*(-15.0+6.0*t));
    }
    inline float spinner_profile(const std::array<float,24>& knots,float span,double seconds) {
        const double x=std::max(0.0,seconds)/span;
        const size_t i=std::min(static_cast<size_t>(x),knots.size()-1);
        if(i+1==knots.size()) return knots.back();
        return static_cast<float>(knots[i]+(knots[i+1]-knots[i])*smoother_unit(x-i));
    }
    inline double spinner_profile_integral(const std::array<float,24>& knots,float span,double seconds) {
        const double x=std::max(0.0,seconds)/span;
        const size_t complete=std::min(static_cast<size_t>(x),knots.size()-1);
        double area=0.0;
        for(size_t i=0;i<complete;++i) area+=(knots[i]+knots[i+1])*0.5*span;
        if(complete+1==knots.size()) return area+(x-complete)*span*knots.back();
        const double u=x-complete,u2=u*u,u4=u2*u2;
        const double integral=u4*(2.5+u*(-3.0+u));
        return area+span*(knots[complete]*u+(knots[complete+1]-knots[complete])*integral);
    }
    inline float spinner_rpm_at(const spinner_style_t& style,spinner_rpm_range_t range,double seconds) {
        return range.minimum+(range.maximum-range.minimum)*spinner_profile(style.rpm_knots,style.knot_seconds,seconds);
    }
    inline double spinner_rpm_integral(const spinner_style_t& style,spinner_rpm_range_t range,double seconds) {
        return range.minimum*seconds+(range.maximum-range.minimum)*
            spinner_profile_integral(style.rpm_knots,style.knot_seconds,seconds);
    }
    // Ray/ellipse intersection about the true spinner center. Shape changes
    // cannot silently multiply actual polar RPM as ellipse parameterization did.
    inline float spinner_orbit_radius(const spinner_style_t& style,double angle,double seconds,
                                      float* major=nullptr,float* minor=nullptr,point_t* drift=nullptr) {
        const float scale=spinner_profile(style.radius_knots,style.knot_seconds,seconds);
        const float aspect=style.axis_ratio*spinner_profile(style.aspect_knots,style.knot_seconds,seconds);
        const float a=style.mean_radius*scale*std::sqrt(aspect),b=style.mean_radius*scale/std::sqrt(aspect);
        const float rotation=style.rotation+style.rotation_drift*std::sin(static_cast<float>(seconds)*0.21f);
        const point_t offset=style.center_bias+style.drift_direction*
            std::sin(static_cast<float>(seconds)*0.37f+style.phase);
        const double c=std::cos(rotation),s=std::sin(rotation);
        const double dx=std::cos(angle-rotation),dy=std::sin(angle-rotation);
        const double cx=offset.x*c+offset.y*s,cy=-offset.x*s+offset.y*c;
        const double aa=dx*dx/(a*a)+dy*dy/(b*b);
        const double bb=-2.0*(dx*cx/(a*a)+dy*cy/(b*b));
        const double cc=cx*cx/(a*a)+cy*cy/(b*b)-1.0;
        double radius=(-bb+std::sqrt(std::max(0.0,bb*bb-4.0*aa*cc)))/(2.0*aa);
        radius*=1.0+style.asymmetry*std::sin(angle*3.0+style.phase+seconds*0.16);
        if(major) *major=a;if(minor) *minor=b;if(drift) *drift=offset;
        return static_cast<float>(radius);
    }
    inline point_t spinner_orbit_point(const spinner_style_t& style,point_t center,double initial_angle,
                                       float direction,spinner_rpm_range_t range,double seconds) {
        const double angle=initial_angle+direction*(2.0*k_pi/60.0)*spinner_rpm_integral(style,range,seconds);
        const float radius=spinner_orbit_radius(style,angle,seconds);
        return center+point_t{static_cast<float>(std::cos(angle)),static_cast<float>(std::sin(angle))}*radius;
    }
    inline kinematic_t spinner_entry_kinematics(const spinner_style_t& style,point_t center,double angle,
                                               float direction,spinner_rpm_range_t range) {
        // Fixed-time differentiation only at acquisition creation.
        constexpr double h=0.001;
        const point_t p0=spinner_orbit_point(style,center,angle,direction,range,0.0);
        const point_t p1=spinner_orbit_point(style,center,angle,direction,range,h);
        const point_t p2=spinner_orbit_point(style,center,angle,direction,range,2*h);
        const point_t p3=spinner_orbit_point(style,center,angle,direction,range,3*h);
        return {p0,(p0*-3.f+p1*4.f-p2)/static_cast<float>(2*h),
            (p0*2.f-p1*5.f+p2*4.f-p3)/static_cast<float>(h*h)};
    }
    struct spinner_state_t {
        bool active=false,first_output=true,range_initialized=false;
        int object_index=-1;
        point_t center{},last_output{},center_drift{},center_bias{};
        motion_state_t incoming{};
        spinner_style_t style{};
        spinner_rpm_range_t range{};
        double initial_angle=0.0,angle=0.0,elapsed_seconds=0.0;
        float angular_velocity=0.f,radius=70.f,direction=1.f;
        float major_axis=70.f,minor_axis=70.f,rotation=0.f,revolutions=0.f;
        float requested_rpm=0.f,min_observed_rpm=1e6f,max_observed_rpm=0.f;
        double output_revolutions=0.0;
        float radial_residual=0.f,radial_velocity_residual=0.f,radial_accel_residual=0.f;
    };
    inline void spinner_begin(spinner_state_t& sp,int index,const osu::hit_object_t& o,bool hr_flip,
                               const motion_state_t& m,uint32_t seed,float session_direction,
                               const spinner_style_t* prepared_style=nullptr) {
        sp={};sp.active=true;sp.object_index=index;sp.center=object_center(o,hr_flip);
        sp.incoming=m;sp.radius=std::max(length(m.position-sp.center),1.f);
        sp.initial_angle=std::atan2(m.position.y-sp.center.y,m.position.x-sp.center.x);
        sp.angle=sp.initial_angle;
        sp.direction=session_direction>=0.f?1.f:-1.f;
        sp.style=prepared_style?*prepared_style:make_spinner_style(seed,(o.end_time-o.start_time)*0.001f);
        sp.rotation=sp.style.rotation;sp.center_bias=sp.style.center_bias;
        sp.last_output=m.position;
    }
    inline point_t spinner_step_range(spinner_state_t& sp,const osu::hit_object_t&,int,float dt,
                                       float rpm_min,float rpm_max) {
        if(!sp.range_initialized) {
            sp.range=sanitize_spinner_range(rpm_min,rpm_max);sp.range_initialized=true;
            const auto k=spinner_entry_kinematics(sp.style,sp.center,sp.initial_angle,sp.direction,sp.range);
            const point_t radial{static_cast<float>(std::cos(sp.initial_angle)),static_cast<float>(std::sin(sp.initial_angle))};
            const float target_radius=length(k.position-sp.center);
            sp.radial_residual=sp.radius-target_radius;
            sp.radial_velocity_residual=dot(sp.incoming.velocity-k.velocity,radial);
            const point_t tangent{-radial.y,radial.x};
            const float incoming_tangent=dot(sp.incoming.velocity,tangent),target_tangent=dot(k.velocity,tangent);
            sp.radial_accel_residual=dot(sp.incoming.acceleration-k.acceleration,radial)+
                incoming_tangent*incoming_tangent/sp.radius-target_tangent*target_tangent/std::max(target_radius,1.f);
        }
        const double previous_time=sp.elapsed_seconds;
        const point_t previous_radial=sp.last_output-sp.center;
        sp.elapsed_seconds+=std::max(0.f,dt);
        sp.requested_rpm=spinner_rpm_at(sp.style,sp.range,sp.elapsed_seconds);
        sp.angle=sp.initial_angle+sp.direction*(2.0*k_pi/60.0)*spinner_rpm_integral(sp.style,sp.range,sp.elapsed_seconds);
        sp.angular_velocity=sp.direction*sp.requested_rpm*(2.f*k_pi/60.f);
        sp.revolutions=static_cast<float>(std::abs(sp.angle-sp.initial_angle)/(2.0*k_pi));
        float radius=spinner_orbit_radius(sp.style,sp.angle,sp.elapsed_seconds,&sp.major_axis,&sp.minor_axis,&sp.center_drift);
        constexpr float response=0.18f;
        const float u=static_cast<float>(sp.elapsed_seconds)/response;
        if(u<1.f) {
            const float q=std::max(u,0.f),q2=q*q,q3=q2*q,q4=q3*q,q5=q4*q;
            radius+=sp.radial_residual*(1.f-10*q3+15*q4-6*q5)+
                sp.radial_velocity_residual*response*(q-6*q3+8*q4-3*q5)+
                sp.radial_accel_residual*(response*response)*0.5f*(q2-3*q3+3*q4-q5);
        }
        // A resume/external reanchor can start outside the style's normal
        // region. Move the guard inward continuously; never teleport that
        // incoming radius straight to the normal boundary on the first frame.
        const float incoming_radius=std::max(length(sp.incoming.position-sp.center),1.f);
        const float entry=static_cast<float>(smoother_unit(u));
        const float lower=std::min(20.f,incoming_radius)*(1.f-entry)+20.f*entry;
        const float upper=std::max(162.f,incoming_radius)*(1.f-entry)+162.f*entry;
        radius=std::clamp(radius,lower,upper);
        const point_t output=sp.center+point_t{static_cast<float>(std::cos(sp.angle)),static_cast<float>(std::sin(sp.angle))}*radius;
        sp.radius=radius;sp.last_output=output;
        if(sp.elapsed_seconds>previous_time) {
            const point_t next_radial=output-sp.center;
            const double delta=std::atan2(previous_radial.x*next_radial.y-previous_radial.y*next_radial.x,
                dot(previous_radial,next_radial));
            sp.output_revolutions+=std::abs(delta)/(2.0*k_pi);
            const float measured=static_cast<float>(std::abs(delta)/(sp.elapsed_seconds-previous_time)*60.0/(2.0*k_pi));
            sp.min_observed_rpm=std::min(sp.min_observed_rpm,measured);sp.max_observed_rpm=std::max(sp.max_observed_rpm,measured);
        }
        return output;
    }
    // Fixed-range adapter for the existing pure RPM regression harness.
    inline point_t spinner_step(spinner_state_t& sp,const osu::hit_object_t& o,int gt,float dt,float rpm,float) {
        return spinner_step_range(sp,o,gt,dt,rpm,rpm);
    }

    // ---- discontinuity budget --------------------------------------------
    //
    // The largest plausible one-frame displacement given the velocity /
    // acceleration at both ends of the step, plus a small slack.  A produced
    // displacement above this is a discontinuity (reported, never hidden).
    inline float kinematic_budget( point_t v_old, point_t v_new, point_t a_old, point_t a_new, float dt ) {
        return std::max( length( v_old ), length( v_new ) ) * dt
             + 0.5f * std::max( length( a_old ), length( a_new ) ) * dt * dt + 2.f;
    }

    inline point_t clamp_to_playfield( point_t p ) {
        p.x = std::clamp( p.x, 32.f, 480.f );
        p.y = std::clamp( p.y, 28.f, 356.f );
        return p;
    }

    // ---- tiny deterministic xorshift (session style / decorative shapes) --
    struct rng_t {
        uint32_t s = 0x9e3779b9u;
        uint32_t next( ) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
        float unit( ) { return static_cast<float>( next( ) & 0x00ffffffu ) / 16777216.f; }
    };

    // Distinguishes persistent external cursor movement from a delayed sample
    // of one of Autobot's own recent absolute outputs.  Kept pure for tests.
    class cursor_observation_guard_t {
    public:
        void reset( ) { m_head = m_count = 0; m_streak = 0; m_candidate_valid = false; }
        void note_output( point_t p ) {
            m_recent[ m_head ] = p;
            m_head = ( m_head + 1 ) % m_recent.size( );
            m_count = std::min( m_count + 1, m_recent.size( ) );
        }
        [[nodiscard]] bool observe( point_t p ) {
            for ( size_t i = 0; i < m_count; ++i ) {
                if ( length( p - m_recent[ i ] ) <= 6.f ) {
                    m_streak = 0; m_candidate_valid = false;
                    return false;
                }
            }
            if ( !m_candidate_valid || length( p - m_candidate ) > 5.f ) {
                m_candidate = p; m_candidate_valid = true; m_streak = 1;
                return false;
            }
            m_candidate = p;
            if ( ++m_streak < 3 ) return false;
            m_streak = 0; m_candidate_valid = false;
            return true;
        }
    private:
        std::array<point_t, 8> m_recent{};
        size_t m_head = 0, m_count = 0;
        int m_streak = 0;
        point_t m_candidate{};
        bool m_candidate_valid = false;
    };

    // ---- tunables the host feeds the engine each frame --------------------
    struct settings_t {
        float aim_spread = 0.10f;
        float curve_strength = 0.36f;
        float momentum = 0.82f;
        float slider_laziness = 0.08f;
        float spinner_rpm_min = 400.f, spinner_rpm_max = 440.f;
        bool  gameplay_flow = true;
        bool  startup_motion = true;
        bool  break_motion = true;
        bool  energetic_dances = true;
        float startup_energy = 0.82f;
        float break_energy = 0.68f;
        bool  perfect_play = false;
        // Internal isolation switches used by the deterministic diagnostic
        // harness.  They are deliberately not config/UI settings.
        bool diagnostic_target_variation = true;
        bool diagnostic_session_style = true;
        bool diagnostic_slider_exit = true;
        bool diagnostic_micro_motion = true;
        bool diagnostic_safe_arrival_cone = true;
    };

    // ---- per-object schedule the host supplies (press time + edge bias) ---
    //
    // Filled from the shared Relax scheduler by the host adapter; for off-line
    // tests it is populated directly.  press() falls back to the object's own
    // start_time when the object has not been scheduled yet.
    struct schedule_view_t {
        std::vector<int>   press_time;
        std::vector<int>   release_time;
        std::vector<float> edge_bias;
        int press( int index, const osu::hit_object_t& o ) const {
            return ( index >= 0 && index < static_cast<int>( press_time.size( ) ) )
                ? press_time[ static_cast<size_t>( index ) ] : o.start_time;
        }
        float bias( int index ) const {
            return ( index >= 0 && index < static_cast<int>( edge_bias.size( ) ) )
                ? edge_bias[ static_cast<size_t>( index ) ] : 0.f;
        }
        int release( int index, const osu::hit_object_t& o ) const {
            return ( index >= 0 && index < static_cast<int>( release_time.size( ) ) )
                ? release_time[ static_cast<size_t>( index ) ] : o.end_time;
        }
    };

    // ---- live diagnostics describing the architecture ---------------------
    struct diagnostics_t {
        plan_type_t plan_type = plan_type_t::none;
        uint64_t    plan_id = 0;
        int         object_index = -1;
        float       plan_progress = 0.f;
        float       time_to_arrival_ms = 0.f;
        bool target_perceived = false;
        int target_perception_time = 0;
        int perception_preempt_ms = 0;
        float target_commitment = 0.f;
        uint64_t perception_promotions = 0;
        point_t     motion_position{}, motion_velocity{}, motion_acceleration{};
        point_t     target_position{};
        float       speed = 0.f, acceleration = 0.f;
        // screen fields (filled by the host adapter)
        int         requested_screen_x = 0, requested_screen_y = 0;
        int         observed_screen_x = 0, observed_screen_y = 0;
        float       projection_self_check_error = 0.f;
        // counters
        uint64_t plans_created = 0, gameplay_plans = 0, slider_plans = 0, spinner_plans = 0;
        uint64_t startup_segments = 0, break_segments = 0, recovery_plans = 0;
        uint64_t plan_invalidations = 0, plan_transitions = 0, external_reanchors = 0;
        uint64_t unexpected_discontinuities = 0, near_discontinuities = 0;
        uint64_t objects_completed = 0;
        float    max_frame_displacement = 0.f, max_speed = 0.f, max_acceleration = 0.f, max_jerk = 0.f;
        decor_motif_t decor_motif = decor_motif_t::count;
        const char* last_transition_reason = "";
        const char* last_flight_reason = "";
        // Target Accuracy passthrough (owned by the Relax controller)
        float    requested_accuracy = 100.f, predicted_accuracy = 100.f, accuracy_debt = 0.f;
        uint64_t grade_300 = 0, grade_100 = 0, grade_50 = 0;
        uint64_t controlled_100 = 0, controlled_50 = 0, intentional_miss = 0;
        uint64_t unsafe_at_100 = 0;
        bool perfect_play = false;
        float safe_300_margin_ms = 0.f;
        uint64_t timing_clamp_count = 0;
        uint64_t accuracy_objects = 0;
        bool combo_safe_mode = true;
        uint32_t session_seed = 0;
        bool deterministic_test_mode = false;
        float style_directness = 0.f, style_curvature = 0.f, style_accel = 0.f;
        float style_momentum = 0.f, style_target_depth = 0.f;
        // Rolling dense-pattern flow.
        int dense_lookahead = 5;
        bool density_mode = false;
        uint64_t dense_flow_activations = 0, dense_pattern_recoveries = 0;
        // Predictive slider integrity (the game does not expose judgments here).
        uint64_t sliders_started = 0, sliders_completed = 0, slider_head_failures = 0;
        uint64_t slider_follow_failures = 0, slider_early_releases = 0;
        uint64_t compact_slider_activations = 0, slider_exit_failures = 0;
        uint64_t ultra_short_slider_activations = 0;
        uint64_t slider_handoff_activations = 0, slider_handoff_rejections = 0;
        uint64_t sliders_evaluated = 0, safe_anchor_sections = 0;
        uint64_t partial_anchor_activations = 0, normal_follow_sections = 0;
        float slider_local_speed = 0.f, slider_anchor_max_distance = 0.f, slider_effective_follow_radius = 0.f;
        slider_strategy_t slider_strategy = slider_strategy_t::track;
        slider_exit_strategy_t slider_exit_strategy = slider_exit_strategy_t::curved_departure;
        point_t slider_tail_tangent{}, slider_outgoing_velocity{};
        float slider_post_tail_overshoot = 0.f, slider_exit_curvature = 0.f;
        float slider_ball_path_length = 0.f, slider_cursor_path_length = 0.f;
        float slider_cursor_ball_ratio = 0.f;
        float local_motion_pressure = 0.f, slider_hand_economy = 0.f;
        float slider_min_follow_margin = 0.f;
        // Spinner integrity.
        uint64_t spinner_acquisitions = 0, unexpected_spinner_reversals = 0;
        uint64_t spinner_direction_changes_between_objects = 0;
        uint64_t spinner_entry_issues = 0, spinner_exit_issues = 0;
        float spinner_preferred_direction = 1.f;
        float spinner_min_radius = 100000.f, spinner_max_radius = 0.f;
        float spinner_requested_rpm = 0.f, spinner_measured_rpm = 0.f, spinner_revolutions = 0.f;
        float spinner_rpm_min=0.f,spinner_rpm_max=0.f,spinner_rpm_low=0.f,spinner_rpm_high=0.f;
        float spinner_major_axis = 0.f, spinner_minor_axis = 0.f, spinner_rotation_degrees = 0.f;
        float spinner_center_drift = 0.f;
        uint32_t spinner_seed = 0;
        int spinner_object_index = -1;
        float spinner_speed_multiplier = 1.f;
        // Decorative ownership and return safety.
        decor_motif_t previous_decor_motif = decor_motif_t::count;
        uint64_t dance_motifs = 0, fidget_motifs = 0;
        uint64_t decoration_canceled_for_acquisition = 0, decoration_caused_recovery = 0;
        float max_decorative_excursion = 0.f;
        float decor_requested_scale = 0.f, decor_bbox_width = 0.f, decor_bbox_height = 0.f;
        float decor_energy = 0.f, decor_budget_ms = 0.f;
        bool break_active = false, break_acquiring = false;
        float break_required_lead_ms = 0.f, break_acquisition_lead_ms = 0.f;
        uint64_t eligible_micro_gaps = 0, micro_gestures_activated = 0;
        uint64_t micro_canceled_for_acquisition = 0, micro_caused_recovery = 0;
        uint64_t micro_rejected_unsafe = 0, suspicious_forward_bursts = 0;
        micro_gesture_t micro_gesture = micro_gesture_t::none;
        float micro_max_excursion = 0.f, micro_safe_envelope = 0.f;
        float micro_required_acquisition_ms = 0.f, micro_available_budget_ms = 0.f;
        uint64_t target_cache_writes = 0, target_coordinate_changes = 0;
        uint64_t transition_intervals = 0, transition_plan_sum = 0, excessive_plan_intervals = 0;
        uint32_t max_plans_per_transition = 0;
        uint64_t arrival_velocity_safety_reductions = 0;
        // Every authoritative output is audited by the host adapter.
        uint64_t outputs_emitted = 0, output_discontinuities = 0;
        float max_output_displacement = 0.f;
    };

    // ---- flight recorder entry -------------------------------------------
    struct flight_record_t {
        uint64_t    frame = 0;
        int         game_time = 0;
        float       dt = 0.f;
        uint64_t    plan_id = 0;
        plan_type_t plan_type = plan_type_t::none;
        int         object_index = -1;
        uint64_t    previous_plan_id = 0;
        plan_type_t previous_plan_type = plan_type_t::none;
        int         previous_object_index = -1;
        point_t     position{}, velocity{}, acceleration{}, target{}, requested{};
        point_t     velocity_after{}, acceleration_after{};
        int         observed_x = 0, observed_y = 0;
        float       displacement = 0.f, bound = 0.f, speed = 0.f, acceleration_magnitude = 0.f, jerk = 0.f;
        float       scheduled_arrival = 0.f, time_remaining = 0.f;
        float       plan_start = 0.f, plan_duration = 0.f;
        float       acquisition_required_ms = 0.f, acquisition_budget_ms = 0.f;
        int         previous_object = -1, next_object = -1;
        uint8_t     previous_object_type = 0, next_object_type = 0;
        slider_strategy_t slider_strategy = slider_strategy_t::track;
        micro_gesture_t micro_gesture = micro_gesture_t::none;
        decor_motif_t decor_motif = decor_motif_t::count;
        uint32_t    session_seed = 0;
        bool        external_reanchor = false, decorative = false;
        bool        slider_active = false, micro_active = false, recovery = false;
        const char* reason = "";
        const char* plan_reason = "";
    };

    // ========================================================================
    //  motion_engine_t : the pure, host-independent movement core.
    //
    //  Everything happens in osu! playfield coordinates.  There is exactly one
    //  MotionState and one MovementPlan.  The host adapter (autobot.hxx) reads
    //  the OS cursor, projects it to playfield space, supplies the schedule,
    //  drives update(), then projects position() back to the screen to emit.
    //
    //  Nothing in here touches Windows, so the whole planner + evaluator is
    //  exercised by the off-line tests in tests/motion_tests.cpp.
    // ========================================================================
    class motion_engine_t {
    public:
        settings_t settings{};

        void configure( const settings_t& s ) { settings = s; }
        [[nodiscard]] bool initialized( ) const { return m_motion.initialized; }
        [[nodiscard]] const motion_state_t& motion( ) const { return m_motion; }
        [[nodiscard]] point_t position( ) const { return m_motion.position; }
        [[nodiscard]] const plan_t& plan( ) const { return m_plan; }
        [[nodiscard]] plan_type_t plan_type( ) const { return m_plan.type; }
        [[nodiscard]] const diagnostics_t& diagnostics( ) const { return m_diag; }
        [[nodiscard]] uint64_t record_count( ) const { return m_rec_count; }
        [[nodiscard]] bool last_record( flight_record_t& out ) const {
            if ( m_rec_count == 0 ) return false;
            out = m_rec[ ( m_rec_head + k_rec_capacity - 1 ) % k_rec_capacity ];
            return true;
        }
        [[nodiscard]] bool last_suspicious_record( flight_record_t& out ) const {
            if ( !m_have_suspicious_record ) return false;
            out = m_last_suspicious_record;
            return true;
        }
        [[nodiscard]] bool last_discontinuity_record( flight_record_t& out ) const {
            if ( !m_have_discontinuity_record ) return false;
            out = m_last_discontinuity_record;
            return true;
        }
        [[nodiscard]] bool cached_target_point( int index, point_t& out ) const {
            if ( index < 0 || index >= static_cast<int>( m_target_valid.size( ) ) ||
                 !m_target_valid[ static_cast<size_t>( index ) ] ) return false;
            out = m_target_points[ static_cast<size_t>( index ) ];
            return true;
        }

        // Start (or restart) a session.  MotionState is seeded from the observed
        // cursor -- the ONLY direct position assignment allowed.
        void begin_session( point_t observed, bool hr_flip, float game_rate, size_t object_count,
                            uint32_t seed, bool deterministic_test = false ) {
            m_motion = {}; m_plan = {}; m_spinner = {}; m_slider_runtime = {}; m_micro = {}; m_decor = {}; m_diag = {};
            m_rec_head = 0; m_rec_count = 0; m_have_suspicious_record = false;
            m_frame = 0; m_last_decor_motif = decor_motif_t::count;
            m_exit_audit_active = false; m_exit_audit_tail = {}; m_exit_audit_tangent = {}; m_exit_audit_start = 0;
            m_plan_count_target = -1; m_plans_for_target = 0;
            m_break_target = -1; m_break_acquiring = false; m_have_discontinuity_record = false;
            m_have_state_time=false; m_perception_time=0;
            m_slider_cache.assign( object_count, {} );
            m_target_points.assign( object_count, {} );
            m_target_valid.assign( object_count, 0 );
            m_spinner_styles.clear();m_spinner_style_slots.assign(object_count,-1);
            m_map_object_count = object_count;
            m_last_completed_index = -1;
            m_hr_flip = hr_flip;
            m_game_rate = std::clamp( game_rate, 0.5f, 2.f );
            m_seed = seed ? seed : 0x9e3779b9u;
            m_rng.s = m_seed;
            m_style.directness    = 0.80f + m_rng.unit( ) * 0.16f;
            m_style.curvature     = 0.75f + m_rng.unit( ) * 0.50f;
            m_style.accel         = 0.90f + m_rng.unit( ) * 0.20f;
            m_style.momentum_ret  = 0.82f + m_rng.unit( ) * 0.14f;
            m_style.arrival_depth = 0.45f + m_rng.unit( ) * 0.20f;
            m_style.idle_energy   = 0.35f + m_rng.unit( ) * 0.45f;
            m_style.slider_tight  = 0.82f + m_rng.unit( ) * 0.16f;
            m_style.phase         = m_rng.unit( ) * 2.f * k_pi;
            m_style.motor_response_ms = 150.f + m_rng.unit( ) * 50.f;
            m_style.preferred_spinner_direction = m_rng.unit( ) < 0.5f ? -1.f : 1.f;
            if ( !settings.diagnostic_session_style ) {
                m_style.directness = 0.88f; m_style.curvature = 1.f; m_style.accel = 1.f;
                m_style.momentum_ret = 0.88f; m_style.arrival_depth = 0.55f;
                m_style.idle_energy = 0.5f; m_style.slider_tight = 0.90f;
                m_style.phase = 0.f; m_style.motor_response_ms = 175.f;
                m_style.preferred_spinner_direction = 1.f;
            }
            // The old session polarity is explicitly reversed ONCE here.
            // Positive polar angle is CLOCKWISE on the y-down osu! playfield.
            // Acquisition and every spinner consume this authoritative sign.
            constexpr float requested_direction_polarity=-1.f;
            m_session_spinner_direction = requested_direction_polarity*m_style.preferred_spinner_direction;
            m_last_spinner_direction = 0.f;
            m_have_last_spinner_direction = false;
            m_diag.spinner_preferred_direction = m_session_spinner_direction;
            m_diag.session_seed = m_seed;
            m_diag.deterministic_test_mode = deterministic_test;
            m_diag.style_directness = m_style.directness;
            m_diag.style_curvature = m_style.curvature;
            m_diag.style_accel = m_style.accel;
            m_diag.style_momentum = m_style.momentum_ret;
            m_diag.style_target_depth = m_style.arrival_depth;
            m_motion.position = observed;
            m_motion.initialized = true;
            m_next_plan_id = 1;
        }

        // Rebase on the observed cursor after a persistent external mismatch or
        // a resume; the NEXT plan begins from the observed state.
        void reanchor( point_t observed, const char* reason ) {
            const plan_t previous = m_plan;
            const motion_state_t before = m_motion;
            const point_t stopped{};
            m_reanchor_flag = true;
            capture_record( before.position, observed, length( observed - before.position ), 0.f,
                            reason, &previous, &stopped, &stopped );
            m_motion.position = observed;
            m_motion.velocity = {}; m_motion.acceleration = {};
            m_plan = {}; m_spinner.active = false; m_slider_runtime.active = false; m_decor.active = false;
            m_diag.external_reanchors++;
        }

        // One frame.  control_time in game ms, dt in real seconds (sanitized).
        void update( const osu::beatmap_data_t& map, const schedule_view_t& sched, int control_time, float dt,
                     int perception_time = INT32_MIN ) {
            if ( !m_motion.initialized ) return;
            ++m_frame; m_frame_ctrl = control_time; m_frame_dt = dt;
            // The host's prepared scheduling clock may lead the rendered game.
            // Only the actual game clock may unlock future spatial information.
            m_perception_time = perception_time == INT32_MIN ? control_time : perception_time;
            m_diag.perception_preempt_ms=object_preempt_ms(map.ar,m_hr_flip);
            m_reanchor_flag = false;
            m_frame_previous_object = m_last_completed_index;
            m_frame_next_object = next_object_index( );
            m_frame_previous_object_type = m_frame_previous_object >= 0 &&
                m_frame_previous_object < static_cast<int>( map.objects.size( ) )
                ? map.objects[ static_cast<size_t>( m_frame_previous_object ) ].type : 0;
            m_frame_next_object_type = m_frame_next_object >= 0 &&
                m_frame_next_object < static_cast<int>( map.objects.size( ) )
                ? map.objects[ static_cast<size_t>( m_frame_next_object ) ].type : 0;

            plan_and_reconcile( control_time, map, sched );
            eval_result_t r = evaluate_plan( control_time, dt, map, sched );
            commit_motion( r, dt );
            m_state_time=control_time; m_have_state_time=true;
            advance_completed( control_time, map, sched );
            update_live_diagnostics( control_time );
        }

        // Host hooks for screen-space events it detects (geometry / emit).
        void note_reanchor_pending( ) { m_reanchor_flag = true; }
        void record_external( point_t at, float disp, float bound, const char* reason ) {
            capture_record( at, at, disp, bound, reason );
            m_diag.unexpected_discontinuities++;
        }
        void bump_discontinuity( ) { m_diag.unexpected_discontinuities++; }
        void set_screen_diag( int rx, int ry, int ox, int oy ) {
            m_diag.requested_screen_x = rx; m_diag.requested_screen_y = ry;
            m_diag.observed_screen_x = ox; m_diag.observed_screen_y = oy;
        }
        void set_accuracy_diag( float requested, float predicted, float debt, uint64_t grade300,
                                uint64_t grade100, uint64_t grade50,
                                uint64_t controlled100, uint64_t controlled50,
                                uint64_t misses, uint64_t unsafe100, uint64_t objects,
                                bool perfect, float safe_margin, uint64_t timing_clamps ) {
            m_diag.requested_accuracy = requested; m_diag.predicted_accuracy = predicted;
            m_diag.accuracy_debt = debt; m_diag.grade_300 = grade300;
            m_diag.grade_100 = grade100; m_diag.grade_50 = grade50;
            m_diag.controlled_100 = controlled100; m_diag.controlled_50 = controlled50;
            m_diag.intentional_miss = misses; m_diag.unsafe_at_100 = unsafe100;
            m_diag.accuracy_objects = objects;
            m_diag.perfect_play = perfect; m_diag.safe_300_margin_ms = safe_margin;
            m_diag.timing_clamp_count = timing_clamps;
        }
        void set_output_diag( point_t previous, point_t current, float displacement, bool discontinuity ) {
            ++m_diag.outputs_emitted;
            m_diag.max_output_displacement = std::max( m_diag.max_output_displacement, displacement );
            if ( discontinuity ) ++m_diag.output_discontinuities;
            capture_output_sample( previous, current, displacement );
        }
        void set_projection_error( float e ) { m_diag.projection_self_check_error = e; }
        void invalidate_plan( const char* reason = "plan-invalidation" ) {
            if ( !m_plan.valid ) return;
            capture_record( m_motion.position, m_motion.position, 0.f, 0.f, reason );
            m_plan.valid = false;
            m_diag.plan_invalidations++;
        }

    private:
        struct style_t {
            float directness = 0.85f, curvature = 1.f, accel = 1.f, momentum_ret = 0.85f;
            float arrival_depth = 0.55f, idle_energy = 0.5f, slider_tight = 0.85f, phase = 0.f;
            float motor_response_ms = 175.f, preferred_spinner_direction = 1.f;
        };
        struct decor_state_t {
            bool active = false, startup = false;
            bool quiet = false;
            point_t anchor{};
            decor_motif_t motif = decor_motif_t::count;
            int segments = 0, segment_index = 0, segment_count = 0;
            float scale = 18.f, rotation = 0.f, handedness = 1.f;
            float budget_ms = 0.f;
            point_t bounds_lo{}, bounds_hi{};
            bool bounds_initialized = false;
        };
        struct slider_runtime_t {
            bool active = false, head_failed = false, follow_failed = false, early_release = false;
            int object_index = -1;
            point_t anchor{};
            float anchor_blend = 0.f;
            bool safe_section = false, strategy_initialized = false;
            slider_strategy_t strategy = slider_strategy_t::track;
            float cursor_path_length = 0.f;
            point_t last_cursor{};
            bool have_last_cursor = false;
            float min_follow_margin = 1e6f;
            // A validated final hand-path segment replaces the old per-frame
            // moving-target chase near the tail. Its endpoint derivative faces
            // the next acquisition, so the next gameplay quintic never inherits
            // an outward slider-ball velocity.
            bool handoff_active = false;
            int handoff_start = 0, handoff_end = 0;
            point_t handoff_p0{}, handoff_v0{}, handoff_a0{};
            point_t handoff_p1{}, handoff_v1{}, handoff_a1{};
        };
        struct micro_state_t {
            bool active = false;
            int target_index = -1, last_decided_target = -1;
            micro_gesture_t gesture = micro_gesture_t::none, previous = micro_gesture_t::none;
            point_t anchor{};
            float safe_envelope = 0.f;
        };
        struct eval_result_t {
            point_t position{}, velocity{}, acceleration{};
            bool dynamic = false;
            const char* reason = "plan";
        };

        motion_state_t m_motion{};
        plan_t         m_plan{};
        spinner_state_t m_spinner{};
        slider_runtime_t m_slider_runtime{};
        micro_state_t    m_micro{};
        style_t        m_style{};
        decor_state_t  m_decor{};
        diagnostics_t  m_diag{};
        rng_t          m_rng{};

        std::vector<slider_path_t> m_slider_cache;
        std::vector<point_t>       m_target_points;
        std::vector<uint8_t>       m_target_valid;
        std::vector<spinner_style_t> m_spinner_styles;
        std::vector<int> m_spinner_style_slots;

        bool     m_hr_flip = false;
        size_t   m_map_object_count = 0;
        int      m_last_completed_index = -1;
        uint32_t m_seed = 0x9e3779b9u;
        float    m_game_rate = 1.f;
        float    m_session_spinner_direction = 1.f;
        float    m_last_spinner_direction = 0.f;
        bool     m_have_last_spinner_direction = false;
        uint64_t m_next_plan_id = 1;
        bool     m_reanchor_flag = false;
        const char* m_plan_reason = "";
        int m_frame_previous_object = -1, m_frame_next_object = -1;
        uint8_t m_frame_previous_object_type = 0, m_frame_next_object_type = 0;
        int m_plan_count_target = -1;
        int m_break_target = -1;
        bool m_break_acquiring = false;
        int m_perception_time=0,m_state_time=0;
        bool m_have_state_time=false;
        uint32_t m_plans_for_target = 0;
        decor_motif_t m_last_decor_motif = decor_motif_t::count;
        bool m_exit_audit_active = false;
        point_t m_exit_audit_tail{}, m_exit_audit_tangent{};
        int m_exit_audit_start = 0;

        uint64_t m_frame = 0;
        int      m_frame_ctrl = 0;
        float    m_frame_dt = 0.f;
        float    m_frame_jerk = 0.f;

        static constexpr size_t k_rec_capacity = 64;
        std::array<flight_record_t, k_rec_capacity> m_rec{};
        size_t   m_rec_head = 0;
        uint64_t m_rec_count = 0;
        flight_record_t m_last_suspicious_record{};
        bool m_have_suspicious_record = false;
        flight_record_t m_last_discontinuity_record{};
        bool m_have_discontinuity_record = false;

        // ---- schedule helpers --------------------------------------------
        int next_object_index( ) const {
            const int n = static_cast<int>( m_map_object_count );
            const int i = m_last_completed_index + 1;
            return i < n ? i : -1;
        }
        int find_active_long( int control_time, const osu::beatmap_data_t& map, const schedule_view_t& sched ) const {
            const size_t start = static_cast<size_t>( std::max( m_last_completed_index - 1, 0 ) );
            for ( size_t i = start; i < map.objects.size( ); ++i ) {
                const auto& o = map.objects[ i ];
                if ( !is_slider( o ) && !is_spinner( o ) ) continue;
                const int press = sched.press( static_cast<int>( i ), o );
                if ( control_time >= press && control_time <= o.end_time + 8 ) return static_cast<int>( i );
                if ( press > control_time ) break;
            }
            return -1;
        }
        bool dense_neighborhood( int index, const osu::beatmap_data_t& map,
                                 const schedule_view_t& sched, int* useful = nullptr ) const {
            int dense_links = 0, links = 0;
            for ( int look = 1; look <= 5 && index + look < static_cast<int>( map.objects.size( ) ); ++look ) {
                const int a = sched.press( index + look - 1, map.objects[ static_cast<size_t>( index + look - 1 ) ] );
                const int b = sched.press( index + look, map.objects[ static_cast<size_t>( index + look ) ] );
                const int gap = b - a;
                if ( gap <= 0 || gap > 220 ) break;
                ++links;
                if ( gap < 175 ) ++dense_links;
            }
            if ( useful ) *useful = links;
            return dense_links >= 2 || ( dense_links == 1 && links == 1 );
        }

        // Continuous local effort estimate. This never modifies circle targets
        // or their jump planner; only slider freedom consumes this budget.
        float local_motion_pressure( int index, const osu::beatmap_data_t& map,
                                     const schedule_view_t& sched ) const {
            float total = 0.f, weights = 0.f;
            const float rate = std::max(m_game_rate, 0.5f);
            for ( int i = std::max(1,index-2); i <= std::min(index+4,static_cast<int>(map.objects.size())-1); ++i ) {
                const auto& a = map.objects[static_cast<size_t>(i-1)];
                const auto& b = map.objects[static_cast<size_t>(i)];
                const float interval = std::max(1.f, static_cast<float>(sched.press(i,b)-sched.press(i-1,a))/rate);
                const float free_time = std::max(25.f, static_cast<float>(sched.press(i,b)-
                    std::max(a.end_time,sched.press(i-1,a)))/rate);
                const bool visible = perceived(i,map) && perceived(i-1,map);
                const float distance = visible ? length(object_center(b,m_hr_flip)-object_center(a,m_hr_flip)) : 0.f;
                const float cadence = 1.f-smoothstep(100.f,430.f,interval);
                const float travel = smoothstep(250.f,1600.f,distance*1000.f/free_time);
                float turn = 0.f;
                if ( visible && i > 1 && perceived(i-2,map) ) {
                    const point_t incoming = normalized(object_center(a,m_hr_flip)-
                        object_center(map.objects[static_cast<size_t>(i-2)],m_hr_flip));
                    const point_t outgoing = normalized(object_center(b,m_hr_flip)-object_center(a,m_hr_flip));
                    turn = (1.f-dot(incoming,outgoing))*0.5f*cadence;
                }
                const float demand = 1.f-(1.f-0.72f*cadence)*(1.f-0.48f*travel)*(1.f-0.18f*turn);
                const float weight = 1.f/(1.f+0.55f*std::abs(i-index));
                total += demand*weight; weights += weight;
            }
            return weights > 0.f ? saturate(total/weights) : 0.f;
        }

        float break_acquisition_lead( point_t target ) const {
            const point_t delta = target-m_motion.position;
            const float distance = length(delta);
            const point_t direction = normalized(delta);
            const float along = dot(m_motion.velocity,direction);
            const float lateral = length(m_motion.velocity-direction*along);
            const float comfortable_speed = 680.f*m_style.accel;
            const float travel = std::max(distance*1000.f/comfortable_speed,
                2000.f*std::sqrt(distance/(9500.f*m_style.accel)));
            const float braking = std::max(-along,0.f)*0.12f + lateral*0.035f;
            const float acceleration_reserve = std::min(length(m_motion.acceleration)*0.001f,100.f);
            return std::clamp(travel+braking+acceleration_reserve+145.f+
                (settings.perfect_play ? 65.f : 0.f),280.f,1450.f);
        }
        void advance_completed( int control_time, const osu::beatmap_data_t& map, const schedule_view_t& sched ) {
            while ( m_last_completed_index + 1 < static_cast<int>( map.objects.size( ) ) ) {
                const int n = m_last_completed_index + 1;
                if ( control_time < sched.press( n, map.objects[ static_cast<size_t>( n ) ] ) ) break;
                m_last_completed_index = n;
                m_diag.objects_completed++;
            }
        }
        point_t target_point( int index, const osu::beatmap_data_t& map, const schedule_view_t& sched ) {
            if ( index < 0 || index >= static_cast<int>( map.objects.size( ) ) ) return m_motion.position;
            if ( !perceived(index,map) ) return m_motion.position; // never cache a hidden target
            const size_t slot = static_cast<size_t>( index );
            if ( slot < m_target_valid.size( ) && m_target_valid[ slot ] ) return m_target_points[ slot ];
            const osu::hit_object_t& o = map.objects[ slot ];
            point_t p;
            if ( is_spinner( o ) ) {
                // Approach the ORBIT, not the centre: a radial entry point at the
                // orbit radius so the spinner plan can take over with (almost) no
                // first-frame snap.  Radial is taken from the current cursor so
                // the incoming direction is respected; cached write-once.
                const point_t center = object_center( o, m_hr_flip );
                point_t radial = normalized( m_motion.position - center );
                if ( length_sq( radial ) < 0.1f ) radial = { 1.f, 0.f };
                const auto& style=prepared_spinner(index,map);
                const double angle=std::atan2(radial.y,radial.x);
                p = center+radial*spinner_orbit_radius(style,angle,0.0);
            } else if ( !settings.diagnostic_target_variation ) {
                p = object_center( o, m_hr_flip );
            } else {
                const bool dense = dense_neighborhood( index, map, sched );
                const float pattern_scale = settings.perfect_play ? 0.72f : dense ? 0.62f : 1.f;
                p = select_target_point( index, o, m_hr_flip, map.cs, m_seed, settings.aim_spread,
                                         m_style.arrival_depth, m_style.phase, sched.bias( index ),
                                         pattern_scale );
            }
            if ( slot < m_target_points.size( ) ) {
                if ( m_target_valid[ slot ] && length( m_target_points[ slot ] - p ) > 0.0001f )
                    ++m_diag.target_coordinate_changes;
                if ( !m_target_valid[ slot ] ) ++m_diag.target_cache_writes;
                m_target_points[ slot ] = p; m_target_valid[ slot ] = 1;
            }
            return p;
        }
        const spinner_style_t& prepared_spinner(int index,const osu::beatmap_data_t& map) {
            int& slot=m_spinner_style_slots[static_cast<size_t>(index)];
            if(slot<0) {
                const auto& o=map.objects[static_cast<size_t>(index)];
                auto style=make_spinner_style(mix_seed(m_seed^(static_cast<uint32_t>(index+1)*0x9e3779b9u)),
                    (o.end_time-o.start_time)*0.001f/m_game_rate);
                style.range=sanitize_spinner_range(settings.spinner_rpm_min,settings.spinner_rpm_max);
                slot=static_cast<int>(m_spinner_styles.size());m_spinner_styles.push_back(style);
            }
            return m_spinner_styles[static_cast<size_t>(slot)];
        }
        const slider_path_t& slider_path( int index, const osu::hit_object_t& o ) {
            static slider_path_t empty{};
            if ( index < 0 || index >= static_cast<int>( m_slider_cache.size( ) ) ) return empty;
            auto& slot = m_slider_cache[ static_cast<size_t>( index ) ];
            if ( slot.start_time == o.start_time ) return slot;
            slot = build_slider_path( o, m_hr_flip );
            return slot;
        }

        struct local_slider_coverage_t {
            point_t anchor{};
            float max_distance = 0.f;
            float speed = 0.f;
            bool safe = false;
        };

        struct whole_slider_coverage_t {
            point_t anchor{};
            float max_distance = 0.f;
            float head_max_distance = 0.f;
            float path_length = 0.f;
            bool safe = false, head_safe = false;
        };

        whole_slider_coverage_t whole_slider_coverage( const slider_path_t& sp,
                                                        const osu::hit_object_t& o,
                                                        float safe_follow ) const {
            whole_slider_coverage_t out{};
            if ( sp.empty( ) || o.end_time <= o.start_time ) return out;
            // All polyline vertices cover the entire spatial path, including
            // every repeat. Uniform time sampling can alias tiny repeated loops.
            const auto& points = sp.path;
            point_t lo = points.front(), hi = lo, centroid{};
            for ( const point_t p : points ) {
                centroid += p;
                lo.x = std::min( lo.x, p.x ); lo.y = std::min( lo.y, p.y );
                hi.x = std::max( hi.x, p.x ); hi.y = std::max( hi.y, p.y );
            }
            out.path_length = sp.total*std::max(o.slider_repeat,1);
            centroid = centroid / static_cast<float>( points.size() );
            std::array<point_t, 3> candidates{ ( lo + hi ) * 0.5f, centroid,
                ( points.front() + points.back() ) * 0.5f };
            float best = 100000.f;
            for ( const point_t candidate : candidates ) {
                float maximum = 0.f;
                for ( const point_t p : points ) maximum = std::max( maximum, length( p - candidate ) );
                if ( maximum < best ) { best = maximum; out.anchor = candidate; }
            }
            out.max_distance = best;
            const float hit = safe_follow / 1.85f;
            const float margin = hit * 0.18f;
            float head_maximum = 0.f;
            for ( const point_t p : points ) head_maximum = std::max( head_maximum, length( p - points.front() ) );
            out.head_max_distance = head_maximum;
            out.head_safe = head_maximum + margin < safe_follow * 0.92f;
            out.safe = out.max_distance + margin < safe_follow * 0.80f;
            return out;
        }

        local_slider_coverage_t local_slider_coverage( const slider_path_t& sp,
                                                       const osu::hit_object_t& o,
                                                       int control_time, float safe_follow,
                                                       float allowance, float horizon_real_ms,
                                                       point_t preferred ) const {
            local_slider_coverage_t out{};
            if ( sp.empty( ) ) return out;
            constexpr int samples = 17;
            int horizon_game_ms = std::max( 8, static_cast<int>( std::lround( horizon_real_ms * m_game_rate ) ) );
            const int begin = std::clamp( control_time, o.start_time, o.end_time );
            std::array<point_t, samples> points{};
            for ( int attempt = 0; attempt < 5; ++attempt ) {
                const int end = std::min(o.end_time,begin+horizon_game_ms);
                for ( int i = 0; i < samples; ++i ) {
                    const float u = i/static_cast<float>(samples-1);
                    points[static_cast<size_t>(i)] = eval_slider_ball(sp,o,begin+
                        static_cast<int>(std::lround((end-begin)*u)));
                }
                point_t candidate = preferred;
                if ( project_to_follow_region(candidate,points.data(),points.size(),allowance) ) {
                    out.anchor = candidate; out.safe = attempt == 0;
                    break;
                }
                horizon_game_ms = std::max(4,horizon_game_ms/2);
                out.anchor = points.front();
            }
            for ( const point_t p : points ) out.max_distance = std::max( out.max_distance, length( p - out.anchor ) );

            const int sample_ms = std::max( 4, static_cast<int>( std::lround( 12.f * m_game_rate ) ) );
            const int tm = std::max( o.start_time, begin - sample_ms );
            const int tp = std::min( o.end_time, begin + sample_ms );
            const float real_seconds = std::max( ( tp - tm ) * 0.001f / std::max( m_game_rate, 0.5f ), 0.001f );
            out.speed = length( eval_slider_ball( sp, o, tp ) - eval_slider_ball( sp, o, tm ) ) / real_seconds;
            out.safe = out.safe && out.max_distance < safe_follow*0.74f;
            return out;
        }

        // ---- planner ------------------------------------------------------
        bool perceived( int index, const osu::beatmap_data_t& map ) const {
            return index>=0 && index<static_cast<int>(map.objects.size()) &&
                m_perception_time >= map.objects[static_cast<size_t>(index)].start_time-object_preempt_ms(map.ar,m_hr_flip);
        }
        double state_start_time( int control_time ) const {
            // New curves start at the time represented by their captured state.
            // Starting at *this* callback emitted the same position twice, then
            // reaccelerated next frame despite apparently continuous stored p/v/a.
            return m_have_state_time && control_time>=m_state_time && control_time-m_state_time<=50
                ? m_state_time : control_time;
        }
        void make_or_keep_unperceived( int index, int arrival, int control_time ) {
            if ( m_plan.valid && m_plan.object_index==index && m_plan.waiting_for_perception ) return;
            plan_t p{};p.id=m_next_plan_id++;p.type=plan_type_t::gameplay;p.valid=true;p.object_index=index;
            p.t_start=state_start_time(control_time);p.t_end=arrival;p.arrival_key=arrival;
            p.p0=m_motion.position;p.v0=m_motion.velocity;p.a0=m_motion.acceleration;
            p.waiting_for_perception=true;p.initial_commitment=0.f;
            p.continuation_response=std::clamp(0.08f+length(p.v0)/10000.f,0.08f,0.18f);
            if(length(p.a0)>6000.f) p.continuation_response=0.045f;
            p.p1=p.p0; // no hidden position, orientation, endpoint, or RNG
            register_new_plan(p,"unperceived-local-continuation");++m_diag.gameplay_plans;
        }
        void plan_and_reconcile( int control_time, const osu::beatmap_data_t& map, const schedule_view_t& sched ) {
            m_diag.break_active = false;
            m_diag.break_acquiring = false;
            const int active = find_active_long( control_time, map, sched );
            if ( active >= 0 && perceived(active,map) ) {
                const auto& o = map.objects[ static_cast<size_t>( active ) ];
                if ( is_spinner( o ) ) make_or_keep_spinner( active, o, map, control_time );
                else                   make_or_keep_slider( active, o, map, control_time, sched );
                m_decor.active = false;
                return;
            }
            if ( m_slider_runtime.active ) finalize_slider( map, control_time );
            if ( m_spinner.active ) finalize_spinner( );
            m_spinner.active = false;

            const int target = next_object_index( );
            if ( target < 0 ) { m_decor.active = false; return; }

            const auto& o = map.objects[ static_cast<size_t>( target ) ];
            const int arrival = sched.press( target, o );
            m_diag.target_perceived=perceived(target,map);
            m_diag.target_perception_time=o.start_time-object_preempt_ms(map.ar,m_hr_flip);
            if ( !m_diag.target_perceived ) {
                m_diag.target_commitment=0.f;
                const float real_left=static_cast<float>(arrival-control_time)/m_game_rate;
                bool idle_break=false;
                if(m_last_completed_index>=0) {
                    const auto& previous=map.objects[static_cast<size_t>(m_last_completed_index)];
                    idle_break=(arrival-std::max(previous.end_time,sched.press(m_last_completed_index,previous)))/m_game_rate>=2400.f;
                }
                const bool startup=m_last_completed_index<0 && settings.startup_motion;
                if(idle_break || startup) {
                    m_diag.break_active=idle_break;
                    // Timing can constrain a gesture's budget; hidden distance
                    // cannot change its family, scale, direction or duration.
                    make_or_keep_decor(startup,control_time,std::max(0.f,real_left-object_preempt_ms(map.ar,m_hr_flip)/m_game_rate),
                        idle_break && !settings.break_motion);
                } else make_or_keep_unperceived(target,arrival,control_time);
                return;
            }
            const point_t tp = target_point( target, map, sched );
            const float time_left = static_cast<float>( arrival - control_time );

            if ( time_left <= 0.f ) {
                // A healthy active acquisition must be allowed to evaluate its
                // exact endpoint before completion advances below.  Replacing it
                // with RECOVERY for this one frame created a periodic fast kink.
                const bool arriving_normally = m_plan.valid && m_plan.object_index == target &&
                    ( m_plan.type == plan_type_t::gameplay || m_plan.type == plan_type_t::recovery ) &&
                    std::abs( m_plan.arrival_key - static_cast<double>( arrival ) ) <= 12.0;
                if ( arriving_normally ) return;
                make_or_keep_gameplay( plan_type_t::recovery, target, arrival, tp, control_time, map, sched );
                m_decor.active = false;
                return;
            }
            const float lead = std::max( acquisition_required_ms( m_motion.position, m_motion.velocity,
                m_motion.acceleration, tp, m_style.accel ), m_style.motor_response_ms );
            const bool startup = m_last_completed_index < 0 && settings.startup_motion;
            bool genuine_break = false;
            if ( m_last_completed_index >= 0 ) {
                const auto& previous = map.objects[ static_cast<size_t>( m_last_completed_index ) ];
                const int previous_done = std::max( previous.end_time,
                    sched.press( m_last_completed_index, previous ) );
                const float real_gap_ms = static_cast<float>( arrival - previous_done ) /
                    std::max( m_game_rate, 0.5f );
                // Spare acquisition time in a slow pattern is not a map break.
                // Decorative ownership requires a genuine multi-second idle gap.
                genuine_break = real_gap_ms >= 2400.f;
            }
            m_diag.break_active = genuine_break;
            if ( genuine_break ) {
                if ( m_break_target != target ) { m_break_target = target; m_break_acquiring = false; }
                const float real_left = time_left/std::max(m_game_rate,0.5f);
                const float natural_lead = break_acquisition_lead(tp);
                m_diag.break_required_lead_ms = natural_lead;
                if ( !m_break_acquiring && real_left > natural_lead ) {
                    m_diag.break_acquiring = false;
                    // Break ownership is independent of whether visual dances
                    // are enabled. Disabled means a quiet local hold, not a
                    // five-second gameplay plan toward the distant next circle.
                    make_or_keep_decor(false,control_time,real_left-natural_lead,!settings.break_motion);
                    return;
                }
                if ( !m_break_acquiring ) m_diag.break_acquisition_lead_ms = real_left;
                m_break_acquiring = true; m_diag.break_acquiring = true;
            } else {
                m_diag.break_acquiring = false;
                const float safety = 260.f + ( settings.perfect_play ? 140.f : 0.f );
                if ( startup && time_left > lead+safety+120.f ) {
                    make_or_keep_decor(true,control_time,time_left-lead-safety);
                    return;
                }
            }

            if ( m_decor.active ) {
                m_diag.decoration_canceled_for_acquisition++;
                m_decor.active = false;
            }

            make_or_keep_gameplay( plan_type_t::gameplay, target, arrival, tp, control_time, map, sched );
            m_decor.active = false;
        }

        void make_or_keep_gameplay( plan_type_t type, int index, int arrival, point_t target,
                                    int control_time, const osu::beatmap_data_t& map, const schedule_view_t& sched ) {
            const bool promoting = m_plan.valid && m_plan.object_index==index && m_plan.waiting_for_perception;
            const bool same = !promoting && m_plan.valid && m_plan.type == type && m_plan.object_index == index &&
                              std::abs( m_plan.arrival_key - static_cast<double>( arrival ) ) <= 12.0;
            if ( same ) return;

            plan_t p{};
            p.id = promoting ? m_plan.id : m_next_plan_id++;
            p.type = type;
            p.object_index = index;
            p.valid = true;
            p.t_start = state_start_time(control_time);
            p.t_end = static_cast<double>( std::max( arrival, control_time + 1 ) );
            p.arrival_key = static_cast<double>( arrival );
            p.p0 = m_motion.position; p.v0 = m_motion.velocity; p.a0 = m_motion.acceleration;
            p.p1 = target;
            p.slider_exit_entry = m_plan.valid && (m_plan.type == plan_type_t::slider || m_plan.type==plan_type_t::spinner ||
                (m_plan.slider_exit_entry && m_plan.object_index == index));
            p.entry_response_seconds = std::clamp(0.045f+length(p.v0)/16000.f,0.045f,0.12f);
            p.v1 = arrival_velocity( index, target, type, map, sched );
            if(is_spinner(map.objects[static_cast<size_t>(index)])) {
                p.spinner_entry=true;
                const point_t center=object_center(map.objects[static_cast<size_t>(index)],m_hr_flip);
                const auto& style=prepared_spinner(index,map);
                const auto entry=spinner_entry_kinematics(style,center,std::atan2(target.y-center.y,target.x-center.x),
                    m_session_spinner_direction,style.range);
                p.v1=entry.velocity;p.a1=entry.acceleration;
            }
            const float available=std::max(1.f,static_cast<float>(p.t_end-p.t_start)/m_game_rate);
            const float required_ms=acquisition_required_ms(p.p0,p.v0,p.a0,p.p1,m_style.accel);
            p.initial_commitment=smoothstep(0.40f,1.f,required_ms/available);
            if(promoting || m_plan.type==plan_type_t::break_decor || m_plan.type==plan_type_t::startup)
                p.commitment_softness=1.f-p.initial_commitment;
            const bool dense = dense_neighborhood( index, map, sched );
            const float seconds = std::max( ( arrival - control_time ) * 0.001f, 0.02f );
            const float required = length( target - m_motion.position ) / seconds;
            if ( type == plan_type_t::recovery ) {
                p.max_speed = std::clamp( required * 2.4f + 900.f, 1600.f, 7000.f );
                p.max_accel = std::clamp( required * 10.f + 14000.f, 16000.f, 60000.f );
                p.v1 = p.v1 * 0.4f;
            } else {
                p.max_speed = std::clamp( required * 2.1f + 500.f, 1200.f, 6200.f );
                p.max_accel = std::clamp( required * 8.f + 9000.f, 12000.f, 52000.f );
            }
            if ( !is_spinner(map.objects[static_cast<size_t>(index)]) && settings.diagnostic_safe_arrival_cone && type == plan_type_t::gameplay && trajectory_overshoots_target( p,
                    hit_radius( map.cs, m_hr_flip ) * 0.25f ) ) {
                p.v1 = {};
                ++m_diag.arrival_velocity_safety_reductions;
            }
            configure_micro_modifier( p, index, arrival, control_time, map, sched );
            const char* reason = type == plan_type_t::recovery ? "arrival-late" : "target-identity-change";
            if ( type != plan_type_t::recovery && m_plan.valid ) {
                if ( m_plan.type == plan_type_t::slider ) reason = "slider-exit";
                else if ( m_plan.type == plan_type_t::spinner ) reason = "spinner-exit";
                else if ( m_plan.type == plan_type_t::startup ) reason = "startup-exit";
                else if ( m_plan.type == plan_type_t::break_decor ) reason = "break-exit";
                else if ( m_plan.type == plan_type_t::recovery ) reason = "recovery-exit";
            }
            if ( m_plan.valid && m_plan.object_index == index &&
                 std::abs( m_plan.arrival_key - static_cast<double>( arrival ) ) > 12.0 )
                reason = "schedule-arrival-change";
            const bool recovering_from_decor = type == plan_type_t::recovery && m_plan.valid &&
                ( m_plan.type == plan_type_t::startup || m_plan.type == plan_type_t::break_decor );
            if(promoting) {
                capture_transition(m_plan,p,"perception-commit");m_plan=p;m_plan_reason="perception-commit";
                ++m_diag.perception_promotions;
            } else register_new_plan( p, reason );
            m_diag.density_mode = dense;
            if ( dense ) ++m_diag.dense_flow_activations;
            if ( type == plan_type_t::recovery ) {
                m_diag.recovery_plans++;
                if ( dense ) ++m_diag.dense_pattern_recoveries;
                if ( recovering_from_decor ) m_diag.decoration_caused_recovery++;
            }
            else if(!promoting) m_diag.gameplay_plans++;
        }

        void configure_micro_modifier( plan_t& p, int target_index, int arrival, int control_time,
                                       const osu::beatmap_data_t& map, const schedule_view_t& sched ) {
            m_micro.active = false;
            if ( p.type != plan_type_t::gameplay || !settings.diagnostic_micro_motion ||
                 settings.perfect_play || target_index <= 0 ||
                 target_index >= static_cast<int>( map.objects.size() ) ) return;
            const auto& target = map.objects[ static_cast<size_t>( target_index ) ];
            const auto& previous = map.objects[ static_cast<size_t>( target_index - 1 ) ];
            if ( is_slider( target ) || is_spinner( target ) || is_slider( previous ) || is_spinner( previous ) ||
                 dense_neighborhood( target_index, map, sched ) ) return;
            const int previous_done = std::max( previous.end_time, sched.press( target_index - 1, previous ) );
            const float real_gap = static_cast<float>( arrival - previous_done ) / std::max( m_game_rate, 0.5f );
            if ( real_gap < 360.f || real_gap >= 1800.f ) return;
            const float remaining = static_cast<float>( arrival - control_time ) / std::max( m_game_rate, 0.5f );
            const float required = std::max( acquisition_required_ms( p.p0, p.v0, p.a0, p.p1, m_style.accel ),
                                             m_style.motor_response_ms );
            const float spare = remaining - required - 115.f;
            if ( spare < 90.f ) return;
            ++m_diag.eligible_micro_gaps;
            const uint32_t h = mix_seed( m_seed ^ ( static_cast<uint32_t>( target_index + 1 ) * 0x85ebca6bu ) );
            const float chance = 0.20f + 0.30f * smoothstep( 90.f, 520.f, spare );
            if ( static_cast<float>( h & 0x00ffffffu ) / 16777216.f >= chance ) return;
            point_t direction = normalized( p.p1 - p.p0 );
            if ( length_sq( direction ) < 0.1f ) direction = { 1.f, 0.f };
            const float side = ( h & 0x1000000u ) ? 1.f : -1.f;
            p.micro_direction = { -direction.y * side, direction.x * side };
            const float unit = static_cast<float>( ( h >> 8 ) & 0xffffu ) / 65535.f;
            p.micro_amplitude = std::clamp( spare * 0.018f, 3.f, 14.f ) * ( 0.82f + 0.28f * unit );
            p.micro_modifier = true;
            const int count = static_cast<int>( micro_gesture_t::small_oval );
            p.micro_gesture = static_cast<micro_gesture_t>( 1 + static_cast<int>( ( h >> 24 ) % count ) );
            m_micro.active = true; m_micro.target_index = target_index; m_micro.anchor = p.p0;
            m_micro.safe_envelope = p.micro_amplitude; m_micro.gesture = p.micro_gesture;
            m_diag.micro_gesture = p.micro_gesture; m_diag.micro_safe_envelope = p.micro_amplitude;
            m_diag.micro_max_excursion = std::max( m_diag.micro_max_excursion, p.micro_amplitude );
            m_diag.micro_required_acquisition_ms = required; m_diag.micro_available_budget_ms = spare;
            ++m_diag.micro_gestures_activated;
        }

        bool trajectory_overshoots_target( const plan_t& p, float margin ) const {
            const point_t delta = p.p1 - p.p0;
            const float distance = length( delta );
            if ( distance < 0.1f ) return false;
            const point_t direction = delta / distance;
            const float duration = std::max( static_cast<float>( p.t_end - p.t_start ) * 0.001f /
                std::max( m_game_rate, 0.5f ), 0.02f );
            for ( int sample_index = 1; sample_index < 32; ++sample_index ) {
                const float u = sample_index / 32.f;
                const kinematic_t k = eval_acquisition_curve(p,duration,u);
                const float along = dot( k.position - p.p0, direction );
                if ( along > distance + margin && dot( k.velocity, direction ) > 0.f ) return true;
            }
            return false;
        }

        point_t arrival_velocity( int index, point_t target, plan_type_t type,
                                  const osu::beatmap_data_t& map, const schedule_view_t& sched ) {
            if ( type == plan_type_t::recovery || !settings.gameplay_flow ) return {};
            if ( index >= 0 && index < static_cast<int>( map.objects.size( ) ) &&
                 is_spinner( map.objects[ static_cast<size_t>( index ) ] ) ) {
                const point_t center = object_center( map.objects[ static_cast<size_t>( index ) ], m_hr_flip );
                point_t radial = normalized( target - center );
                if ( length_sq( radial ) < 0.1f ) radial = { 1.f, 0.f };
                const point_t tangent{ -radial.y * m_session_spinner_direction,
                                        radial.x * m_session_spinner_direction };
                return tangent * ( 320.f + 100.f * m_style.momentum_ret );
            }
            const int next = index + 1;
            if ( next >= static_cast<int>( map.objects.size( ) ) ) return {};
            if ( !perceived(next,map) ) {
                // Continue the known incoming stroke softly. The hidden object's
                // location/type must not select the arrival tangent.
                return normalized(target-m_motion.position)*std::min(hit_radius(map.cs,m_hr_flip)*2.f,
                    length(target-m_motion.position)*0.22f);
            }
            const point_t ntp = target_point( next, map, sched );
            const int a = sched.press( index, map.objects[ static_cast<size_t>( index ) ] );
            const int b = sched.press( next, map.objects[ static_cast<size_t>( next ) ] );
            const float seconds = std::max( ( b - a ) * 0.001f, 0.035f );
            const point_t delta = ntp - target;
            const float dist = length( delta );
            const float radius = hit_radius( map.cs, m_hr_flip );
            float gain = 1.f;
            if ( dist < radius * 0.45f ) gain = 0.18f;
            point_t dir = normalized( delta );
            // Endpoint velocity must be compatible with the direction used to
            // reach this hit point.  On a reversal, assigning the full outgoing
            // velocity at the current target mathematically forces the quintic
            // to cross beyond the target and return before the hit time.
            if ( settings.diagnostic_safe_arrival_cone ) {
                const point_t incoming = normalized( target - m_motion.position );
                const float turn_alignment = length_sq( incoming ) > 0.01f
                    ? std::clamp( dot( incoming, dir ), -1.f, 1.f ) : 1.f;
                const float tangent_cone_gain = smoothstep( -0.12f, 0.62f, turn_alignment );
                gain *= tangent_cone_gain;
                if ( tangent_cone_gain <= 0.001f ) return {};
            }
            const float carry = std::clamp( settings.momentum, 0.f, 0.95f ) * m_style.momentum_ret;
            const float curve = 0.72f + 0.28f * std::clamp( settings.curve_strength, 0.f, 1.f ) * m_style.curvature;
            const float response_shape = std::clamp( 175.f / m_style.motor_response_ms, 0.88f, 1.12f );
            int useful = 0;
            if ( dense_neighborhood( index, map, sched, &useful ) ) {
                point_t rolling{};
                float total_weight = 0.f;
                point_t previous = target;
                for ( int look = 1; look <= std::min( useful, 5 ); ++look ) {
                    if ( !perceived(index+look,map) ) break;
                    const point_t future = object_center( map.objects[ static_cast<size_t>( index + look ) ], m_hr_flip );
                    const point_t segment = normalized( future - previous );
                    const float weight = 1.f / static_cast<float>( look );
                    rolling += segment * weight; total_weight += weight;
                    previous = future;
                }
                if ( total_weight > 0.f && length_sq( rolling ) > 0.01f ) {
                    const point_t flow_dir = normalized( rolling / total_weight );
                    const float alignment = std::clamp( dot( dir, flow_dir ), -1.f, 1.f );
                    const float rolling_gain = smoothstep( -0.55f, 0.75f, alignment );
                    dir = normalized( dir * ( 0.72f + 0.20f * rolling_gain ) + flow_dir * ( 0.08f + 0.20f * rolling_gain ) );
                    gain *= 0.35f + 0.65f * rolling_gain;
                }
            }
            return limit( dir * ( dist / seconds ) * carry * gain * curve * response_shape, 2200.f );
        }

        void make_or_keep_slider( int index, const osu::hit_object_t& o,
                                  const osu::beatmap_data_t& map, int control_time,
                                  const schedule_view_t& sched ) {
            if ( m_plan.valid && m_plan.type == plan_type_t::slider && m_plan.object_index == index ) return;
            const slider_path_t& sp = slider_path( index, o );
            const point_t ball = sp.empty( ) ? object_center( o, m_hr_flip ) : eval_slider_ball( sp, o, control_time );
            const float radius = hit_radius( map.cs, m_hr_flip );
            const float safe_follow = radius * 1.85f;
            const whole_slider_coverage_t whole = whole_slider_coverage( sp, o, safe_follow );
            const float duration_real_ms = static_cast<float>( std::max( o.end_time - o.start_time, 0 ) ) /
                std::max( m_game_rate, 0.5f );
            const bool compact = whole.safe;
            const float pressure = local_motion_pressure(index,map,sched);
            const float duration_economy = 1.f-smoothstep(220.f,1100.f,duration_real_ms);
            const float ball_speed = whole.path_length*1000.f/std::max(duration_real_ms,1.f);
            const float speed_economy = 0.90f*smoothstep(80.f,300.f,ball_speed);
            // Even moderate averaged pressure means several upcoming movement
            // demands share the same hand. Reserve freedom early, continuously,
            // rather than waiting for every neighboring interval to be dense.
            const float pressure_economy = 1.f-std::exp(-3.f*pressure);
            const float economy = saturate(std::max({duration_economy,pressure_economy,speed_economy})+
                0.12f*std::clamp(settings.slider_laziness,0.f,1.f));
            const float allowance = safe_follow*(0.20f+0.44f*economy);
            point_t hold_anchor = m_motion.position;
            const bool whole_hold = !sp.empty() && whole.max_distance+radius*0.08f < allowance &&
                project_to_follow_region(hold_anchor,sp.path.data(),sp.path.size(),allowance);
            const bool ultra_short = whole_hold && duration_real_ms <= 320.f;

            plan_t p{};
            p.id = m_next_plan_id++;
            p.type = plan_type_t::slider;
            p.object_index = index;
            p.valid = true;
            p.t_start = state_start_time(control_time);
            p.t_end = static_cast<double>( o.end_time );
            p.p0 = m_motion.position; p.v0 = m_motion.velocity; p.a0 = m_motion.acceleration;
            p.entry_offset = m_motion.position - ball;
            p.compact_slider = compact;
            p.ultra_short_slider = ultra_short;
            p.slider_whole_hold = whole_hold;
            p.slider_anchor = whole_hold ? hold_anchor : ball;
            p.slider_hand_allowance = allowance; p.slider_motion_pressure = pressure; p.slider_economy = economy;
            p.slider_ball_path_length = whole.path_length;
            p.slider_safe_radius = safe_follow;
            const uint32_t h = m_seed ^ ( static_cast<uint32_t>( index + 1 ) * 2246822519u );
            const float angle = static_cast<float>( h & 0xffffu ) / 65535.f * 2.f * k_pi;
            const float offset_amount = radius * ( 0.025f + std::clamp( settings.slider_laziness, 0.f, 1.f ) * 0.08f );
            p.slider_offset = point_t{ std::cos( angle ), std::sin( angle ) } * offset_amount;

            int entry_duration = std::min( ultra_short ? 44 : 80, std::max( o.end_time - control_time, 1 ) );
            const int candidates[] = { 80, 64, 48, 32, 20 };
            if ( whole_hold ) {
                p.p1 = p.slider_anchor; p.v1 = {}; p.a1 = {};
            } else for ( const int candidate : candidates ) {
                if ( candidate > o.end_time - control_time ) continue;
                const int candidate_end = control_time + candidate;
                const point_t endpoint = eval_slider_ball( sp, o, candidate_end );
                const int sample = 6;
                const point_t bm = eval_slider_ball( sp, o, std::max( control_time, candidate_end - sample ) );
                const point_t bp = eval_slider_ball( sp, o, std::min( o.end_time, candidate_end + sample ) );
                const float sample_seconds = sample * 0.001f / std::max( m_game_rate, 0.5f );
                const point_t ev = ( bp - bm ) / std::max( sample_seconds * 2.f, 0.001f );
                const point_t ea = ( bp - endpoint * 2.f + bm ) /
                    std::max( sample_seconds * sample_seconds, 0.000001f );
                bool safe = true;
                const float entry_game_ms=static_cast<float>(candidate_end-p.t_start);
                const float duration_seconds = entry_game_ms * 0.001f / std::max( m_game_rate, 0.5f );
                for ( int sample_index = 0; sample_index <= 8; ++sample_index ) {
                    const float u = sample_index / 8.f;
                    const auto k = eval_quintic( p.p0, p.v0, p.a0, endpoint, ev, ea, duration_seconds, u );
                    const int sample_time = static_cast<int>(p.t_start) + static_cast<int>( std::lround( entry_game_ms * u ) );
                    const point_t sample_ball = eval_slider_ball( sp, o, sample_time );
                    if ( length( k.position - sample_ball ) > safe_follow * 0.76f ) { safe = false; break; }
                }
                entry_duration = candidate;
                p.p1 = endpoint; p.v1 = ev; p.a1 = ea;
                if ( safe ) break;
            }
            const int entry_end = std::min( o.end_time, control_time + entry_duration );
            p.arrival_key = static_cast<double>( entry_end );
            if ( length_sq( p.p1 ) < 0.001f ) {
                p.p1 = eval_slider_ball( sp, o, entry_end );
                p.v1 = {}; p.a1 = {};
            }
            register_new_plan( p, "slider-entry" );
            m_diag.slider_plans++;
            m_diag.sliders_started++;
            m_diag.sliders_evaluated++;
            if ( compact ) m_diag.compact_slider_activations++;
            if ( ultra_short ) m_diag.ultra_short_slider_activations++;
            m_slider_runtime = {};
            m_slider_runtime.active = true; m_slider_runtime.object_index = index;
            m_slider_runtime.anchor = p.slider_anchor;
            m_slider_runtime.last_cursor = m_motion.position; m_slider_runtime.have_last_cursor = true;
            m_diag.slider_ball_path_length = whole.path_length;
            m_diag.local_motion_pressure = pressure; m_diag.slider_hand_economy = economy;
            m_diag.slider_min_follow_margin = safe_follow;
            if ( length( m_motion.position - object_center( o, m_hr_flip ) ) > radius * 0.96f ) {
                m_slider_runtime.head_failed = true; m_diag.slider_head_failures++;
            }
            if ( sched.release( index, o ) < o.end_time + 12 ) {
                m_slider_runtime.early_release = true; m_diag.slider_early_releases++;
            }
        }

        void finalize_slider( const osu::beatmap_data_t& map, int control_time ) {
            if ( !m_slider_runtime.active ) return;
            const int index = m_slider_runtime.object_index;
            if ( index >= 0 && index < static_cast<int>( map.objects.size( ) ) ) {
                const auto& o = map.objects[ static_cast<size_t>( index ) ];
                const auto& sp = slider_path( index, o );
                const point_t tail = sp.empty( ) ? object_center( o, m_hr_flip ) : eval_slider_ball( sp, o, o.end_time );
                const int sample = std::max( 4, static_cast<int>( std::lround( 14.f * m_game_rate ) ) );
                const point_t before_tail = sp.empty( ) ? tail : eval_slider_ball( sp, o, std::max( o.start_time, o.end_time - sample ) );
                m_exit_audit_tail = tail;
                m_exit_audit_tangent = normalized( tail - before_tail );
                m_exit_audit_start = control_time;
                m_exit_audit_active = length_sq( m_exit_audit_tangent ) > 0.1f;
                m_diag.slider_outgoing_velocity = m_motion.velocity;
                if ( length( m_motion.position - tail ) > m_plan.slider_safe_radius ) {
                    m_diag.slider_exit_failures++;
                    m_slider_runtime.follow_failed = true;
                }
            }
            if ( !m_slider_runtime.head_failed && !m_slider_runtime.follow_failed && !m_slider_runtime.early_release )
                m_diag.sliders_completed++;
            m_diag.slider_cursor_path_length = m_slider_runtime.cursor_path_length;
            m_diag.slider_ball_path_length = m_plan.slider_ball_path_length;
            m_diag.slider_cursor_ball_ratio = m_plan.slider_ball_path_length > 0.01f
                ? m_slider_runtime.cursor_path_length / m_plan.slider_ball_path_length : 0.f;
            m_slider_runtime.active = false;
        }

        void make_or_keep_spinner( int index, const osu::hit_object_t& o, const osu::beatmap_data_t& map, int control_time ) {
            if ( m_plan.valid && m_plan.type == plan_type_t::spinner && m_plan.object_index == index &&
                 m_spinner.active && m_spinner.object_index == index )
                return;
            const uint32_t spinner_seed = mix_seed( m_seed ^
                ( static_cast<uint32_t>( index + 1 ) * 0x9e3779b9u ) );
            spinner_begin( m_spinner, index, o, m_hr_flip, m_motion,
                spinner_seed, m_session_spinner_direction, &prepared_spinner(index,map) );
            if ( m_have_last_spinner_direction && m_spinner.direction != m_last_spinner_direction )
                ++m_diag.spinner_direction_changes_between_objects;
            m_last_spinner_direction = m_spinner.direction;
            m_have_last_spinner_direction = true;
            plan_t p{};
            p.id = m_next_plan_id++;
            p.type = plan_type_t::spinner;
            p.object_index = index;
            p.valid = true;
            p.t_start = static_cast<double>( control_time );
            p.t_end = static_cast<double>( o.end_time );
            p.p0 = m_motion.position; p.v0 = m_motion.velocity; p.a0 = m_motion.acceleration;
            register_new_plan( p, "spinner-entry" );
            m_diag.spinner_plans++;
            m_diag.spinner_acquisitions++;
            m_diag.spinner_seed = spinner_seed;
            m_diag.spinner_object_index = index;
            m_diag.spinner_speed_multiplier = m_game_rate;
            if ( m_spinner.direction != m_session_spinner_direction ) m_diag.unexpected_spinner_reversals++;
        }

        void finalize_spinner( ) {
            if ( !finite( m_motion.position ) || !finite( m_motion.velocity ) ) m_diag.spinner_exit_issues++;
        }

        void make_or_keep_decor( bool startup, int control_time, float budget_ms, bool quiet = false ) {
            const plan_type_t type = startup ? plan_type_t::startup : plan_type_t::break_decor;
            if ( !m_decor.active || m_plan.type != type || m_decor.quiet != quiet ) {
                m_decor = {}; m_decor.active = true; m_decor.startup = startup; m_decor.anchor = m_motion.position;
                m_decor.quiet = quiet;
                m_decor.budget_ms = budget_ms;
                m_diag.decor_budget_ms = budget_ms;
                if ( !quiet ) choose_decor_motif( );
                start_decor_segment( control_time );
                return;
            }
            m_decor.budget_ms = budget_ms;
            m_diag.decor_budget_ms = budget_ms;
            if ( !quiet && control_time >= m_plan.t_end ) start_decor_segment( control_time );
        }

        void choose_decor_motif( ) {
            const float energy = std::clamp( m_decor.startup ? settings.startup_energy : settings.break_energy, 0.f, 1.f );
            const bool allow_dance = m_decor.budget_ms >= 2850.f && settings.energetic_dances;
            const float dance_probability = 0.34f + 0.56f * energy;
            const int family_begin = allow_dance && m_rng.unit( ) < dance_probability ? 0 : 5;
            const int family_count = 5;
            int chosen = family_begin + static_cast<int>( m_rng.unit( ) * family_count ) % family_count;
            const int count = static_cast<int>( decor_motif_t::count );
            if ( count > 1 && chosen == static_cast<int>( m_last_decor_motif ) ) chosen = ( chosen + 1 ) % count;
            if ( !allow_dance && chosen < 5 ) chosen = 5 + chosen % 5;
            m_diag.previous_decor_motif = m_last_decor_motif;
            m_decor.motif = static_cast<decor_motif_t>( chosen );
            m_last_decor_motif = m_decor.motif;
            const bool dance = decor_is_dance( m_decor.motif );
            if ( dance ) {
                const float room = smoothstep( 2850.f, 6500.f, m_decor.budget_ms );
                m_decor.scale = ( 108.f + room * 68.f ) * ( 0.88f + m_rng.unit( ) * 0.20f ) *
                    ( 0.66f + 0.34f * energy );
                m_diag.dance_motifs++;
            } else {
                const int level = chosen - 5;
                m_decor.scale = ( 8.f + level * 7.f + energy * 13.f ) * ( 0.82f + m_rng.unit( ) * 0.34f );
                m_diag.fidget_motifs++;
            }
            m_decor.rotation = m_rng.unit( ) * 2.f * k_pi;
            m_decor.handedness = m_rng.unit( ) < 0.5f ? -1.f : 1.f;
            m_decor.segment_index = 0;
            m_decor.segment_count = ( dance ? 9 : 6 ) + static_cast<int>( m_rng.unit( ) * 4.f );
            m_decor.bounds_lo = m_decor.bounds_hi = m_motion.position;
            m_decor.bounds_initialized = true;
            m_diag.decor_motif = m_decor.motif;
            m_diag.decor_requested_scale = m_decor.scale;
            m_diag.decor_energy = energy;
            m_diag.decor_budget_ms = m_decor.budget_ms;
            m_diag.decor_bbox_width = m_diag.decor_bbox_height = 0.f;
        }

        point_t motif_point( float t ) const {
            t = saturate( t );
            const float a = 2.f * k_pi * t * m_decor.handedness;
            point_t q{};
            switch ( m_decor.motif ) {
            case decor_motif_t::uneven_circle:
                q = { std::sin( a ) * ( 1.f + 0.10f * std::sin( a * 3.f + m_style.phase ) ),
                      ( 1.f - std::cos( a ) ) * ( 0.78f + 0.08f * std::sin( a + m_style.phase ) ) }; break;
            case decor_motif_t::irregular_figure_eight:
                q = { std::sin( a ) * ( 1.f + 0.12f * t ), 0.62f * std::sin( a * 2.f + 0.12f * std::sin( a ) ) }; break;
            case decor_motif_t::wide_sweep:
                q = { 1.55f * std::sin( k_pi * t ), 0.48f * std::sin( a ) + 0.22f * ( 1.f - std::cos( k_pi * t ) ) }; break;
            case decor_motif_t::lazy_loop:
                q = { std::sin( a ) + 0.20f * std::sin( a * 0.5f ),
                      0.72f * ( 1.f - std::cos( a ) ) + 0.12f * std::sin( a * 2.f ) }; break;
            case decor_motif_t::broad_wave:
                q = { 1.45f * t, 0.52f * std::sin( a * 1.5f ) * ( 0.75f + 0.25f * std::sin( k_pi * t ) ) }; break;
            case decor_motif_t::small_fidget:
                q = { 0.52f * std::sin( a * 2.f ), 0.38f * std::sin( a * 3.f + 0.4f ) - 0.38f * std::sin( 0.4f ) }; break;
            case decor_motif_t::medium_fidget:
                q = { std::sin( a ) * ( 0.65f + 0.2f * std::sin( a * 0.5f ) ), 0.48f * std::sin( a * 2.f ) }; break;
            case decor_motif_t::loose_fidget:
                q = { 0.78f * std::sin( a * 1.5f ), 0.58f * std::sin( a * 2.5f ) }; break;
            case decor_motif_t::directional_wiggle:
                q = { 0.65f * t + 0.42f * std::sin( a * 2.f ), 0.34f * std::sin( a * 1.5f ) }; break;
            case decor_motif_t::curved_fidget:
                q = { std::sin( k_pi * t ) * ( 0.72f + 0.18f * std::sin( a * 2.f ) ),
                      0.52f * std::sin( a * 2.f ) * ( 1.f - 0.35f * t ) }; break;
            default: break;
            }
            q.x *= m_decor.handedness;
            const float c = std::cos( m_decor.rotation ), s = std::sin( m_decor.rotation );
            return { ( q.x * c - q.y * s ) * m_decor.scale,
                     ( q.x * s + q.y * c ) * m_decor.scale };
        }

        void start_decor_segment( int control_time ) {
            const bool startup = m_decor.startup;
            if ( m_decor.quiet ) {
                plan_t p{}; p.id = m_next_plan_id++; p.type = plan_type_t::break_decor; p.valid = true;
                p.t_start = state_start_time(control_time); p.t_end = control_time+std::lround(320.f*m_game_rate);
                p.arrival_key = p.t_end; p.p0 = m_motion.position; p.v0 = m_motion.velocity; p.a0 = m_motion.acceleration;
                p.p1 = clamp_to_playfield(p.p0+limit(p.v0*0.06f+p.a0*0.0005f,14.f));
                register_new_plan(p,"quiet-break-hold"); ++m_diag.break_segments;
                return;
            }
            const float energy = std::clamp( startup ? settings.startup_energy : settings.break_energy, 0.f, 1.f );
            if ( m_decor.segment_index >= m_decor.segment_count ) {
                m_decor.anchor = m_motion.position;
                choose_decor_motif( );
            }
            const float t1 = static_cast<float>( m_decor.segment_index + 1 ) / m_decor.segment_count;
            point_t dest = m_decor.anchor + motif_point( t1 );
            // Keep endpoint/tangent headroom so the quintic itself remains in
            // bounds without an output-side positional clamp.
            dest.x = std::clamp( dest.x, 52.f, 460.f );
            dest.y = std::clamp( dest.y, 48.f, 336.f );
            const bool dance = decor_is_dance( m_decor.motif );
            const float motif_ms = dance ? 2100.f + ( 1.f - energy ) * 700.f
                                         : ( startup ? 900.f : 1250.f ) + ( 1.f - energy ) * 450.f;
            const int duration = std::max( 110, static_cast<int>( motif_ms / m_decor.segment_count *
                (startup ? 1.f : m_game_rate) ) );
            plan_t p{};
            p.id = m_next_plan_id++;
            p.type = type_of_decor( startup );
            p.object_index = -1;
            p.valid = true;
            p.t_start = state_start_time(control_time);
            p.t_end = static_cast<double>( control_time + duration );
            p.arrival_key = p.t_end;
            // Never falsify the entry derivatives.  Calming is expressed by the
            // local endpoint, duration, and endpoint tangent, preserving C0/C1/C2.
            p.p0 = m_motion.position; p.v0 = m_motion.velocity; p.a0 = m_motion.acceleration;
            p.p1 = dest;
            const float eps = 0.002f;
            const point_t tangent = ( motif_point( std::min( 1.f, t1 + eps ) ) -
                                      motif_point( std::max( 0.f, t1 - eps ) ) ) / ( 2.f * eps );
            const float motif_seconds = std::max( motif_ms * 0.001f, 0.25f );
            p.v1 = limit( tangent / motif_seconds, startup ? 210.f : 150.f );
            if ( dest.x <= 58.f && p.v1.x < 0.f ) p.v1.x = 0.f;
            if ( dest.x >= 454.f && p.v1.x > 0.f ) p.v1.x = 0.f;
            if ( dest.y <= 54.f && p.v1.y < 0.f ) p.v1.y = 0.f;
            if ( dest.y >= 330.f && p.v1.y > 0.f ) p.v1.y = 0.f;
            p.max_speed = 1400.f; p.max_accel = 16000.f;
            register_new_plan( p, m_decor.segment_index == 0 ? "decor-motif-start" : "decor-segment" );
            m_decor.active = true;
            m_decor.segments++; m_decor.segment_index++;
            if ( startup ) m_diag.startup_segments++;
            else m_diag.break_segments++;
        }
        static plan_type_t type_of_decor( bool startup ) { return startup ? plan_type_t::startup : plan_type_t::break_decor; }

        void register_new_plan( const plan_t& p, const char* reason ) {
            const plan_t previous = m_plan;
            const bool was_valid = previous.valid;
            if ( was_valid ) {
                capture_transition( previous, p, reason );
                m_diag.plan_transitions++;
            }
            if ( p.object_index >= 0 ) {
                if ( m_plan_count_target >= 0 && p.object_index != m_plan_count_target ) {
                    ++m_diag.transition_intervals;
                    m_diag.transition_plan_sum += m_plans_for_target;
                    m_diag.max_plans_per_transition = std::max( m_diag.max_plans_per_transition,
                        m_plans_for_target );
                    // A slider legitimately owns one acquisition plan plus one
                    // follow plan. Three or more intents for the same object is
                    // the mini-plan chain this diagnostic is meant to expose.
                    if ( m_plans_for_target > 2 ) ++m_diag.excessive_plan_intervals;
                    m_plans_for_target = 0;
                }
                if ( p.object_index != m_plan_count_target ) m_plan_count_target = p.object_index;
                ++m_plans_for_target;
            }
            m_plan = p;
            m_plan_reason = reason;
            m_diag.plans_created++;
            m_diag.plan_id = p.id;
            m_diag.last_transition_reason = reason;
        }

        // ---- evaluate -----------------------------------------------------
        eval_result_t evaluate_plan( int control_time, float dt, const osu::beatmap_data_t& map,
                                     const schedule_view_t& sched ) {
            eval_result_t r{};
            r.position = m_motion.position; r.velocity = m_motion.velocity; r.acceleration = m_motion.acceleration;
            if ( !m_plan.valid ) { r.reason = "no-plan"; return r; }
            switch ( m_plan.type ) {
            case plan_type_t::slider:  return eval_slider( control_time, dt, map, sched );
            case plan_type_t::spinner: return eval_spinner( control_time, dt, map );
            default:                   return eval_trajectory( control_time );
            }
        }
        eval_result_t eval_trajectory( int control_time ) {
            eval_result_t r{};
            if ( m_plan.waiting_for_perception ) {
                const float t=std::max(0.f,static_cast<float>(control_time-m_plan.t_start)*0.001f/m_game_rate);
                const float tau=m_plan.continuation_response,e=std::exp(-t/tau);
                const point_t c=m_plan.a0+m_plan.v0/tau;
                r.position=m_plan.p0+m_plan.v0*(tau*(1.f-e))+c*(tau*tau*(1.f-(1.f+t/tau)*e));
                r.velocity=(m_plan.v0+c*t)*e;r.acceleration=(m_plan.a0-c*(t/tau))*e;
                r.reason="UNPERCEIVED-CONTINUATION";return r;
            }
            const double dur_ms = std::max( m_plan.t_end - m_plan.t_start, 1.0 );
            const float u = saturate( static_cast<float>( ( control_time - m_plan.t_start ) / dur_ms ) );
            const float dur_sec = std::max( static_cast<float>( dur_ms ) * 0.001f / std::max( m_game_rate, 0.5f ), 0.02f );
            kinematic_t k = eval_acquisition_curve(m_plan,dur_sec,u);
            if ( m_plan.micro_modifier ) {
                const float u2 = u * u, u3 = u2 * u, u4 = u3 * u, u5 = u4 * u, u6 = u5 * u;
                const float bump = 64.f * ( u3 - 3.f * u4 + 3.f * u5 - u6 );
                const float bump_d1 = 64.f * ( 3.f * u2 - 12.f * u3 + 15.f * u4 - 6.f * u5 );
                const float bump_d2 = 64.f * ( 6.f * u - 36.f * u2 + 60.f * u3 - 30.f * u4 );
                k.position += m_plan.micro_direction * ( m_plan.micro_amplitude * bump );
                k.velocity += m_plan.micro_direction * ( m_plan.micro_amplitude * bump_d1 / dur_sec );
                k.acceleration += m_plan.micro_direction * ( m_plan.micro_amplitude * bump_d2 /
                    ( dur_sec * dur_sec ) );
            }
            r.position = k.position;
            // Store the TRUE analytic derivatives so the continuity budget in
            // commit_motion is self-consistent with the actual position delta.
            // (Speed is shaped by the trajectory endpoints/duration, not by a
            // post-hoc clamp that would desync velocity from position.)
            r.velocity = k.velocity;
            r.acceleration = k.acceleration;
            r.dynamic = false;
            r.reason = plan_type_name( m_plan.type );
            return r;
        }
        void record_slider_hand( point_t hand, point_t ball ) {
            if ( m_slider_runtime.have_last_cursor )
                m_slider_runtime.cursor_path_length += length(hand-m_slider_runtime.last_cursor);
            m_slider_runtime.last_cursor = hand; m_slider_runtime.have_last_cursor = true;
            const float margin = m_plan.slider_safe_radius-length(hand-ball);
            m_slider_runtime.min_follow_margin = std::min(m_slider_runtime.min_follow_margin,margin);
            m_diag.slider_min_follow_margin = m_slider_runtime.min_follow_margin;
            if ( margin < m_plan.slider_safe_radius*0.10f && !m_slider_runtime.follow_failed ) {
                m_slider_runtime.follow_failed = true; ++m_diag.slider_follow_failures;
            }
        }

        eval_result_t eval_slider( int control_time, float dt, const osu::beatmap_data_t& map,
                                   const schedule_view_t& sched ) {
            eval_result_t r{};
            const auto& o = map.objects[ static_cast<size_t>( m_plan.object_index ) ];
            const slider_path_t& sp = slider_path( m_plan.object_index, o );
            if ( control_time <= static_cast<int>( m_plan.arrival_key ) ) {
                const double dur_ms = std::max( m_plan.arrival_key - m_plan.t_start, 1.0 );
                const float u = saturate( static_cast<float>( ( control_time - m_plan.t_start ) / dur_ms ) );
                const float dur_sec = std::max( static_cast<float>( dur_ms ) * 0.001f /
                    std::max( m_game_rate, 0.5f ), 0.02f );
                const kinematic_t k = eval_quintic( m_plan.p0, m_plan.v0, m_plan.a0,
                    m_plan.p1, m_plan.v1, m_plan.a1, dur_sec, u );
                r.position = k.position; r.velocity = k.velocity; r.acceleration = k.acceleration;
                record_slider_hand(r.position,eval_slider_ball(sp,o,std::min(control_time,o.end_time)));
                r.dynamic = false; r.reason = "SLIDER-ENTRY";
                return r;
            }
            const point_t ball = sp.empty( ) ? object_center( o, m_hr_flip )
                                             : eval_slider_ball( sp, o, std::min( control_time, o.end_time ) );
            const float lazy = std::clamp( settings.slider_laziness, 0.f, 1.f ) * ( 2.f - m_style.slider_tight );
            point_t desired{};
            const float follow_age = std::max( 0.f,
                static_cast<float>( control_time - static_cast<int>( m_plan.arrival_key ) ) * 0.001f );
            const float follow_blend = smoothstep( 0.f, 0.18f, follow_age );
            const float horizon = 35.f+245.f*m_plan.slider_economy;
            const local_slider_coverage_t coverage = local_slider_coverage( sp, o, control_time,
                m_plan.slider_safe_radius, m_plan.slider_hand_allowance, horizon, m_slider_runtime.anchor );
            const float safe_dt = std::max( dt, 0.000001f );
            if ( !m_plan.slider_whole_hold ) m_slider_runtime.anchor = coverage.anchor;
            // This is an explanatory economy indicator, no longer a multiplier
            // blending the cursor back toward the exact moving slider ball.
            m_slider_runtime.anchor_blend = m_plan.slider_whole_hold ? 1.f :
                coverage.safe ? m_plan.slider_economy : 0.f;

            slider_strategy_t strategy = slider_strategy_t::track;
            if ( m_slider_runtime.anchor_blend >= 0.65f ) strategy = slider_strategy_t::anchor;
            else if ( m_slider_runtime.anchor_blend >= 0.12f ) strategy = slider_strategy_t::blend;
            if ( coverage.safe && !m_slider_runtime.safe_section ) {
                ++m_diag.safe_anchor_sections;
            }
            if ( !m_slider_runtime.strategy_initialized || strategy != m_slider_runtime.strategy ) {
                if ( strategy == slider_strategy_t::track ) ++m_diag.normal_follow_sections;
                else if ( m_slider_runtime.strategy_initialized &&
                          m_slider_runtime.strategy == slider_strategy_t::track &&
                          !m_plan.compact_slider ) ++m_diag.partial_anchor_activations;
                m_slider_runtime.strategy_initialized = true;
            }
            m_slider_runtime.safe_section = coverage.safe;
            m_slider_runtime.strategy = strategy;

            const float age = static_cast<float>( control_time - m_plan.t_start ) * 0.001f;
            const float amp = m_plan.slider_whole_hold ? std::min(0.85f,m_plan.slider_safe_radius*0.012f) : 0.f;
            const point_t micro{
                std::sin( m_style.phase + age * 6.1f ) * amp,
                std::sin( m_style.phase * 1.37f + age * 4.7f ) * amp * 0.68f
            };
            const point_t safe_target = m_slider_runtime.anchor + micro*follow_blend;

            // Classify the exit, then make the last part of the slider hand path a
            // single validated segment whose endpoint derivative faces the next
            // acquisition. The previous moving-target follower could finish with
            // the slider ball's large outward velocity. Passing that derivative
            // into a short gameplay quintic forced a visible overshoot-and-return.
            const int next_index = m_plan.object_index + 1;
            const float real_ms_to_tail = static_cast<float>( std::max( o.end_time - control_time, 0 ) ) /
                std::max( m_game_rate, 0.5f );
            point_t tail{}, next_target{};
            slider_exit_strategy_t exit_strategy = slider_exit_strategy_t::curved_departure;
            bool have_next_target = false;
            if ( settings.diagnostic_slider_exit && next_index < static_cast<int>( map.objects.size( ) ) &&
                 real_ms_to_tail <= 240.f && perceived(next_index,map) ) {
                const int tail_sample = std::max( 4, static_cast<int>( std::lround( 14.f * m_game_rate ) ) );
                tail = eval_slider_ball( sp, o, o.end_time );
                point_t tail_tangent = normalized( tail - eval_slider_ball( sp, o,
                    std::max( o.start_time, o.end_time - tail_sample ) ) );
                if ( length_sq( tail_tangent ) < 0.1f ) tail_tangent = normalized( ball - m_motion.position );
                next_target = target_point( next_index, map, sched );
                have_next_target = true;
                const point_t next_dir = normalized( next_target - tail );
                const float alignment = std::clamp( dot( tail_tangent, next_dir ), -1.f, 1.f );
                const float turn = std::abs( tail_tangent.x * next_dir.y - tail_tangent.y * next_dir.x );
                if ( alignment > 0.68f ) {
                    exit_strategy = slider_exit_strategy_t::aligned_carry;
                } else if ( alignment < -0.45f ) {
                    exit_strategy = slider_exit_strategy_t::reversal_brake;
                } else if ( turn < 0.42f ) {
                    exit_strategy = slider_exit_strategy_t::direct_peel;
                }
                m_diag.slider_tail_tangent = tail_tangent;
                m_diag.slider_exit_strategy = exit_strategy;
                m_diag.slider_exit_curvature = turn;
            }

            if ( have_next_target && !m_slider_runtime.handoff_active ) {
                float handoff_window_ms = 180.f;
                switch ( exit_strategy ) {
                case slider_exit_strategy_t::aligned_carry:    handoff_window_ms = 145.f; break;
                case slider_exit_strategy_t::direct_peel:      handoff_window_ms = 175.f; break;
                case slider_exit_strategy_t::curved_departure: handoff_window_ms = 195.f; break;
                case slider_exit_strategy_t::reversal_brake:   handoff_window_ms = 220.f; break;
                }
                if ( m_plan.ultra_short_slider ) handoff_window_ms = std::min( handoff_window_ms, 100.f );
                else if ( m_plan.compact_slider ) handoff_window_ms = std::min( handoff_window_ms, 150.f );

                if ( real_ms_to_tail <= handoff_window_ms && real_ms_to_tail >= 32.f ) {
                    const point_t next_delta = next_target - tail;
                    const int next_arrival = sched.press( next_index,
                        map.objects[ static_cast<size_t>( next_index ) ] );
                    const float next_seconds = std::max( static_cast<float>( next_arrival - o.end_time ) *
                        0.001f / std::max( m_game_rate, 0.5f ), 0.055f );
                    const float urgency = 1.f-smoothstep(0.10f,0.65f,next_seconds);
                    const point_t hand_base = m_plan.slider_whole_hold ? m_slider_runtime.anchor : m_motion.position;
                    point_t endpoint = hand_base + limit(next_target-hand_base,
                        m_plan.slider_safe_radius*(0.025f+0.085f*urgency));
                    if ( m_plan.slider_whole_hold ) {
                        // Do not undo whole-path economy by demanding the exact
                        // tail. Remain in the full-path intersection, using only
                        // a little available freedom toward the next object.
                        if ( !project_to_follow_region(endpoint,sp.path.data(),sp.path.size(),
                                m_plan.slider_hand_allowance) ) endpoint = m_plan.slider_anchor;
                    } else {
                        std::array<point_t,25> remaining{};
                        for ( size_t i=0; i<remaining.size(); ++i ) remaining[i] = eval_slider_ball(sp,o,
                            control_time+static_cast<int>((o.end_time-control_time)*i/(remaining.size()-1)));
                        if ( !project_to_follow_region(endpoint,remaining.data(),remaining.size(),
                                m_plan.slider_safe_radius*0.70f) )
                            endpoint = tail+limit(next_delta,m_plan.slider_safe_radius*0.20f);
                    }
                    const point_t next_dir = normalized(next_target-endpoint);
                    float carry_gain = 0.12f;
                    switch ( exit_strategy ) {
                    case slider_exit_strategy_t::aligned_carry:    carry_gain = 0.22f; break;
                    case slider_exit_strategy_t::direct_peel:      carry_gain = 0.16f; break;
                    case slider_exit_strategy_t::curved_departure: carry_gain = 0.11f; break;
                    case slider_exit_strategy_t::reversal_brake:   carry_gain = 0.06f; break;
                    }
                    const int handoff_start=static_cast<int>(state_start_time(control_time));
                    const float duration_seconds = std::max( static_cast<float>( o.end_time - handoff_start ) *
                        0.001f / std::max( m_game_rate, 0.5f ), 0.001f );
                    const float available_carry = 35.f+0.8f*length(endpoint-m_motion.position)/duration_seconds;
                    const point_t endpoint_velocity = next_dir * std::clamp(
                        length( next_target - endpoint ) / next_seconds * carry_gain, 0.f,
                        std::min(420.f,available_carry) );

                    bool safe_handoff = true;
                    kinematic_t previous_sample{ m_motion.position, m_motion.velocity, m_motion.acceleration };
                    constexpr int handoff_samples = 64;
                    const float sample_dt = duration_seconds / handoff_samples;
                    for ( int sample_index = 1; sample_index <= handoff_samples; ++sample_index ) {
                        const float u = sample_index / static_cast<float>( handoff_samples );
                        const kinematic_t k = eval_quintic( m_motion.position, m_motion.velocity,
                            m_motion.acceleration, endpoint, endpoint_velocity, {}, duration_seconds, u );
                        const int sample_time = handoff_start + static_cast<int>( std::lround(
                            ( o.end_time - handoff_start ) * u ) );
                        const point_t sample_ball = eval_slider_ball( sp, o, sample_time );
                        const float displacement = length( k.position - previous_sample.position );
                        // Use a deliberately tighter sampler budget than the live
                        // discontinuity detector (which has two pixels of input
                        // noise slack). A mathematically sharp terminal spike can
                        // otherwise pass many tiny samples, then jump on an 8 ms
                        // runtime step even though every point remains hit-safe.
                        const float continuity_bound = std::max( length( previous_sample.velocity ),
                            length( k.velocity ) ) * sample_dt + 0.5f * std::max(
                            length( previous_sample.acceleration ), length( k.acceleration ) ) *
                            sample_dt * sample_dt + 0.002f;
                        if ( length( k.position - sample_ball ) > m_plan.slider_safe_radius * 0.74f ||
                             displacement > continuity_bound + 0.01f ) {
                            safe_handoff = false;
                            break;
                        }
                        previous_sample = k;
                    }
                    if ( safe_handoff ) {
                        m_slider_runtime.handoff_active = true;
                        m_slider_runtime.handoff_start = handoff_start;
                        m_slider_runtime.handoff_end = o.end_time;
                        m_slider_runtime.handoff_p0 = m_motion.position;
                        m_slider_runtime.handoff_v0 = m_motion.velocity;
                        m_slider_runtime.handoff_a0 = m_motion.acceleration;
                        m_slider_runtime.handoff_p1 = endpoint;
                        m_slider_runtime.handoff_v1 = endpoint_velocity;
                        m_slider_runtime.handoff_a1 = {};
                        ++m_diag.slider_handoff_activations;
                    } else {
                        ++m_diag.slider_handoff_rejections;
                    }
                }
            }

            if ( m_slider_runtime.handoff_active ) {
                const int handoff_game_ms = std::max( m_slider_runtime.handoff_end -
                    m_slider_runtime.handoff_start, 1 );
                const float u = saturate( static_cast<float>( control_time -
                    m_slider_runtime.handoff_start ) / static_cast<float>( handoff_game_ms ) );
                const float duration_seconds = std::max( handoff_game_ms * 0.001f /
                    std::max( m_game_rate, 0.5f ), 0.001f );
                const kinematic_t k = eval_quintic( m_slider_runtime.handoff_p0,
                    m_slider_runtime.handoff_v0, m_slider_runtime.handoff_a0,
                    m_slider_runtime.handoff_p1, m_slider_runtime.handoff_v1,
                    m_slider_runtime.handoff_a1, duration_seconds, u );
                desired = k.position;
                const float displacement = length( desired - m_motion.position );
                const float live_bound = kinematic_budget( m_motion.velocity, k.velocity,
                    m_motion.acceleration, k.acceleration, safe_dt );
                if ( displacement <= live_bound + 0.01f ) {
                    record_slider_hand(desired,ball);
                    r.position = desired;
                    r.velocity = k.velocity;
                    r.acceleration = k.acceleration;
                    r.dynamic = false;
                    r.reason = "SLIDER-HANDOFF";
                    return r;
                }
                // A rare callback cadence can land between the validation
                // samples. Abandon this optional hand-path optimization instead
                // of emitting a discontinuous terminal step.
                m_slider_runtime.handoff_active = false;
                ++m_diag.slider_handoff_rejections;
            }

            const float response = 0.045f + lazy * 0.030f;
            desired = critically_damped_position( m_motion.position, m_motion.velocity,
                safe_target, safe_dt, response );

            m_plan.slider_anchor = m_slider_runtime.anchor;
            m_plan.slider_anchor_blend = m_slider_runtime.anchor_blend;
            m_plan.slider_local_speed = coverage.speed;
            m_plan.slider_anchor_max_distance = coverage.max_distance;
            m_diag.slider_local_speed = coverage.speed;
            m_diag.slider_anchor_max_distance = coverage.max_distance;
            m_diag.slider_effective_follow_radius = m_plan.slider_safe_radius * 0.82f;
            m_diag.slider_strategy = strategy;
            const float distance_to_ball = length( desired - ball );
            if ( distance_to_ball > m_plan.slider_safe_radius * 0.74f )
                desired = ball + normalized( desired - ball ) * ( m_plan.slider_safe_radius * 0.74f );
            record_slider_hand(desired,ball);
            r.position = desired;
            r.dynamic = true;
            r.reason = "SLIDER";
            return r;
        }
        eval_result_t eval_spinner( int control_time, float dt, const osu::beatmap_data_t& map ) {
            eval_result_t r{};
            const auto& o = map.objects[ static_cast<size_t>( m_plan.object_index ) ];
            if ( !m_spinner.active || m_spinner.object_index != m_plan.object_index )
                spinner_begin( m_spinner, m_plan.object_index, o, m_hr_flip, m_motion,
                    mix_seed( m_seed ^ ( static_cast<uint32_t>( m_plan.object_index + 1 ) * 0x9e3779b9u ) ),
                    m_session_spinner_direction, &prepared_spinner(m_plan.object_index,map) );
            if ( m_spinner.direction != m_session_spinner_direction ) {
                m_diag.unexpected_spinner_reversals++;
                m_spinner.direction = m_session_spinner_direction;
                m_spinner.angular_velocity = std::abs( m_spinner.angular_velocity ) * m_spinner.direction;
            }
            const point_t before = m_motion.position;
            const bool first = m_spinner.first_output;
            r.position = spinner_step_range( m_spinner, o, control_time, dt,
                m_spinner.style.range.minimum,m_spinner.style.range.maximum );
            if ( first ) {
                const float expected = length( m_motion.velocity ) * dt +
                    0.5f * length( m_motion.acceleration ) * dt * dt + 2.f;
                if ( length( r.position - before ) > expected + 0.5f ) ++m_diag.spinner_entry_issues;
                m_spinner.first_output = false;
            }
            m_diag.spinner_min_radius = std::min( m_diag.spinner_min_radius, m_spinner.radius );
            m_diag.spinner_max_radius = std::max( m_diag.spinner_max_radius, m_spinner.radius );
            m_diag.spinner_requested_rpm = m_spinner.requested_rpm;
            m_diag.spinner_rpm_min=m_spinner.range.minimum;m_diag.spinner_rpm_max=m_spinner.range.maximum;
            m_diag.spinner_rpm_low=m_spinner.min_observed_rpm;m_diag.spinner_rpm_high=m_spinner.max_observed_rpm;
            m_diag.spinner_revolutions = m_spinner.revolutions;
            m_diag.spinner_measured_rpm = m_spinner.elapsed_seconds > 0.05f
                ? static_cast<float>(m_spinner.output_revolutions / m_spinner.elapsed_seconds * 60.0) : 0.f;
            m_diag.spinner_major_axis = m_spinner.major_axis;
            m_diag.spinner_minor_axis = m_spinner.minor_axis;
            m_diag.spinner_rotation_degrees = m_spinner.rotation * 180.f / k_pi;
            m_diag.spinner_center_drift = length( m_spinner.center_drift );
            r.dynamic = true;
            r.reason = "SPINNER";
            return r;
        }

        // ---- commit -------------------------------------------------------
        void commit_motion( eval_result_t r, float dt ) {
            const point_t old_p = m_motion.position, old_v = m_motion.velocity, old_a = m_motion.acceleration;
            if ( !finite( r.position ) ) {
                m_diag.unexpected_discontinuities++;
                capture_record( old_p, r.position, 0.f, 0.f, "non-finite" );
                m_last_discontinuity_record = m_last_suspicious_record;
                m_have_discontinuity_record = true;
                return;
            }
            point_t new_v, new_a;
            const float safe_dt = std::max( dt, 0.000001f );
            if ( r.dynamic ) {
                new_v = ( r.position - old_p ) / safe_dt;
                new_a = ( new_v - old_v ) / safe_dt;
            } else {
                new_v = finite( r.velocity ) ? r.velocity : ( r.position - old_p ) / safe_dt;
                new_a = finite( r.acceleration ) ? r.acceleration : point_t{};
            }
            const float disp = length( r.position - old_p );
            const float bound = kinematic_budget( old_v, new_v, old_a, new_a, dt );
            const float jerk = length( new_a - old_a ) / safe_dt;
            m_frame_jerk = jerk;
            if ( disp > bound + 0.01f ) {
                m_diag.unexpected_discontinuities++;
                capture_record( old_p, r.position, disp, bound, r.reason, nullptr, &new_v, &new_a );
                m_last_discontinuity_record = m_last_suspicious_record;
                m_have_discontinuity_record = true;
            } else if ( disp > 6.f && disp > bound * 0.72f ) {
                m_diag.near_discontinuities++;
                capture_record( old_p, r.position, disp, bound, "near-discontinuity", nullptr, &new_v, &new_a );
            }
            m_motion.position = r.position;
            m_motion.velocity = new_v;
            m_motion.acceleration = new_a;
            if ( m_exit_audit_active ) {
                const float along = dot( m_motion.position - m_exit_audit_tail, m_exit_audit_tangent );
                m_diag.slider_post_tail_overshoot = std::max( m_diag.slider_post_tail_overshoot, std::max( along, 0.f ) );
                if ( m_frame_ctrl - m_exit_audit_start > 320 || dot( new_v, m_exit_audit_tangent ) <= 0.f )
                    m_exit_audit_active = false;
            }
            m_diag.max_frame_displacement = std::max( m_diag.max_frame_displacement, disp );
            m_diag.max_speed = std::max( m_diag.max_speed, length( new_v ) );
            m_diag.max_acceleration = std::max( m_diag.max_acceleration, length( new_a ) );
            m_diag.max_jerk = std::max( m_diag.max_jerk, jerk );
            if ( m_plan.type == plan_type_t::startup || m_plan.type == plan_type_t::break_decor )
                m_diag.max_decorative_excursion = std::max( m_diag.max_decorative_excursion,
                    length( m_motion.position - m_decor.anchor ) );
            if ( ( m_plan.type == plan_type_t::startup || m_plan.type == plan_type_t::break_decor ) &&
                 m_decor.bounds_initialized ) {
                m_decor.bounds_lo.x = std::min( m_decor.bounds_lo.x, m_motion.position.x );
                m_decor.bounds_lo.y = std::min( m_decor.bounds_lo.y, m_motion.position.y );
                m_decor.bounds_hi.x = std::max( m_decor.bounds_hi.x, m_motion.position.x );
                m_decor.bounds_hi.y = std::max( m_decor.bounds_hi.y, m_motion.position.y );
                m_diag.decor_bbox_width = m_decor.bounds_hi.x - m_decor.bounds_lo.x;
                m_diag.decor_bbox_height = m_decor.bounds_hi.y - m_decor.bounds_lo.y;
            }
        }

        void update_live_diagnostics( int control_time ) {
            m_diag.plan_type = m_plan.type;
            m_diag.plan_id = m_plan.id;
            m_diag.object_index = m_plan.object_index;
            const double dur = std::max( m_plan.t_end - m_plan.t_start, 1.0 );
            m_diag.plan_progress = m_plan.valid ? saturate( static_cast<float>( ( control_time - m_plan.t_start ) / dur ) ) : 0.f;
            m_diag.target_commitment=m_plan.waiting_for_perception ? 0.f : m_plan.initial_commitment+
                (1.f-m_plan.initial_commitment)*smoothstep(0.f,0.55f,m_diag.plan_progress);
            m_diag.time_to_arrival_ms = m_plan.valid ? static_cast<float>( m_plan.t_end - control_time ) : 0.f;
            m_diag.motion_position = m_motion.position;
            m_diag.motion_velocity = m_motion.velocity;
            m_diag.motion_acceleration = m_motion.acceleration;
            m_diag.speed = length( m_motion.velocity );
            m_diag.acceleration = length( m_motion.acceleration );
            m_diag.target_position = ( m_plan.valid && m_plan.type != plan_type_t::slider && m_plan.type != plan_type_t::spinner )
                ? m_plan.p1 : m_motion.position;
        }

        void capture_transition( const plan_t& previous, const plan_t& next, const char* reason ) {
            flight_record_t t{};
            t.frame = m_frame; t.game_time = m_frame_ctrl; t.dt = m_frame_dt;
            t.previous_plan_id = previous.id; t.previous_plan_type = previous.type;
            t.previous_object_index = previous.object_index;
            t.plan_id = next.id; t.plan_type = next.type; t.object_index = next.object_index;
            t.position = m_motion.position; t.velocity = m_motion.velocity; t.acceleration = m_motion.acceleration;
            t.velocity_after = next.v0; t.acceleration_after = next.a0;
            t.target = next.p1; t.requested = next.p0;
            t.scheduled_arrival = static_cast<float>( next.t_end );
            t.time_remaining = static_cast<float>( next.t_end - m_frame_ctrl );
            t.plan_start = static_cast<float>( next.t_start );
            t.plan_duration = static_cast<float>( next.t_end - next.t_start );
            t.acquisition_required_ms = m_diag.micro_required_acquisition_ms;
            t.acquisition_budget_ms = m_diag.micro_available_budget_ms;
            t.previous_object = m_frame_previous_object; t.next_object = m_frame_next_object;
            t.previous_object_type = m_frame_previous_object_type; t.next_object_type = m_frame_next_object_type;
            t.slider_strategy = m_diag.slider_strategy; t.micro_gesture = m_micro.gesture;
            t.decor_motif = m_decor.motif; t.session_seed = m_seed;
            t.slider_active = m_slider_runtime.active; t.micro_active = m_micro.active;
            t.recovery = next.type == plan_type_t::recovery;
            t.decorative = next.type == plan_type_t::startup || next.type == plan_type_t::break_decor;
            t.reason = reason; t.plan_reason = reason;
            push_record( t );
            m_diag.last_flight_reason = reason;
        }

        void capture_record( point_t before, point_t desired, float disp, float bound, const char* reason,
                             const plan_t* previous = nullptr, const point_t* velocity_after = nullptr,
                             const point_t* acceleration_after = nullptr ) {
            flight_record_t t{};
            t.frame = m_frame; t.game_time = m_frame_ctrl; t.dt = m_frame_dt;
            t.plan_id = m_plan.id; t.plan_type = m_plan.type; t.object_index = m_plan.object_index;
            if ( previous ) {
                t.previous_plan_id = previous->id; t.previous_plan_type = previous->type;
                t.previous_object_index = previous->object_index;
            }
            t.position = before; t.velocity = m_motion.velocity; t.acceleration = m_motion.acceleration;
            t.velocity_after = velocity_after ? *velocity_after : m_motion.velocity;
            t.acceleration_after = acceleration_after ? *acceleration_after : m_motion.acceleration;
            t.target = m_plan.p1; t.requested = desired;
            t.observed_x = m_diag.observed_screen_x; t.observed_y = m_diag.observed_screen_y;
            t.displacement = disp; t.bound = bound;
            t.speed = length( t.velocity_after ); t.acceleration_magnitude = length( t.acceleration_after );
            t.jerk = m_frame_jerk;
            t.scheduled_arrival = static_cast<float>( m_plan.t_end );
            t.time_remaining = static_cast<float>( m_plan.t_end - m_frame_ctrl );
            t.plan_start = static_cast<float>( m_plan.t_start );
            t.plan_duration = static_cast<float>( m_plan.t_end - m_plan.t_start );
            t.acquisition_required_ms = m_diag.micro_required_acquisition_ms;
            t.acquisition_budget_ms = m_diag.micro_available_budget_ms;
            t.previous_object = m_frame_previous_object; t.next_object = m_frame_next_object;
            t.previous_object_type = m_frame_previous_object_type; t.next_object_type = m_frame_next_object_type;
            t.slider_strategy = m_diag.slider_strategy; t.micro_gesture = m_micro.gesture;
            t.decor_motif = m_decor.motif; t.session_seed = m_seed;
            t.slider_active = m_slider_runtime.active; t.micro_active = m_micro.active;
            t.recovery = m_plan.type == plan_type_t::recovery;
            t.external_reanchor = m_reanchor_flag;
            t.decorative = m_plan.type == plan_type_t::startup || m_plan.type == plan_type_t::break_decor;
            t.reason = reason; t.plan_reason = m_plan_reason;
            push_record( t );
            m_last_suspicious_record = t;
            m_have_suspicious_record = true;
            m_diag.last_flight_reason = reason;
        }

        void capture_output_sample( point_t previous, point_t current, float displacement ) {
            flight_record_t t{};
            t.frame = m_frame; t.game_time = m_frame_ctrl; t.dt = m_frame_dt;
            t.plan_id = m_plan.id; t.plan_type = m_plan.type; t.object_index = m_plan.object_index;
            t.position = previous; t.requested = current;
            t.velocity = m_motion.velocity; t.acceleration = m_motion.acceleration;
            t.velocity_after = m_motion.velocity; t.acceleration_after = m_motion.acceleration;
            t.target = m_plan.p1;
            t.observed_x = m_diag.observed_screen_x; t.observed_y = m_diag.observed_screen_y;
            t.displacement = displacement; t.speed = length( m_motion.velocity );
            t.acceleration_magnitude = length( m_motion.acceleration ); t.jerk = m_frame_jerk;
            t.scheduled_arrival = static_cast<float>( m_plan.t_end );
            t.time_remaining = static_cast<float>( m_plan.t_end - m_frame_ctrl );
            t.plan_start = static_cast<float>( m_plan.t_start );
            t.plan_duration = static_cast<float>( m_plan.t_end - m_plan.t_start );
            t.acquisition_required_ms = m_diag.micro_required_acquisition_ms;
            t.acquisition_budget_ms = m_diag.micro_available_budget_ms;
            t.previous_object = m_frame_previous_object; t.next_object = m_frame_next_object;
            t.previous_object_type = m_frame_previous_object_type; t.next_object_type = m_frame_next_object_type;
            t.slider_strategy = m_diag.slider_strategy; t.micro_gesture = m_micro.gesture;
            t.decor_motif = m_decor.motif; t.session_seed = m_seed;
            t.slider_active = m_slider_runtime.active; t.micro_active = m_micro.active;
            t.recovery = m_plan.type == plan_type_t::recovery;
            t.external_reanchor = m_reanchor_flag;
            t.decorative = m_plan.type == plan_type_t::startup || m_plan.type == plan_type_t::break_decor;
            t.reason = "output-sample"; t.plan_reason = m_plan_reason;
            push_record( t );
        }

        void push_record( const flight_record_t& t ) {
            m_rec[ m_rec_head ] = t;
            m_rec_head = ( m_rec_head + 1 ) % k_rec_capacity;
            ++m_rec_count;
        }
    };

} // namespace autobot
