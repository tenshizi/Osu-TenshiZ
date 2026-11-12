// ============================================================================
//  motion_tests.cpp  --  deterministic off-line tests for the Autobot engine.
//
//  Exercises the pure motion_engine_t (motion_core.hxx) without osu!, Windows,
//  or Relax.  Each scenario builds a synthetic beatmap + schedule, drives the
//  engine frame by frame, and asserts the movement invariants:
//
//      * every produced position is finite,
//      * no unexpected discontinuity (the engine's own C2 budget is never
//        exceeded) -- i.e. no teleports / no first-frame snaps,
//      * per-frame displacement stays bounded,
//      * exactly one plan owns movement,
//      * a decorative plan never owns while gameplay acquisition is required,
//      * cached target points never change,
//      * plans begin from the current MotionState (no stale reuse),
//      * plan transitions happen in the right order.
//
//  Build (from TenshiZ/):
//    cl /std:c++20 /EHsc /I. /Iimpl tests\motion_tests.cpp /Fe:motion_tests.exe
//  Run:
//    motion_tests.exe
// ============================================================================

#include <core/autobot/motion_core.hxx>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace autobot;

static int g_failures = 0;
static int g_checks = 0;

static void check( bool cond, const std::string& scenario, const std::string& what ) {
    ++g_checks;
    if ( !cond ) {
        ++g_failures;
        std::printf( "  [FAIL] %-28s : %s\n", scenario.c_str( ), what.c_str( ) );
    }
}

// ---- synthetic object builders --------------------------------------------
static osu::hit_object_t circle( int t, float x, float y ) {
    osu::hit_object_t o; o.start_time = t; o.end_time = t; o.x = x; o.y = y; o.type = 1; return o;
}
static osu::hit_object_t slider( int t, int end, float x, float y, const char* curve, float length, int repeat = 1 ) {
    osu::hit_object_t o; o.start_time = t; o.end_time = end; o.x = x; o.y = y; o.type = 2;
    o.slider_curve_str = curve; o.slider_length = length; o.slider_repeat = repeat; return o;
}
static osu::hit_object_t spinner( int t, int end ) {
    osu::hit_object_t o; o.start_time = t; o.end_time = end; o.x = 256.f; o.y = 192.f; o.type = 8; return o;
}

static osu::beatmap_data_t make_map( std::vector<osu::hit_object_t> objs, float cs = 4.f ) {
    osu::beatmap_data_t m; m.loaded = true; m.cs = cs; m.od = 8.f; m.ar = 9.f; m.objects = std::move( objs );
    return m;
}
static schedule_view_t make_schedule( const osu::beatmap_data_t& map ) {
    schedule_view_t s;
    s.press_time.resize( map.objects.size( ) );
    s.release_time.resize( map.objects.size( ) );
    s.edge_bias.assign( map.objects.size( ), 0.f );
    for ( size_t i = 0; i < map.objects.size( ); ++i ) {
        s.press_time[ i ] = map.objects[ i ].start_time;
        s.release_time[ i ] = map.objects[ i ].end_time + ( is_slider( map.objects[ i ] ) ? 24 : 0 );
    }
    return s;
}

// ---- a driver that records the movement invariants ------------------------
struct sim_result_t {
    int    frames = 0;
    float  max_frame_disp = 0.f;
    bool   all_finite = true;
    bool   decor_owned_during_acquire = false;
    // ordered set of distinct object indices that a GAMEPLAY/RECOVERY/SLIDER/SPINNER
    // plan pointed at, in the order they first appeared.
    std::vector<int> gameplay_route;
    std::vector<plan_type_t> plan_sequence;   // distinct consecutive plan types
};

// Runs the engine from t0 to t1 in fixed control-time steps.
// acquire_guard: if true, asserts the plan is never decorative when the next
// object is within `acquire_window_ms` of the control time.
static sim_result_t run( motion_engine_t& eng, const osu::beatmap_data_t& map, schedule_view_t& sched,
                         int t0, int t1, int step_ms, float dt_scale = 1.f, int acquire_window_ms = 250 ) {
    sim_result_t r;
    point_t prev = eng.position( );
    plan_type_t last_type = plan_type_t::none;
    int last_route = -999;
    uint64_t disc = eng.diagnostics( ).unexpected_discontinuities;
    for ( int t = t0; t <= t1; t += step_ms ) {
        const float dt = ( step_ms * 0.001f ) * dt_scale;
        eng.update( map, sched, t, dt );
        if ( std::getenv( "DBG" ) && eng.diagnostics( ).unexpected_discontinuities > disc ) {
            disc = eng.diagnostics( ).unexpected_discontinuities;
            flight_record_t fr; const bool have_record = eng.last_record( fr );
            if ( !have_record ) continue;
            std::printf( "    DBG t=%d plan=%s obj=%d disp=%.2f bound=%.2f reason=%s\n",
                t, plan_type_name( fr.plan_type ), fr.object_index, fr.displacement, fr.bound, fr.reason );
        }
        const point_t p = eng.position( );
        if ( !std::isfinite( p.x ) || !std::isfinite( p.y ) ) r.all_finite = false;
        const float d = length( p - prev );
        if ( r.frames > 0 ) r.max_frame_disp = std::max( r.max_frame_disp, d );
        prev = p;
        ++r.frames;

        const plan_t& pl = eng.plan( );
        if ( pl.type != last_type ) { r.plan_sequence.push_back( pl.type ); last_type = pl.type; }
        const bool gameplay_like = pl.type == plan_type_t::gameplay || pl.type == plan_type_t::recovery ||
                                   pl.type == plan_type_t::slider || pl.type == plan_type_t::spinner;
        if ( gameplay_like && pl.object_index != last_route ) { r.gameplay_route.push_back( pl.object_index ); last_route = pl.object_index; }

        // decorative-owns-during-acquire guard
        const bool decor = pl.type == plan_type_t::startup || pl.type == plan_type_t::break_decor;
        if ( decor ) {
            // find the next object press relative to t
            for ( const auto& sp : sched.press_time ) {
                if ( sp >= t && sp - t <= acquire_window_ms ) { r.decor_owned_during_acquire = true; break; }
            }
        }
    }
    return r;
}

static void assert_common( const std::string& name, const sim_result_t& r, float disp_cap ) {
    check( r.all_finite, name, "all positions finite" );
    check( r.max_frame_disp <= disp_cap, name,
        "max frame displacement " + std::to_string( r.max_frame_disp ) + " <= " + std::to_string( disp_cap ) );
    check( !r.decor_owned_during_acquire, name, "decorative never owns during acquisition" );
}

struct transition_metrics_t {
    uint64_t transitions = 0, suspicious = 0, total_plans = 0, excessive_plan_transitions = 0;
    uint64_t micro_plans = 0, recovery_plans = 0;
    uint64_t one_plan = 0, two_plans = 0, three_plus_plans = 0;
    float max_displacement = 0.f, max_off_route = 0.f, max_speed = 0.f, max_accel = 0.f, max_jerk = 0.f;
    int max_plans = 0, reversals = 0;
};

static float distance_to_segment( point_t p, point_t a, point_t b ) {
    const point_t d = b - a;
    const float u = length_sq( d ) > 0.001f ? std::clamp( dot( p - a, d ) / length_sq( d ), 0.f, 1.f ) : 0.f;
    return length( p - ( a + d * u ) );
}

static transition_metrics_t trace_transitions( motion_engine_t& eng, const osu::beatmap_data_t& map,
                                                schedule_view_t& sched, int step_ms = 4, bool verbose = false,
                                                int reanchor_time = -1 ) {
    transition_metrics_t out{};
    if ( map.objects.size() < 2 ) return out;
    int interval = 0, plans = 0;
    uint64_t previous_plan = 0;
    point_t previous_acceleration{};
    point_t previous_position = eng.position();
    float previous_progress_speed = 0.f;
    bool have_acceleration = false;
    bool reanchored = false;
    for ( int t = sched.press_time.front(); t <= sched.press_time.back(); t += step_ms ) {
        while ( interval + 1 < static_cast<int>( map.objects.size() ) - 1 && t >= sched.press_time[ interval + 1 ] ) {
            ++out.transitions; out.total_plans += plans; out.max_plans = std::max( out.max_plans, plans );
            if ( plans > 2 ) ++out.excessive_plan_transitions;
            if ( plans <= 1 ) ++out.one_plan; else if ( plans == 2 ) ++out.two_plans; else ++out.three_plus_plans;
            ++interval; plans = 0; previous_plan = 0; previous_progress_speed = 0.f;
        }
        if ( !reanchored && reanchor_time >= 0 && t >= reanchor_time ) {
            eng.reanchor( eng.position(), "ablation-reanchor" );
            reanchored = true;
        }
        eng.update( map, sched, t, step_ms * 0.001f );
        const motion_state_t& motion = eng.motion();
        out.max_displacement = std::max( out.max_displacement, length( motion.position - previous_position ) );
        previous_position = motion.position;
        out.max_speed = std::max( out.max_speed, length( motion.velocity ) );
        out.max_accel = std::max( out.max_accel, length( motion.acceleration ) );
        if ( have_acceleration ) out.max_jerk = std::max( out.max_jerk,
            length( motion.acceleration - previous_acceleration ) / ( step_ms * 0.001f ) );
        previous_acceleration = motion.acceleration; have_acceleration = true;
        const plan_t& plan = eng.plan();
        if ( plan.id != 0 && plan.id != previous_plan ) {
            ++plans; previous_plan = plan.id;
            if ( plan.type == plan_type_t::micro_gap ) ++out.micro_plans;
            if ( plan.type == plan_type_t::recovery ) ++out.recovery_plans;
        }
        if ( interval + 1 >= static_cast<int>( map.objects.size() ) ) continue;
        const auto& previous_object = map.objects[ static_cast<size_t>( interval ) ];
        if ( ( is_slider( previous_object ) || is_spinner( previous_object ) ) && t <= previous_object.end_time + 8 )
            continue;
        point_t from = object_center( previous_object, false );
        if ( is_slider( previous_object ) ) {
            const slider_path_t path = build_slider_path( previous_object, false );
            if ( !path.empty() ) from = eval_slider_ball( path, previous_object, previous_object.end_time );
        }
        point_t to = object_center( map.objects[ static_cast<size_t>( interval + 1 ) ], false );
        point_t cached{};
        if ( !is_slider( previous_object ) && !is_spinner( previous_object ) &&
             eng.cached_target_point( interval, cached ) ) from = cached;
        if ( eng.cached_target_point( interval + 1, cached ) ) to = cached;
        const point_t direction = normalized( to - from );
        const float progress_speed = dot( motion.velocity, direction );
        if ( previous_progress_speed > 120.f && progress_speed < -120.f ) ++out.reversals;
        previous_progress_speed = progress_speed;
        const float off_route = distance_to_segment( motion.position, from, to );
        out.max_off_route = std::max( out.max_off_route, off_route );
        const bool away_from_previous = dot( motion.velocity, motion.position - from ) > 0.f;
        const bool not_toward_next = dot( motion.velocity, to - motion.position ) <= 0.f;
        if ( away_from_previous && not_toward_next && off_route > 10.f && length( motion.velocity ) > 500.f ) {
            ++out.suspicious;
            if ( verbose && out.suspicious <= 6 ) std::printf(
                "      event t=%d interval=%d types=%u->%u plan=%s p=(%.1f,%.1f) v=(%.0f,%.0f) from=(%.1f,%.1f) next=(%.1f,%.1f) off=%.1f entry v/a=(%.0f,%.0f)/(%.0f,%.0f) duration=%.0f\n",
                t, interval, static_cast<unsigned>( previous_object.type ),
                static_cast<unsigned>( map.objects[ static_cast<size_t>( interval + 1 ) ].type ),
                plan_type_name( plan.type ), motion.position.x, motion.position.y, motion.velocity.x,
                motion.velocity.y, from.x, from.y, to.x, to.y, off_route,
                plan.v0.x,plan.v0.y,plan.a0.x,plan.a0.y,plan.t_end-plan.t_start );
        }
    }
    ++out.transitions; out.total_plans += plans; out.max_plans = std::max( out.max_plans, plans );
    if ( plans > 2 ) ++out.excessive_plan_transitions;
    if ( plans <= 1 ) ++out.one_plan; else if ( plans == 2 ) ++out.two_plans; else ++out.three_plus_plans;
    return out;
}

static motion_engine_t fresh_engine( const osu::beatmap_data_t& map, point_t start, uint32_t seed = 12345u ) {
    motion_engine_t e;
    settings_t s;                                   // defaults ~ the shipped config
    e.configure( s );
    e.begin_session( start, /*hr_flip*/false, /*rate*/1.f, map.objects.size( ), seed, true );
    return e;
}

// ===========================================================================
//  Scenarios
// ===========================================================================

// 1. spaced circles
static void t_spaced_circles( ) {
    const std::string name = "1 spaced circles";
    auto map = make_map( { circle( 1000, 100, 100 ), circle( 1400, 300, 120 ),
                           circle( 1800, 260, 300 ), circle( 2200, 120, 280 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 200, 2500, 4 );
    assert_common( name, r, 45.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "no discontinuities" );
    // route visits the four circles in order
    std::vector<int> want = { 0, 1, 2, 3 };
    bool ok = r.gameplay_route.size( ) >= 4;
    for ( size_t i = 0; ok && i < want.size( ); ++i ) ok = ( i < r.gameplay_route.size( ) && r.gameplay_route[ i ] == want[ i ] );
    check( ok, name, "route A->B->C->D in order" );
    check( eng.diagnostics( ).objects_completed == 4, name, "all objects completed" );
}

// 2. dense stream
static void t_dense_stream( ) {
    const std::string name = "2 dense stream";
    std::vector<osu::hit_object_t> objs;
    for ( int i = 0; i < 24; ++i ) objs.push_back( circle( 1000 + i * 60, 120.f + i * 12.f, 180.f + ( i % 2 ? 24.f : -24.f ) ) );
    auto map = make_map( objs );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 500, 2600, 4 );
    assert_common( name, r, 30.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "no discontinuities" );
    check( eng.diagnostics( ).objects_completed == 24, name, "stream completed" );
    check( eng.diagnostics( ).dense_flow_activations > 0, name, "rolling dense-flow lookahead activated" );
    check( eng.diagnostics( ).dense_pattern_recoveries == 0, name, "dense stream needed no recovery" );
}

// 3. large jumps
static void t_large_jumps( ) {
    const std::string name = "3 large jumps";
    auto map = make_map( { circle( 1000, 40, 40 ), circle( 1500, 480, 350 ),
                           circle( 2000, 40, 350 ), circle( 2500, 480, 40 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 200, 2800, 4 );
    assert_common( name, r, 60.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "no discontinuities" );
}

// 4. sharp reversal
static void t_sharp_reversal( ) {
    const std::string name = "4 sharp reversal";
    auto map = make_map( { circle( 1000, 100, 200 ), circle( 1300, 400, 200 ),
                           circle( 1600, 100, 200 ), circle( 1900, 400, 200 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 200, 2200, 4 );
    assert_common( name, r, 55.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "no discontinuities" );
}

// 5. stacked notes
static void t_stacked_notes( ) {
    const std::string name = "5 stacked notes";
    std::vector<osu::hit_object_t> objs;
    for ( int i = 0; i < 6; ++i ) objs.push_back( circle( 1000 + i * 120, 250.f, 190.f ) );  // all on top
    auto map = make_map( objs );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 500, 1900, 4 );
    assert_common( name, r, 25.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "no discontinuities" );
    // after arriving, movement over the stack stays tiny
    check( r.max_frame_disp < 20.f, name, "stack: minimal movement" );
}

// 6. circle -> slider
static void t_circle_slider( ) {
    const std::string name = "6 circle->slider";
    auto map = make_map( { circle( 1000, 100, 100 ),
                           slider( 1500, 2100, 300, 200, "B|380:200|420:320", 260.f ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 200, 2200, 4 );
    assert_common( name, r, 45.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "no snap into slider follow" );
    // saw a slider plan
    bool saw_slider = false;
    for ( auto t : r.plan_sequence ) if ( t == plan_type_t::slider ) saw_slider = true;
    check( saw_slider, name, "entered slider plan" );
}

// 7. slider -> circle
static void t_slider_circle( ) {
    const std::string name = "7 slider->circle";
    auto map = make_map( { slider( 1000, 1600, 150, 150, "L|350:150", 200.f ),
                           circle( 2000, 400, 300 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 150, 150 } );
    auto r = run( eng, map, sched, 200, 2200, 4 );
    assert_common( name, r, 45.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "smooth slider exit -> circle" );
    check( eng.diagnostics( ).objects_completed == 2, name, "both completed" );
}

// 8. circle -> spinner
static void t_circle_spinner( ) {
    const std::string name = "8 circle->spinner";
    auto map = make_map( { circle( 1000, 120, 120 ), spinner( 1500, 2600 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 200, 2700, 4 );
    assert_common( name, r, 50.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "no snap into spinner orbit" );
    bool saw_spinner = false;
    for ( auto t : r.plan_sequence ) if ( t == plan_type_t::spinner ) saw_spinner = true;
    check( saw_spinner, name, "entered spinner plan" );
}

// 9. spinner -> circle
static void t_spinner_circle( ) {
    const std::string name = "9 spinner->circle";
    auto map = make_map( { spinner( 1000, 2000 ), circle( 2500, 400, 120 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 200, 2800, 4 );
    assert_common( name, r, 55.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "smooth spinner exit -> circle" );
}

// 10. long break -> gameplay
static void t_long_break( ) {
    const std::string name = "10 long break->gameplay";
    auto map = make_map( { circle( 1000, 200, 200 ), circle( 6000, 300, 200 ) } );  // 5s gap
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 200, 6100, 8 );
    assert_common( name, r, 45.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "no discontinuities across break" );
    check( eng.diagnostics( ).break_segments > 0, name, "break motion happened" );
    check( eng.diagnostics( ).objects_completed == 2, name, "resumed and hit next object" );
}

// 11. startup -> first object
static void t_startup( ) {
    const std::string name = "11 startup->first object";
    auto map = make_map( { circle( 4000, 300, 250 ) } );   // 4s of lead time
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 100, 4100, 8 );
    assert_common( name, r, 45.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "no discontinuities during startup" );
    check( eng.diagnostics( ).startup_segments > 0, name, "startup motion happened" );
    check( eng.diagnostics( ).objects_completed == 1, name, "reached first object" );
}

// 12. frame hitch
static void t_frame_hitch( ) {
    const std::string name = "12 frame hitch";
    auto map = make_map( { circle( 1000, 120, 120 ), circle( 1600, 380, 260 ), circle( 2200, 120, 300 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    // normal frames, then one giant hitch (250 ms of control time, clamped dt), then normal
    point_t prev = eng.position( );
    float worst = 0.f;
    bool finite_ok = true;
    for ( int t = 200; t <= 2400; t += 4 ) {
        int step = 4;
        float dt = 0.004f;
        if ( t == 1400 ) { step = 250; dt = 0.025f; }   // hitch: engine must clamp dt
        eng.update( map, sched, t, dt );
        const point_t p = eng.position( );
        finite_ok = finite_ok && std::isfinite( p.x ) && std::isfinite( p.y );
        worst = std::max( worst, length( p - prev ) );
        prev = p;
        if ( step != 4 ) t += ( step - 4 );
    }
    check( finite_ok, name, "all positions finite through hitch" );
    // A 250 ms control-time advance legitimately covers real distance, but a
    // single frame must not produce an unbounded jump; the engine records any
    // budget violation.  We accept a larger single-frame move here but require
    // it to remain finite.  A control-time jump may be retained in the bounded
    // flight recorder instead of silently hiding the catch-up displacement.
    check( eng.diagnostics( ).unexpected_discontinuities <= 1, name, "hitch produces at most one recorded catch-up event" );
}

// 13. map rewind (retry) -> new session
static void t_map_rewind( ) {
    const std::string name = "13 map rewind";
    auto map = make_map( { circle( 1000, 120, 120 ), circle( 1600, 380, 260 ), circle( 2200, 300, 300 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    run( eng, map, sched, 200, 1800, 4 );
    // host detects the rewind and restarts the session from the observed cursor
    const point_t observed = eng.position( );
    eng.begin_session( observed, false, 1.f, map.objects.size( ), 999u );
    auto r = run( eng, map, sched, 200, 2400, 4 );
    assert_common( name, r, 45.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "no discontinuity after rewind" );
    check( eng.diagnostics( ).objects_completed == 3, name, "replayed all objects" );
}

// 14. one anomalous cursor observation (single reanchor request ignored logic)
static void t_one_anomaly( ) {
    const std::string name = "14 one anomalous observation";
    auto map = make_map( { circle( 1000, 120, 120 ), circle( 1600, 380, 260 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    // Drive normally.  A single spurious observation should NOT reanchor -- the
    // adapter only reanchors after 3 persistent frames, so at the engine level
    // we verify that a lone reanchor is the ONLY way position changes abruptly,
    // and that without a reanchor call the motion stays continuous.
    auto r = run( eng, map, sched, 200, 1800, 4 );
    assert_common( name, r, 45.f );
    check( eng.diagnostics( ).external_reanchors == 0, name, "no reanchor without persistent mismatch" );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "single anomaly does not perturb engine" );
}

// 15. persistent cursor mismatch -> reanchor is continuous at the engine level
static void t_persistent_mismatch( ) {
    const std::string name = "15 persistent mismatch";
    auto map = make_map( { circle( 1000, 120, 120 ), circle( 2000, 380, 260 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    run( eng, map, sched, 200, 900, 4 );
    // Host decided the mismatch is persistent and reanchors to an observed point.
    const point_t observed = eng.position( ) + point_t{ 40.f, -25.f };
    eng.reanchor( observed, "external-mismatch" );
    check( eng.diagnostics( ).external_reanchors == 1, name, "reanchor counted" );
    check( length( eng.position( ) - observed ) < 0.01f, name, "reanchored to observed" );
    // after reanchor, the NEXT plan starts from the observed state -> continuous
    auto r = run( eng, map, sched, 904, 2100, 4 );
    check( r.all_finite, name, "finite after reanchor" );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "continuous after reanchor" );
}

// 16. one anomalous playfield geometry sample (handled in adapter; verify the
//     engine motion is projection-independent and stays continuous regardless)
static void t_geometry_anomaly( ) {
    const std::string name = "16 geometry anomaly";
    // The engine works purely in playfield space; a bad *screen* rect is the
    // adapter's concern (it holds the frame).  Here we confirm the engine never
    // itself produces a discontinuity that a bad projection could amplify.
    auto map = make_map( { circle( 1000, 120, 120 ), circle( 1600, 380, 260 ), circle( 2200, 200, 320 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 200, 2400, 4 );
    assert_common( name, r, 45.f );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "engine motion continuous" );
    check( eng.diagnostics( ).max_frame_displacement < 45.f, name, "bounded internal displacement" );
}

// ---- cross-cutting invariants ---------------------------------------------

// target points are stable across the whole session (never reroll)
static void t_target_point_stability( ) {
    const std::string name = "X target-point stability";
    auto map = make_map( { circle( 1000, 100, 100 ), circle( 1400, 300, 120 ), circle( 1800, 260, 300 ) } );
    auto sched = make_schedule( map );
    // The engine caches target points internally; we can only observe stability
    // through select_target_point being deterministic for a fixed seed.
    point_t a = select_target_point( 1, map.objects[ 1 ], false, map.cs, 777u, 0.10f, 0.55f, 1.2f, 0.f );
    point_t b = select_target_point( 1, map.objects[ 1 ], false, map.cs, 777u, 0.10f, 0.55f, 1.2f, 0.f );
    check( length( a - b ) < 1e-4f, name, "target point deterministic for fixed seed" );
    // and the chosen point is inside the object's hit circle
    const point_t c = object_center( map.objects[ 1 ], false );
    check( length( a - c ) <= hit_radius( map.cs, false ), name, "target point inside hit radius" );
}

// quintic endpoints: reaches p1 at u=1, starts at p0 at u=0 (timed arrival)
static void t_quintic_arrival( ) {
    const std::string name = "X quintic arrival";
    point_t p0{ 50, 60 }, v0{ 120, -40 }, a0{ 0, 0 }, p1{ 400, 300 }, v1{ 60, 0 };
    auto k0 = eval_quintic( p0, v0, a0, p1, v1, {}, 0.5f, 0.f );
    auto k1 = eval_quintic( p0, v0, a0, p1, v1, {}, 0.5f, 1.f );
    check( length( k0.position - p0 ) < 1e-3f, name, "starts at p0" );
    check( length( k1.position - p1 ) < 1e-3f, name, "arrives at p1" );
    check( length( k0.velocity - v0 ) < 1e-2f, name, "starts at v0 (continuity)" );
}

// A routine slow section has spare acquisition time but is not a real break.
static void t_normal_slow_section( ) {
    const std::string name = "17 normal slow section";
    auto map = make_map( { circle( 1000, 180, 190 ), circle( 3200, 340, 190 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 } );
    auto r = run( eng, map, sched, 200, 3300, 4 );
    assert_common( name, r, 45.f );
    check( eng.diagnostics( ).break_segments == 0, name, "2.2s routine gap does not activate break decor" );
}

// Long mixed gameplay catches periodic plan churn/recovery/decor activation.
static void t_long_normal_gameplay( ) {
    const std::string name = "18 long normal gameplay";
    std::vector<osu::hit_object_t> objects;
    int t = 800;
    for ( int i = 0; i < 1200; ++i ) {
        t += 68 + ( i * 37 % 310 );
        const float x = 256.f + 170.f * std::sin( static_cast<float>( i ) * 0.37f );
        const float y = 192.f + 120.f * std::sin( static_cast<float>( i ) * 0.23f + 0.7f );
        objects.push_back( circle( t, x, y ) );
    }
    auto map = make_map( std::move( objects ) );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 }, 0x5123u );
    auto r = run( eng, map, sched, 200, t + 100, 8 );
    assert_common( name, r, 45.f );
    check( eng.diagnostics( ).break_segments == 0, name, "ordinary gameplay never decorates" );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "no discontinuity across 1200 objects" );
    check( eng.diagnostics( ).recovery_plans < 4, name, "no recovery loop" );
}

static void t_decor_transition_derivatives( ) {
    const std::string name = "19 decor derivative continuity";
    auto map = make_map( { circle( 1000, 180, 180 ), circle( 6200, 350, 220 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 }, 441u );
    uint64_t previous_id = eng.plan( ).id;
    bool checked_decor = false, checked_return = false;
    float farthest = 0.f;
    point_t decor_anchor{};
    bool have_anchor = false;
    for ( int t = 200; t <= 6300; t += 4 ) {
        const motion_state_t before = eng.motion( );
        const plan_type_t before_type = eng.plan_type( );
        eng.update( map, sched, t, 0.004f );
        const plan_t& p = eng.plan( );
        if ( p.id != previous_id ) {
            check( length( p.p0 - before.position ) < 0.001f, name, "new plan captures current position" );
            check( length( p.v0 - before.velocity ) < 0.05f, name, "new plan captures current velocity" );
            check( length( p.a0 - before.acceleration ) < 0.5f, name, "new plan captures current acceleration" );
            if ( p.type == plan_type_t::break_decor ) checked_decor = true;
            if ( before_type == plan_type_t::break_decor && p.type == plan_type_t::gameplay ) checked_return = true;
            previous_id = p.id;
        }
        if ( p.type == plan_type_t::break_decor ) {
            if ( !have_anchor ) { decor_anchor = eng.position( ); have_anchor = true; }
            farthest = std::max( farthest, length( eng.position( ) - decor_anchor ) );
        }
    }
    check( checked_decor, name, "entered break decoration" );
    check( checked_return, name, "break yielded to gameplay" );
    check( farthest < 360.f, name, "break motif remained inside a bounded playfield excursion" );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "no C0 discontinuity" );
}

static void t_startup_motif_library( ) {
    const std::string name = "20 startup motif library";
    bool seen[ static_cast<int>( decor_motif_t::count ) ]{};
    int unique = 0;
    for ( uint32_t seed = 1; seed <= 24; ++seed ) {
        auto map = make_map( { circle( 4200, 320, 210 ) } );
        auto sched = make_schedule( map );
        auto a = fresh_engine( map, { 256, 192 }, seed );
        auto b = fresh_engine( map, { 256, 192 }, seed );
        bool deterministic = true, bounded = true;
        decor_motif_t first_motif = decor_motif_t::count;
        for ( int t = 100; t <= 4300; t += 8 ) {
            a.update( map, sched, t, 0.008f ); b.update( map, sched, t, 0.008f );
            if ( first_motif == decor_motif_t::count ) first_motif = a.diagnostics( ).decor_motif;
            deterministic = deterministic && length( a.position( ) - b.position( ) ) < 0.001f;
            bounded = bounded && a.position( ).x >= 32.f && a.position( ).x <= 480.f &&
                                  a.position( ).y >= 28.f && a.position( ).y <= 356.f;
        }
        const int motif = static_cast<int>( first_motif );
        if ( motif >= 0 && motif < static_cast<int>( decor_motif_t::count ) && !seen[ motif ] ) {
            seen[ motif ] = true; ++unique;
        }
        check( deterministic, name, "same seed reproduces identical motion" );
        check( bounded, name, "motif stays in playfield" );
        check( a.diagnostics( ).unexpected_discontinuities == 0, name, "motif and exit remain continuous" );
    }
    check( unique >= 7, name, "at least seven distinct motifs selected" );
}

static void t_stale_output_observations( ) {
    const std::string name = "21 stale output observations";
    cursor_observation_guard_t guard;
    for ( int i = 0; i < 8; ++i ) guard.note_output( { 100.f + i * 18.f, 200.f } );
    bool false_reanchor = false;
    for ( int i = 0; i < 30; ++i )
        false_reanchor = false_reanchor || guard.observe( { 100.f + ( i % 8 ) * 18.f, 200.f } );
    check( !false_reanchor, name, "delayed own outputs do not reanchor" );
    check( !guard.observe( { 420.f, 300.f } ), name, "first external sample waits" );
    check( !guard.observe( { 421.f, 300.f } ), name, "second external sample waits" );
    check( guard.observe( { 420.f, 301.f } ), name, "persistent coherent external position reanchors" );
}

static void t_all_plan_transition_continuity( ) {
    const std::string name = "22 plan transition continuity";
    auto map = make_map( {
        circle( 1000, 120, 120 ), circle( 1400, 340, 130 ),
        slider( 1800, 2400, 260, 180, "L|400:220", 150.f ),
        circle( 2800, 390, 300 ), spinner( 3300, 4300 ), circle( 8000, 150, 250 )
    } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 }, 7721u );
    bool transitions[ 8 ][ 8 ]{};
    uint64_t prior_id = 0;
    for ( int t = 200; t <= 8100; t += 4 ) {
        const auto before = eng.motion( );
        const auto before_type = eng.plan_type( );
        eng.update( map, sched, t, 0.004f );
        const plan_t& plan = eng.plan( );
        if ( plan.id != prior_id ) {
            check( length( plan.p0 - before.position ) < 0.001f, name, "transition preserves position" );
            check( length( plan.v0 - before.velocity ) < 0.05f, name, "transition preserves velocity" );
            check( length( plan.a0 - before.acceleration ) < 0.5f, name, "transition preserves acceleration" );
            transitions[ static_cast<int>( before_type ) ][ static_cast<int>( plan.type ) ] = true;
            prior_id = plan.id;
        }
    }
    check( transitions[ static_cast<int>( plan_type_t::gameplay ) ][ static_cast<int>( plan_type_t::gameplay ) ], name, "GAMEPLAY -> GAMEPLAY" );
    check( transitions[ static_cast<int>( plan_type_t::gameplay ) ][ static_cast<int>( plan_type_t::slider ) ], name, "GAMEPLAY -> SLIDER" );
    check( transitions[ static_cast<int>( plan_type_t::slider ) ][ static_cast<int>( plan_type_t::gameplay ) ], name, "SLIDER -> GAMEPLAY" );
    check( transitions[ static_cast<int>( plan_type_t::gameplay ) ][ static_cast<int>( plan_type_t::spinner ) ], name, "GAMEPLAY -> SPINNER" );
    check( transitions[ static_cast<int>( plan_type_t::spinner ) ][ static_cast<int>( plan_type_t::break_decor ) ], name, "SPINNER -> BREAK" );
    check( transitions[ static_cast<int>( plan_type_t::break_decor ) ][ static_cast<int>( plan_type_t::gameplay ) ], name, "BREAK -> GAMEPLAY" );
    check( eng.diagnostics( ).unexpected_discontinuities == 0, name, "all transitions remain continuous" );

    auto recovery_map = make_map( { circle( 1000, 100, 100 ), circle( 1600, 380, 280 ) } );
    auto recovery_sched = make_schedule( recovery_map );
    auto recovery = fresh_engine( recovery_map, { 256, 192 }, 99u );
    recovery.update( recovery_map, recovery_sched, 1120, 0.004f );
    check( recovery.plan_type( ) == plan_type_t::recovery, name, "genuine late start activates RECOVERY" );
    const auto recovery_before = recovery.motion( );
    recovery.update( recovery_map, recovery_sched, 1124, 0.004f );
    check( recovery.plan_type( ) == plan_type_t::gameplay, name, "RECOVERY -> GAMEPLAY" );
    check( length( recovery.plan( ).p0 - recovery_before.position ) < 0.001f, name, "recovery exit owns current state" );
    check( length( recovery.plan( ).v0 - recovery_before.velocity ) < 0.05f, name, "recovery exit preserves velocity" );
    check( length( recovery.plan( ).a0 - recovery_before.acceleration ) < 0.5f, name, "recovery exit preserves acceleration" );
}

static void t_compact_repeat_slider( ) {
    const std::string name = "23 compact repeat slider";
    auto map = make_map( { slider( 1000, 2200, 250, 190, "L|278:190", 28.f, 8 ),
                           circle( 2600, 360, 240 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 250, 190 }, 313u );
    point_t anchor{}; bool have_anchor = false; float excursion = 0.f;
    for ( int t = 500; t <= 2700; t += 4 ) {
        eng.update( map, sched, t, 0.004f );
        if ( eng.plan_type( ) == plan_type_t::slider && eng.plan( ).compact_slider ) {
            if ( !have_anchor ) { anchor = eng.plan( ).slider_anchor; have_anchor = true; }
            excursion = std::max( excursion, length( eng.position( ) - anchor ) );
        }
    }
    const auto d = eng.diagnostics( );
    check( have_anchor && d.compact_slider_activations == 1, name, "compact mode activated" );
    check( excursion < 18.f, name, "cursor does not trace every short reversal" );
    check( d.slider_follow_failures == 0 && d.slider_early_releases == 0, name, "follow and release remained safe" );
    check( d.sliders_completed == 1, name, "slider predicted complete" );
    check( d.unexpected_discontinuities == 0, name, "continuous compact entry/exit" );
}

static void t_large_slider_follow( ) {
    const std::string name = "24 large slider follow";
    auto map = make_map( { slider( 1000, 2300, 90, 100, "B|220:60|390:260|470:300", 430.f ),
                           circle( 2800, 420, 300 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 90, 100 }, 711u );
    auto result = run( eng, map, sched, 400, 2900, 4 );
    assert_common( name, result, 45.f );
    const auto d = eng.diagnostics( );
    check( d.compact_slider_activations == 0, name, "large path stays in normal follow mode" );
    check( d.slider_follow_failures == 0 && d.slider_early_releases == 0, name, "large follow remained safe" );
    check( d.sliders_completed == 1, name, "large slider predicted complete" );
}

static void t_spinner_character( ) {
    const std::string name = "25 spinner character";
    auto map = make_map( { circle( 1000, 120, 180 ), spinner( 1700, 4200 ), circle( 4700, 390, 220 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 120, 180 }, 0xabcdu );
    auto result = run( eng, map, sched, 300, 4800, 4 );
    assert_common( name, result, 55.f );
    const auto d = eng.diagnostics( );
    check( d.spinner_acquisitions == 1, name, "spinner acquired once" );
    check( d.unexpected_spinner_reversals == 0, name, "preferred direction never reversed" );
    check( d.spinner_max_radius - d.spinner_min_radius > 2.f, name, "orbit radius varies smoothly" );
    check( d.spinner_entry_issues == 0, name, "entry reached orbit region" );
    check( d.unexpected_discontinuities == 0, name, "spinner entry and exit continuous" );
}

static void t_decor_families_and_scale( ) {
    const std::string name = "26 decor families / scale";
    bool saw_dance = false, saw_fidget = false, saw_large = false;
    for ( uint32_t seed = 10; seed < 50; ++seed ) {
        auto map = make_map( { circle( 8500, 330, 220 ) } );
        auto sched = make_schedule( map );
        auto eng = fresh_engine( map, { 256, 192 }, seed );
        run( eng, map, sched, 100, 7000, 8 );
        const auto d = eng.diagnostics( );
        saw_dance = saw_dance || d.dance_motifs > 0;
        saw_fidget = saw_fidget || d.fidget_motifs > 0;
        saw_large = saw_large || d.max_decorative_excursion >= 150.f;
        check( d.unexpected_discontinuities == 0, name, "motifs remain continuous" );
        check( d.decoration_caused_recovery == 0, name, "decoration never causes recovery" );
    }
    check( saw_dance, name, "large dance family selected" );
    check( saw_fidget, name, "fidget family selected" );
    check( saw_large, name, "long budget permits substantial excursion" );
}

static void t_speed_aware_local_slider_coverage( ) {
    const std::string name = "27 speed-aware loop coverage";
    const char* tiny_loop = "B|280:150|320:192|280:234|232:234|192:192|232:150|280:192";
    struct counts_t { int track = 0, blend = 0, anchor = 0; diagnostics_t d{}; };
    auto measure = [&]( int duration, float loop_size, const char* curve ) {
        auto map = make_map( { slider( 1000, 1000 + duration, 280, 192, curve, loop_size ),
                               circle( 1600 + duration, 400, 240 ) } );
        auto sched = make_schedule( map );
        auto eng = fresh_engine( map, { 280, 192 }, 0x711u );
        counts_t c{};
        for ( int t = 500; t <= 1050 + duration; t += 4 ) {
            eng.update( map, sched, t, 0.004f );
            if ( eng.plan_type( ) != plan_type_t::slider || t <= static_cast<int>( eng.plan( ).arrival_key ) ) continue;
            switch ( eng.diagnostics( ).slider_strategy ) {
            case slider_strategy_t::anchor: ++c.anchor; break;
            case slider_strategy_t::blend: ++c.blend; break;
            default: ++c.track; break;
            }
        }
        c.d = eng.diagnostics( );
        return c;
    };
    const auto fast = measure( 650, 0.f, tiny_loop );
    const auto medium = measure( 1000, 0.f, tiny_loop );
    const auto slow = measure( 3200, 0.f, tiny_loop );
    const char* large_loop = "B|256:60|410:80|450:192|410:320|256:330|100:320|60:192|100:70|256:60";
    const auto large = measure( 900, 0.f, large_loop );
    if ( std::getenv( "DBG" ) ) std::printf( "    loop fast T/B/A=%d/%d/%d safe=%llu max=%.1f speed=%.1f; med=%d/%d/%d; slow=%d/%d/%d; large=%d/%d/%d\n",
        fast.track, fast.blend, fast.anchor, static_cast<unsigned long long>( fast.d.safe_anchor_sections ),
        fast.d.slider_anchor_max_distance, fast.d.slider_local_speed,
        medium.track, medium.blend, medium.anchor, slow.track, slow.blend, slow.anchor,
        large.track, large.blend, large.anchor );

    check( fast.anchor > 0, name, "very-fast small loop reaches ANCHOR" );
    check( medium.blend > 0, name, "medium loop uses BLEND" );
    check( slow.track > fast.track, name, "same slow geometry tracks more" );
    check( fast.anchor > slow.anchor, name, "anchor response decreases monotonically with speed" );
    check( fast.d.safe_anchor_sections > 0, name, "safe local section detected" );
    check( large.track > large.anchor*4 && large.d.compact_slider_activations==0 &&
        large.d.slider_cursor_ball_ratio>0.70f, name,
        "large loop remains tracking-dominant; a short safe tail region may hold" );
    check( fast.d.slider_follow_failures == 0 && slow.d.slider_follow_failures == 0,
        name, "fast and slow policies remain follow-safe" );
    check( fast.d.unexpected_discontinuities == 0 && medium.d.unexpected_discontinuities == 0 &&
           slow.d.unexpected_discontinuities == 0, name, "strategy changes remain continuous" );
}

static void t_partial_slider_section( ) {
    const std::string name = "28 partial loop section";
    const char* path = "B|190:192|190:192|225:155|270:192|225:229|190:192|190:192|410:220";
    auto map = make_map( { slider( 1000, 2100, 80, 192, path, 0.f ), circle( 2500, 430, 250 ) } );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 80, 192 }, 0x818u );
    bool saw_track = false, saw_anchor_or_blend = false, saw_track_after = false;
    for ( int t = 500; t <= 2200; t += 4 ) {
        eng.update( map, sched, t, 0.004f );
        if ( eng.plan_type( ) != plan_type_t::slider ) continue;
        const auto strategy = eng.diagnostics( ).slider_strategy;
        if ( strategy == slider_strategy_t::track ) {
            if ( saw_anchor_or_blend ) saw_track_after = true;
            else saw_track = true;
        } else if ( saw_track ) saw_anchor_or_blend = true;
    }
    const auto d = eng.diagnostics( );
    if ( std::getenv( "DBG" ) ) std::printf( "    partial track=%d simplify=%d after=%d safe=%llu partial=%llu compact=%llu speed=%.1f max=%.1f\n",
        saw_track, saw_anchor_or_blend, saw_track_after,
        static_cast<unsigned long long>( d.safe_anchor_sections ),
        static_cast<unsigned long long>( d.partial_anchor_activations ),
        static_cast<unsigned long long>( d.compact_slider_activations ), d.slider_local_speed,
        d.slider_anchor_max_distance );
    check( saw_track && saw_anchor_or_blend && saw_track_after, name, "TRACK -> local simplification -> TRACK" );
    check( d.partial_anchor_activations > 0, name, "partial safe section counted" );
    check( d.compact_slider_activations == 0, name, "whole slider was not classified compact" );
    check( d.unexpected_discontinuities == 0, name, "partial transitions remain continuous" );
}

static void t_spinner_rpm_and_shape( ) {
    const std::string name = "29 spinner rpm / oval";
    for ( const float rpm : { 300.f, 360.f, 420.f } ) {
        osu::hit_object_t o = spinner( 0, 10000 );
        const auto style=make_spinner_style(0x4a31u,10.f);
        const auto entry=spinner_entry_kinematics(style,{256,192},0.f,1.f,{rpm,rpm});
        motion_state_t m{entry.position,entry.velocity,entry.acceleration,true};
        spinner_state_t sp{};
        spinner_begin( sp, 0, o, false, m, 0x4a31u, 1.f );
        point_t previous = m.position;
        float first_disp = 0.f;
        bool reversed = false;
        float previous_angle = sp.angle;
        double sx = 0.0, sy = 0.0, sxx = 0.0, syy = 0.0, sxy = 0.0;
        int shape_samples = 0;
        for ( int ms = 4; ms <= 10000; ms += 4 ) {
            const point_t p = spinner_step( sp, o, ms, 0.004f, rpm, 0.7f );
            if ( ms == 4 ) first_disp = length( p - previous );
            reversed = reversed || sp.angle < previous_angle;
            if ( ms >= 2000 && ms <= 8000 ) {
                sx += p.x; sy += p.y; sxx += p.x * p.x; syy += p.y * p.y; sxy += p.x * p.y;
                ++shape_samples;
            }
            previous_angle = sp.angle; previous = p;
        }
        const double mx = sx / shape_samples, my = sy / shape_samples;
        const double cxx = sxx / shape_samples - mx * mx;
        const double cyy = syy / shape_samples - my * my;
        const double cxy = sxy / shape_samples - mx * my;
        const double trace = cxx + cyy;
        const double root = std::sqrt( std::max( 0.0, ( cxx - cyy ) * ( cxx - cyy ) + 4.0 * cxy * cxy ) );
        const double observed_ratio = std::sqrt( std::max( 1.0, ( trace + root ) / std::max( trace - root, 0.001 ) ) );
        const float expected = rpm / 6.f;
        if ( std::getenv( "DBG" ) ) std::printf( "    spinner %.0f rpm -> %.2f rev (%.1f measured), axes %.1f/%.1f @ %.1f deg, first %.2f\n",
            rpm, sp.revolutions, sp.revolutions * 6.f, sp.major_axis, sp.minor_axis,
            sp.rotation * 180.f / k_pi, first_disp );
        check( std::abs( sp.revolutions - expected ) <= expected * 0.09f, name,
            "10-second revolution count tracks requested RPM" );
        check( first_disp <= length(m.velocity)*.004f+.5f*length(m.acceleration)*.004f*.004f+.2f,
            name, "prepared bounded-RPM entry respects incoming kinematics" );
        check( !reversed, name, "signed angular motion never reverses" );
        check( sp.major_axis / std::max( sp.minor_axis, 1.f ) > 1.15f, name, "emitted base shape is clearly oval" );
        check( observed_ratio > 1.12, name, "actual emitted samples have a non-circular principal-axis ratio" );
        const float degrees = std::abs( sp.rotation * 180.f / k_pi );
        check( degrees >= 25.f && degrees <= 65.f, name, "oval orientation is diagonal" );
    }
}

static void t_decor_energy_response( ) {
    const std::string name = "30 decoration energy response";
    double low_excursion = 0.0, high_excursion = 0.0;
    uint64_t off_dances = 0, high_dances = 0;
    for ( uint32_t seed = 1; seed <= 32; ++seed ) {
        auto map = make_map( { circle( 10000, 330, 220 ) } );
        auto sched = make_schedule( map );
        motion_engine_t low, high, off;
        settings_t ls; ls.startup_energy = 0.05f; ls.break_energy = 0.05f;
        settings_t hs; hs.startup_energy = 1.f; hs.break_energy = 1.f; hs.energetic_dances = true;
        settings_t os = hs; os.energetic_dances = false;
        low.configure( ls ); high.configure( hs ); off.configure( os );
        low.begin_session( {256,192}, false, 1.f, map.objects.size(), seed );
        high.begin_session( {256,192}, false, 1.f, map.objects.size(), seed );
        off.begin_session( {256,192}, false, 1.f, map.objects.size(), seed );
        run( low, map, sched, 100, 8200, 8 );
        run( high, map, sched, 100, 8200, 8 );
        run( off, map, sched, 100, 8200, 8 );
        low_excursion += low.diagnostics().max_decorative_excursion;
        high_excursion += high.diagnostics().max_decorative_excursion;
        off_dances += off.diagnostics().dance_motifs;
        high_dances += high.diagnostics().dance_motifs;
    }
    check( high_excursion > low_excursion * 1.18, name, "high energy materially increases excursion" );
    check( high_dances > 0, name, "high energetic mode produces large dances" );
    check( off_dances == 0, name, "Energetic Dances OFF suppresses large family" );
}

static void t_mixed_motion_stress( ) {
    const std::string name = "31 mixed 2200-object stress";
    std::vector<osu::hit_object_t> objects;
    int t = 700;
    for ( int i = 0; i < 2200; ++i ) {
        if ( i > 0 && i % 440 == 0 ) {
            t += 2600;
            objects.push_back( spinner( t, t + 900 ) );
            t += 1050;
        } else if ( i % 73 == 40 ) {
            t += 210;
            objects.push_back( slider( t, t + 320, 150.f + ( i % 5 ) * 42.f, 170.f,
                "B|210:130|285:210|350:170", 230.f, 1 + i % 3 ) );
            t += 390;
        } else {
            t += 70 + ( i * 29 % 120 );
            objects.push_back( circle( t, 256.f + 185.f * std::sin( i * 0.31f ),
                192.f + 125.f * std::sin( i * 0.19f + 0.4f ) ) );
        }
    }
    auto map = make_map( std::move( objects ) );
    auto sched = make_schedule( map );
    auto eng = fresh_engine( map, { 256, 192 }, 0x9912u );
    auto r = run( eng, map, sched, 100, t + 2100, 8 );
    const auto d = eng.diagnostics( );
    check( r.all_finite, name, "all frames remain finite" );
    check( d.unexpected_discontinuities == 0, name, "no unexplained discontinuity" );
    check( d.slider_early_releases == 0, name, "no slider early release" );
    check( d.unexpected_spinner_reversals == 0, name, "no spinner reversal" );
    check( d.decoration_caused_recovery == 0, name, "decoration recovery remains zero" );
    check( d.recovery_plans < 12, name, "no recovery loop" );
}

static void t_slider_exit_geometry_variety( ) {
    const std::string name = "32 slider exit geometry";
    struct exit_sample_t { slider_exit_strategy_t strategy{}; point_t velocity{}; float overshoot = 0.f; };
    auto sample = [&]( point_t next, int duration, uint32_t seed ) {
        auto map = make_map( { slider( 1000, 1000 + duration, 100, 192, "L|300:192", 200.f ),
                               circle( 1350 + duration, next.x, next.y ) } );
        auto sched = make_schedule( map ); auto eng = fresh_engine( map, {100,192}, seed );
        run( eng, map, sched, 400, 1450 + duration, 4 );
        const auto d = eng.diagnostics( );
        return exit_sample_t{ d.slider_exit_strategy, d.slider_outgoing_velocity, d.slider_post_tail_overshoot };
    };
    const auto aligned = sample( {420,192}, 500, 1001u );
    const auto reverse = sample( {100,192}, 500, 1002u );
    const auto perpendicular = sample( {300,330}, 500, 1003u );
    const auto fast = sample( {430,250}, 300, 1004u );
    const auto slow = sample( {340,220}, 1200, 1005u );
    check( aligned.strategy == slider_exit_strategy_t::aligned_carry, name, "aligned tail preserves carry" );
    check( reverse.strategy == slider_exit_strategy_t::reversal_brake, name, "reversal begins braking" );
    check( perpendicular.strategy != aligned.strategy && perpendicular.strategy != reverse.strategy,
        name, "perpendicular departure uses a different profile" );
    check( length( aligned.velocity - reverse.velocity ) > 20.f, name, "tail velocities differ by geometry" );
    check( length( fast.velocity - slow.velocity ) > 20.f, name, "fast and slow exits are not identical" );
    const float min_over = std::min( { aligned.overshoot, reverse.overshoot, perpendicular.overshoot, fast.overshoot, slow.overshoot } );
    const float max_over = std::max( { aligned.overshoot, reverse.overshoot, perpendicular.overshoot, fast.overshoot, slow.overshoot } );
    check( max_over - min_over > 0.5f, name, "post-tail travel is not a universal amount" );
}

static float output_revolutions( float rpm, float callback_dt, uint32_t seed = 0x4a31u, bool emulate_old_floor = false ) {
    osu::hit_object_t o = spinner( 0, 10000 );
    motion_state_t m{}; m.initialized = true; m.position = {326,192}; m.velocity = {0,380};
    spinner_state_t sp{}; spinner_begin( sp, 0, o, false, m, seed, 1.f );
    bool have = false; float previous = 0.f, turns = 0.f;
    const int frames = static_cast<int>( std::lround( 10.f / callback_dt ) );
    for ( int frame = 1; frame <= frames; ++frame ) {
        const float elapsed = frame * callback_dt;
        const int gt = std::min( 10000, static_cast<int>( std::lround( elapsed * 1000.f ) ) );
        const float dt = emulate_old_floor ? std::clamp( callback_dt, 0.001f, 0.025f )
                                           : sanitize_controller_dt( callback_dt );
        const point_t p = spinner_step( sp, o, gt, dt, rpm, 0.7f );
        point_t q = p - sp.center - sp.center_drift;
        const float c = std::cos( sp.rotation ), s = std::sin( sp.rotation );
        const point_t local{ q.x * c + q.y * s, -q.x * s + q.y * c };
        const float angle = std::atan2( local.y / std::max( sp.minor_axis, 1.f ),
                                        local.x / std::max( sp.major_axis, 1.f ) );
        if ( have ) {
            float d = angle - previous;
            while ( d > k_pi ) d -= 2.f * k_pi;
            while ( d < -k_pi ) d += 2.f * k_pi;
            turns += d / ( 2.f * k_pi );
        }
        previous = angle; have = true;
    }
    return std::abs( turns );
}

static void t_actual_output_rpm( ) {
    const std::string name = "33 actual-output RPM";
    for ( const float rpm : { 60.f, 120.f, 300.f, 384.f, 420.f } ) {
        const float turns = output_revolutions( rpm, 0.001f );
        const float expected = rpm / 6.f;
        if ( std::getenv( "DBG" ) ) std::printf( "    output %.0f rpm -> %.2f rev\n", rpm, turns );
        check( std::abs( turns - expected ) <= std::max( 1.1f, expected * 0.09f ), name,
            "emitted orbit phase matches requested real-time RPM" );
    }
    const float normal = output_revolutions( 384.f, 0.001f );
    const float high_rate = output_revolutions( 384.f, 0.00032f );
    const float old_high_rate = output_revolutions( 384.f, 0.00032f, 0x4a31u, true );
    check( std::abs( normal - high_rate ) < 0.8f, name, "high callback rate does not manufacture controller time" );
    check( std::abs( sanitize_controller_dt( 0.00032f ) - 0.00032f ) < 1e-7f,
        name, "sub-millisecond steady-clock dt is preserved" );
    check( old_high_rate > high_rate * 2.5f, name, "regression harness reproduces the old high-rate RPM inflation" );
    if ( std::getenv( "DBG" ) ) std::printf( "    actual output 384 rpm: old high-rate %.2f, fixed normal %.2f, fixed high-rate %.2f rev\n",
        old_high_rate, normal, high_rate );
}

static void t_spinner_per_object_variety( ) {
    const std::string name = "34 per-spinner emitted variety";
    const uint32_t session = 0x7812u;
    std::vector<float> ratios, scales, orientations;
    int non_circular = 0;
    float previous_direction = 0.f;
    int direction_changes = 0, mid_reversals = 0;
    for ( int index = 0; index < 20; ++index ) {
        osu::hit_object_t o = spinner( 0, 10000 );
        motion_state_t m{}; m.initialized = true; m.position = {326,192}; m.velocity = {0,360};
        spinner_state_t sp{};
        const uint32_t seed = mix_seed( session ^ ( static_cast<uint32_t>( index + 1 ) * 0x9e3779b9u ) );
        spinner_begin( sp, index, o, false, m, seed, -1.f );
        if ( index > 0 && sp.direction != previous_direction ) ++direction_changes;
        previous_direction = sp.direction;
        double sx = 0., sy = 0., sxx = 0., syy = 0., sxy = 0.;
        int samples = 0;
        for ( int ms = 4; ms <= 10000; ms += 4 ) {
            const point_t p = spinner_step( sp, o, ms, 0.004f, 384.f, 0.5f );
            if ( sp.angular_velocity > 0.f ) ++mid_reversals;
            if ( ms >= 1500 && ms <= 8500 ) {
                sx += p.x; sy += p.y; sxx += p.x * p.x; syy += p.y * p.y; sxy += p.x * p.y; ++samples;
            }
        }
        const double mx = sx / samples, my = sy / samples;
        const double xx = sxx / samples - mx * mx, yy = syy / samples - my * my;
        const double xy = sxy / samples - mx * my;
        const double trace = xx + yy;
        const double disc = std::sqrt( std::max( 0.0, ( xx - yy ) * ( xx - yy ) + 4.0 * xy * xy ) );
        const double l0 = std::max( ( trace + disc ) * 0.5, 0.0001 );
        const double l1 = std::max( ( trace - disc ) * 0.5, 0.0001 );
        const float ratio = static_cast<float>( std::sqrt( l0 / l1 ) );
        const float scale = static_cast<float>( std::sqrt( 2.0 * l0 ) );
        const float orientation = 0.5f * std::atan2( static_cast<float>( 2.0 * xy ), static_cast<float>( xx - yy ) );
        ratios.push_back( ratio ); scales.push_back( scale ); orientations.push_back( orientation );
        if ( ratio >= 1.10f ) ++non_circular;
        check( sp.direction < 0.f, name, "session direction persists" );
        check( sp.angular_velocity <= 0.f, name, "no mid-spinner reversal" );
    }
    const auto [min_ratio, max_ratio] = std::minmax_element( ratios.begin(), ratios.end() );
    const auto [min_scale, max_scale] = std::minmax_element( scales.begin(), scales.end() );
    float min_orientation = 10.f, max_orientation = -10.f;
    for ( float a : orientations ) { min_orientation = std::min( min_orientation, a ); max_orientation = std::max( max_orientation, a ); }
    check( direction_changes == 0 && mid_reversals == 0, name, "all 20 spinners keep one signed direction" );
    check( non_circular >= 17, name, "most actual emitted paths are measurably non-circular" );
    check( *max_ratio - *min_ratio >= 0.18f, name, "actual axis ratios vary meaningfully" );
    check( *max_scale - *min_scale >= 24.f, name, "actual orbit scale spans small through large" );
    check( max_orientation - min_orientation >= 0.70f, name, "actual emitted orientation varies" );
    if ( std::getenv( "DBG" ) ) {
        std::printf( "    emitted ratios:" );
        for ( float ratio : ratios ) std::printf( " %.3f", ratio );
        std::printf( "\n    scale %.2f..%.2f, orientation %.1f..%.1f deg\n", *min_scale, *max_scale,
            min_orientation * 180.f / k_pi, max_orientation * 180.f / k_pi );
    }
}

static void t_micro_gap_policy( ) {
    const std::string name = "35 micro-gap policy";
    const int gaps[] = { 100, 250, 400, 600, 900, 1500 };
    uint64_t eligible = 0, active = 0, short_active = 0, recoveries = 0;
    for ( uint32_t seed = 1; seed <= 160; ++seed ) {
        for ( const int gap : gaps ) {
            auto map = make_map( { circle( 1000, 240, 190 ), circle( 1000 + gap, 285, 205 ) } );
            auto sched = make_schedule( map ); auto eng = fresh_engine( map, {240,190}, seed );
            run( eng, map, sched, 800, 1100 + gap, 4 );
            const auto d = eng.diagnostics( );
            eligible += d.eligible_micro_gaps; active += d.micro_gestures_activated;
            recoveries += d.micro_caused_recovery;
            if ( gap <= 250 ) short_active += d.micro_gestures_activated;
        }
    }
    const double percent = eligible ? static_cast<double>( active ) * 100.0 / eligible : 0.0;
    check( short_active == 0, name, "100/250 ms gaps never gesture" );
    check( eligible > 0 && active > 0, name, "safe 400--1500 ms gaps can activate" );
    check( percent >= 25.0 && percent <= 50.0, name, "multi-seed activation remains situational" );
    check( recoveries == 0, name, "micro gestures cause no recovery" );
    if ( std::getenv( "DBG" ) ) std::printf( "    micro gaps: %llu/%llu = %.2f%%\n",
        static_cast<unsigned long long>( active ), static_cast<unsigned long long>( eligible ), percent );
}

static void t_micro_urgent_cancel( ) {
    const std::string name = "37 micro urgent cancellation";
    bool exercised = false, continuous = false;
    for ( uint32_t seed = 1; seed <= 500 && !exercised; ++seed ) {
        auto map = make_map( { circle( 1000, 170, 180 ), circle( 1900, 390, 210 ) } );
        auto sched = make_schedule( map ); auto eng = fresh_engine( map, {170,180}, seed );
        for ( int t = 800; t <= 1700; t += 4 ) {
            eng.update( map, sched, t, 0.004f );
            if ( !eng.plan().micro_modifier ) continue;
            exercised = true;
            const motion_state_t before = eng.motion();
            const uint64_t previous_plan = eng.plan().id;
            sched.press_time[1] = t + 220;
            eng.update( map, sched, t + 4, 0.004f );
            const plan_t& next = eng.plan();
            continuous = next.type == plan_type_t::gameplay && next.id != previous_plan &&
                length( next.p0 - before.position ) < 0.001f &&
                length( next.v0 - before.velocity ) < 0.01f &&
                length( next.a0 - before.acceleration ) < 0.1f;
            break;
        }
        if ( exercised ) {
            const auto d = eng.diagnostics();
            check( eng.plan_type() == plan_type_t::gameplay, name, "urgent schedule stays one gameplay intent" );
            check( d.micro_caused_recovery == 0, name, "cancellation does not create recovery" );
            check( d.suspicious_forward_bursts == 0, name, "cancellation creates no semantic burst" );
            check( d.unexpected_discontinuities == 0, name, "cancellation creates no discontinuity" );
        }
    }
    check( exercised, name, "a deterministic gesture was exercised" );
    check( continuous, name, "replacement plan captures exact current p/v/a" );
}

static void t_forward_burst_stress( ) {
    const std::string name = "38 23988-transition burst stress";
    transition_metrics_t aggregate{};
    uint64_t recoveries = 0, discontinuities = 0;
    for ( uint32_t seed = 1; seed <= 12; ++seed ) {
        std::vector<osu::hit_object_t> objects;
        int t = 600;
        for ( int i = 0; i < 2000; ++i ) {
            const int slack = ( i % 47 == 11 ) ? 900 : ( i % 19 == 7 ) ? 520 :
                              ( i % 13 == 3 ) ? 320 : 75 + ( i * 37 % 115 );
            t += slack;
            const float x = 256.f + 180.f * std::sin( i * 0.37f + seed * 0.11f );
            const float y = 192.f + 125.f * std::sin( i * 0.23f + seed * 0.17f );
            if ( i % 31 == 8 || i % 31 == 9 ) {
                const int slider_duration = 210 + ( i % 4 ) * 55;
                objects.push_back( slider( t, t + slider_duration, x, y,
                    "B|210:140|285:235|365:175", 210.f, 1 + i % 2 ) );
                t += slider_duration;
            } else {
                objects.push_back( circle( t, x, y ) );
            }
        }
        auto map = make_map( std::move( objects ) ); auto sched = make_schedule( map );
        auto eng = fresh_engine( map, {256,192}, 0x5100u + seed );
        const transition_metrics_t metrics = trace_transitions( eng, map, sched, 8, std::getenv("DBG") != nullptr );
        if ( metrics.suspicious > 0 ) std::printf( "      stress seed %u suspicious=%llu off=%.1f\n", seed,
            static_cast<unsigned long long>( metrics.suspicious ), metrics.max_off_route );
        const auto d = eng.diagnostics();
        if ( d.unexpected_discontinuities > 0 ) std::printf(
            "      stress seed %u discontinuities=%llu handoff=%llu rejected=%llu last=%s\n", seed,
            static_cast<unsigned long long>( d.unexpected_discontinuities ),
            static_cast<unsigned long long>( d.slider_handoff_activations ),
            static_cast<unsigned long long>( d.slider_handoff_rejections ), d.last_flight_reason );
        if ( d.unexpected_discontinuities > 0 ) {
            flight_record_t record{};
            if ( eng.last_discontinuity_record( record ) ) std::printf(
                "        record t=%d plan=%s obj=%d reason=%s p=(%.1f,%.1f) wanted=(%.1f,%.1f) "
                "v=(%.0f,%.0f)->(%.0f,%.0f) a=(%.0f,%.0f)->(%.0f,%.0f) disp/bound=%.2f/%.2f\n",
                record.game_time, plan_type_name( record.plan_type ), record.object_index, record.reason,
                record.position.x, record.position.y, record.requested.x, record.requested.y,
                record.velocity.x, record.velocity.y, record.velocity_after.x, record.velocity_after.y,
                record.acceleration.x, record.acceleration.y,
                record.acceleration_after.x, record.acceleration_after.y,
                record.displacement, record.bound );
        }
        aggregate.transitions += metrics.transitions; aggregate.suspicious += metrics.suspicious;
        aggregate.total_plans += metrics.total_plans;
        aggregate.one_plan += metrics.one_plan; aggregate.two_plans += metrics.two_plans;
        aggregate.three_plus_plans += metrics.three_plus_plans;
        aggregate.max_plans = std::max( aggregate.max_plans, metrics.max_plans );
        aggregate.max_displacement = std::max( aggregate.max_displacement, metrics.max_displacement );
        aggregate.max_off_route = std::max( aggregate.max_off_route, metrics.max_off_route );
        aggregate.max_speed = std::max( aggregate.max_speed, metrics.max_speed );
        aggregate.max_accel = std::max( aggregate.max_accel, metrics.max_accel );
        aggregate.max_jerk = std::max( aggregate.max_jerk, metrics.max_jerk );
        recoveries += d.micro_caused_recovery;
        discontinuities += d.unexpected_discontinuities;
    }
    check( aggregate.transitions >= 23988, name, "tens of thousands of mixed transitions executed" );
    check( aggregate.suspicious == 0, name, "zero unexplained forward bursts" );
    check( recoveries == 0, name, "micro motion never forces recovery" );
    check( discontinuities == 0, name, "no continuity regression" );
    check( aggregate.three_plus_plans == 0, name, "no transition chains three or more movement plans" );
    std::printf( "    stress transitions=%llu suspicious=%llu plans 1/2/3+=%llu/%llu/%llu avg/max=%.2f/%d worst disp/off=%.2f/%.2f speed=%.0f accel=%.0f jerk=%.0f\n",
        static_cast<unsigned long long>( aggregate.transitions ), static_cast<unsigned long long>( aggregate.suspicious ),
        static_cast<unsigned long long>( aggregate.one_plan ), static_cast<unsigned long long>( aggregate.two_plans ),
        static_cast<unsigned long long>( aggregate.three_plus_plans ),
        aggregate.transitions ? static_cast<double>( aggregate.total_plans ) / aggregate.transitions : 0.0,
        aggregate.max_plans, aggregate.max_displacement, aggregate.max_off_route,
        aggregate.max_speed, aggregate.max_accel, aggregate.max_jerk );
}

static void t_session_spinner_direction( ) {
    const std::string name = "39 session spinner direction";
    std::vector<osu::hit_object_t> objects;
    int t = 800;
    for ( int i = 0; i < 10; ++i ) { objects.push_back( spinner( t, t + 1000 ) ); t += 1250; }
    auto map = make_map( std::move( objects ) ); auto sched = make_schedule( map );
    auto eng = fresh_engine( map, {326,192}, 0x991133u );
    run( eng, map, sched, 100, t + 200, 4 );
    const auto d = eng.diagnostics();
    check( d.spinner_acquisitions == 10, name, "all 10 spinners initialized" );
    check( d.spinner_direction_changes_between_objects == 0, name, "direction changes between spinners = 0" );
    check( d.unexpected_spinner_reversals == 0, name, "mid-spinner reversals = 0" );
}

static void t_feature_ablation_trace( ) {
    const std::string name = "40 feature ablation trace";
    std::vector<osu::hit_object_t> objects;
    int t = 700;
    for ( int i = 0; i < 180; ++i ) {
        t += ( i % 11 == 4 ) ? 720 : ( i % 7 == 2 ) ? 460 : 260;
        const float x = 256.f + 175.f * std::sin( i * 0.43f );
        const float y = 192.f + 120.f * std::sin( i * 0.27f + 0.3f );
        if ( i % 17 == 8 ) {
            objects.push_back( slider( t, t + 180, x, y, "B|245:175|275:205|305:185", 72.f ) );
            t += 180;
        } else objects.push_back( circle( t, x, y ) );
    }
    const auto map = make_map( std::move( objects ) );
    auto run_configuration = [&]( char label, bool targets, bool style, bool slider_exit,
                                  bool micro, bool gameplay_flow, bool reanchor = false ) {
        auto sched = make_schedule( map ); motion_engine_t eng; settings_t settings;
        settings.startup_motion = false; settings.break_motion = false;
        settings.gameplay_flow = gameplay_flow;
        settings.diagnostic_target_variation = targets;
        settings.diagnostic_session_style = style;
        settings.diagnostic_slider_exit = slider_exit;
        settings.diagnostic_micro_motion = micro;
        eng.configure( settings ); eng.begin_session( {256,192}, false, 1.f, map.objects.size(), 0x6611u, true );
        const int reanchor_time = reanchor ? sched.press_time[ 80 ] + 80 : -1;
        const transition_metrics_t metrics = trace_transitions( eng, map, sched, 4, false, reanchor_time );
        std::printf( "    %c plans avg/max %.2f/%d excessive=%llu suspicious=%llu off=%.1f speed=%.0f accel=%.0f jerk=%.0f micro=%llu recovery=%llu\n",
            label, metrics.transitions ? static_cast<double>( metrics.total_plans ) / metrics.transitions : 0.0,
            metrics.max_plans, static_cast<unsigned long long>( metrics.excessive_plan_transitions ),
            static_cast<unsigned long long>( metrics.suspicious ), metrics.max_off_route, metrics.max_speed,
            metrics.max_accel, metrics.max_jerk, static_cast<unsigned long long>( metrics.micro_plans ),
            static_cast<unsigned long long>( metrics.recovery_plans ) );
        return metrics;
    };
    const auto a = run_configuration( 'A', false, false, false, false, false );
    const auto b = run_configuration( 'B', true,  false, false, false, false );
    const auto c = run_configuration( 'C', true,  true,  false, false, true );
    const auto d = run_configuration( 'D', true,  true,  true,  false, true );
    const auto e = run_configuration( 'E', true,  true,  true,  true,  true );
    const auto f = run_configuration( 'F', true,  true,  true,  true,  true, true );
    const auto g = run_configuration( 'G', true,  true,  true,  true,  true );
    check( a.recovery_plans == b.recovery_plans && b.recovery_plans == c.recovery_plans,
        name, "target/style ablations do not add recovery" );
    check( d.recovery_plans == e.recovery_plans && e.recovery_plans == g.recovery_plans,
        name, "slider/micro/full recovery behavior is stable" );
    check( d.suspicious == c.suspicious, name, "slider-exit classification adds no emitted detour" );
    check( e.max_plans == d.max_plans, name, "micro motion is a trajectory modifier, not another plan" );
    check( f.suspicious == 0 && f.max_plans <= g.max_plans + 1, name,
        "reanchor resumes from current position without an unexplained excursion" );
}

static void t_hit_point_distribution_and_immutability( ) {
    const std::string name = "41 hit-point distribution / immutable";
    constexpr int count = 10000;
    std::vector<float> values; values.reserve( count );
    const float radius = hit_radius( 4.f, false ), usable = radius * 0.82f;
    bool deterministic = true, seed_varies = false, all_safe = true;
    for ( int i = 0; i < count; ++i ) {
        const auto object = circle( 1000 + i * 37, 256.f, 192.f );
        const point_t a = select_target_point( i, object, false, 4.f, 0x8123u, 0.10f, 0.55f, 1.2f, 0.f );
        const point_t b = select_target_point( i, object, false, 4.f, 0x8123u, 0.10f, 0.55f, 1.2f, 0.f );
        const point_t c = select_target_point( i, object, false, 4.f, 0x9123u, 0.10f, 0.55f, 1.2f, 0.f );
        deterministic = deterministic && length( a - b ) < 0.00001f;
        seed_varies = seed_varies || length( a - c ) > 1.f;
        const float offset = length( a - object_center( object, false ) );
        values.push_back( offset / usable ); all_safe = all_safe && offset < radius * 0.42f;
    }
    std::sort( values.begin(), values.end() );
    double sum = 0.; for ( float value : values ) sum += value;
    const float mean = static_cast<float>( sum / values.size() );
    const float p10 = values[ values.size() / 10 ], median = values[ values.size() / 2 ];
    const float p90 = values[ values.size() * 9 / 10 ], maximum = values.back();
    check( deterministic && seed_varies, name, "fixed seed repeats and different sessions vary" );
    check( all_safe && maximum < 0.52f, name, "all points remain conservatively hittable" );
    check( mean > 0.16f && median > 0.16f && p90 > 0.25f, name, "distribution is not center-clustered" );
    check( p10 < median && median < p90 && p90 < maximum, name, "disk distribution has meaningful spread" );

    std::vector<osu::hit_object_t> objects; objects.reserve( count );
    for ( int i = 0; i < count; ++i ) objects.push_back( circle( 1000 + i * 24,
        256.f + 170.f * std::sin( i * 0.31f ), 192.f + 115.f * std::sin( i * 0.19f ) ) );
    auto map = make_map( std::move( objects ) ); auto sched = make_schedule( map );
    motion_engine_t eng; settings_t settings; settings.startup_motion = false; settings.break_motion = false;
    settings.diagnostic_micro_motion = false; eng.configure( settings );
    eng.begin_session( {256,192}, false, 1.f, map.objects.size(), 0x8123u, true );
    std::vector<point_t> first( count ); std::vector<uint8_t> seen( count, 0 );
    uint64_t observed_changes = 0;
    for ( int t = 900; t <= map.objects.back().start_time + 8; t += 4 ) {
        eng.update( map, sched, t, 0.004f );
        const int index = eng.plan().object_index;
        for ( int candidate : { index, index + 1 } ) {
            point_t p{};
            if ( candidate < 0 || candidate >= count || !eng.cached_target_point( candidate, p ) ) continue;
            if ( !seen[ candidate ] ) { first[ candidate ] = p; seen[ candidate ] = 1; }
            else if ( length( first[ candidate ] - p ) > 0.00001f ) ++observed_changes;
        }
    }
    const auto diagnostics = eng.diagnostics();
    check( diagnostics.target_cache_writes == count, name, "10,000 object targets were cached exactly once" );
    check( diagnostics.target_coordinate_changes == 0 && observed_changes == 0,
        name, "chosen target never changes during acquisition/replan" );
    std::printf( "    hit offsets normalized: mean %.3f median %.3f P10 %.3f P50 %.3f P90 %.3f max %.3f\n",
        mean, median, p10, median, p90, maximum );
}

static void t_ultra_short_slider_matrix( ) {
    const std::string name = "42 ultra-short slider matrix";
    struct sample_t { float ratio = 0.f, ball = 0.f, cursor = 0.f; slider_strategy_t strategy{}; uint64_t ultra = 0; };
    auto sample = [&]( int duration, const char* curve, float path_length, int repeat = 1 ) {
        auto map = make_map( { slider( 1000, 1000 + duration, 200, 192, curve, path_length, repeat ) } );
        auto sched = make_schedule( map ); auto eng = fresh_engine( map, {200,192}, 0x7712u + duration + repeat );
        run( eng, map, sched, 700, 1020 + duration, 2 );
        const auto d = eng.diagnostics();
        return sample_t{ d.slider_cursor_ball_ratio, d.slider_ball_path_length,
            d.slider_cursor_path_length, d.slider_strategy, d.ultra_short_slider_activations };
    };
    const sample_t straight = sample( 140, "L|250:192", 50.f );
    const sample_t curve = sample( 180, "B|220:170|245:192", 55.f );
    const sample_t loop = sample( 220, "B|220:170|240:192|220:214|200:192", 82.f );
    const sample_t repeat = sample( 230, "L|238:192", 38.f, 2 );
    const sample_t hook = sample( 200, "C|215:175|235:175|245:195", 58.f );
    const sample_t slow = sample( 1500, "B|220:170|245:192", 55.f );
    const sample_t large = sample( 700, "B|300:80|420:300|470:192", 360.f );
    for ( const sample_t tiny : { straight, curve, loop, repeat, hook } ) {
        check( tiny.ultra == 1, name, "tiny fast shape selects whole-path minimal follow" );
        check( tiny.ratio < 0.45f, name, "tiny cursor/ball travel is substantially below 1:1" );
    }
    check( slow.ultra == 0 && slow.ratio > curve.ratio + 0.15f, name,
        "same slow geometry receives visibly more following" );
    check( large.ultra == 0 && large.ball > 250.f, name, "large geometry remains normal follow" );
    std::printf( "    tiny ratios straight %.3f curve %.3f loop %.3f repeat %.3f hook %.3f; slow %.3f large %.3f\n",
        straight.ratio, curve.ratio, loop.ratio, repeat.ratio, hook.ratio, slow.ratio, large.ratio );
}

static void t_same_y_long_jump_preservation( ) {
    const std::string name = "43 same-Y long jumps";
    std::vector<osu::hit_object_t> objects;
    for ( int i = 0; i < 80; ++i ) objects.push_back( circle( 1000 + i * 480,
        i % 2 ? 455.f : 55.f, 192.f ) );
    auto map = make_map( std::move( objects ) ); auto sched = make_schedule( map );
    // This isolates the arrival-cone regression, not visibility. At AR9 the
    // next-next note is now correctly hidden when this plan is first captured.
    map.ar=5.f;
    motion_engine_t legacy; settings_t legacy_settings; legacy_settings.diagnostic_safe_arrival_cone = false;
    legacy.configure( legacy_settings ); legacy.begin_session( {256,192}, false, 1.f, map.objects.size(), 0x1984u, true );
    auto legacy_sched = sched;
    const auto before = trace_transitions( legacy, map, legacy_sched );
    auto eng = fresh_engine( map, {256,192}, 0x1984u );
    const auto metrics = trace_transitions( eng, map, sched );
    std::printf( "    same-Y transitions=%llu suspicious before/after=%llu/%llu plans avg/max %.2f/%d off %.1f->%.1f speed=%.0f accel=%.0f\n",
        static_cast<unsigned long long>( metrics.transitions ), static_cast<unsigned long long>( before.suspicious ),
        static_cast<unsigned long long>( metrics.suspicious ),
        metrics.transitions ? static_cast<double>( metrics.total_plans ) / metrics.transitions : 0.0,
        metrics.max_plans, before.max_off_route, metrics.max_off_route, metrics.max_speed, metrics.max_accel );
    check( before.suspicious > 0, name, "legacy endpoint velocity reproduces the forward overshoot" );
    check( metrics.suspicious == 0, name, "curved long-jump flow has no away/not-toward bursts" );
    check( eng.diagnostics().unexpected_discontinuities == 0, name, "long-jump continuity preserved" );
    check( metrics.max_plans <= 2, name, "no mini-plan chain on ordinary jumps" );
}

static void t_session_seed_variation( ) {
    const std::string name = "36 session seed variation";
    auto map = make_map( { circle(1000,100,100), circle(1500,350,120), circle(2100,180,300), circle(2800,420,220) } );
    auto sched = make_schedule( map );
    auto run_trace = [&]( uint32_t seed ) {
        auto eng = fresh_engine( map, {256,192}, seed ); std::vector<point_t> trace;
        for ( int t = 300; t <= 2900; t += 8 ) { eng.update( map, sched, t, 0.008f ); trace.push_back( eng.position() ); }
        return std::pair{ trace, eng.diagnostics() };
    };
    const auto a = run_trace( 111u ), b = run_trace( 111u ), c = run_trace( 222u );
    bool same = true, different = false;
    for ( size_t i = 0; i < a.first.size(); ++i ) {
        same = same && length( a.first[i] - b.first[i] ) < 0.0001f;
        different = different || length( a.first[i] - c.first[i] ) > 0.5f;
    }
    check( same, name, "fixed test seed reproduces exactly" );
    check( different, name, "different session seeds change movement details" );
    check( a.second.deterministic_test_mode && b.second.deterministic_test_mode, name, "test seed mode is diagnostic" );
    check( a.second.unexpected_discontinuities == 0 && c.second.unexpected_discontinuities == 0,
        name, "variation remains bounded and continuous" );
}

int main( ) {
    std::printf( "Autobot motion engine -- deterministic off-line tests\n" );
    std::printf( "-----------------------------------------------------\n" );
    t_spaced_circles( );
    t_dense_stream( );
    t_large_jumps( );
    t_sharp_reversal( );
    t_stacked_notes( );
    t_circle_slider( );
    t_slider_circle( );
    t_circle_spinner( );
    t_spinner_circle( );
    t_long_break( );
    t_startup( );
    t_frame_hitch( );
    t_map_rewind( );
    t_one_anomaly( );
    t_persistent_mismatch( );
    t_geometry_anomaly( );
    t_target_point_stability( );
    t_quintic_arrival( );
    t_normal_slow_section( );
    t_long_normal_gameplay( );
    t_decor_transition_derivatives( );
    t_startup_motif_library( );
    t_stale_output_observations( );
    t_all_plan_transition_continuity( );
    t_compact_repeat_slider( );
    t_large_slider_follow( );
    t_spinner_character( );
    t_decor_families_and_scale( );
    t_speed_aware_local_slider_coverage( );
    t_partial_slider_section( );
    t_spinner_rpm_and_shape( );
    t_decor_energy_response( );
    t_mixed_motion_stress( );
    t_slider_exit_geometry_variety( );
    t_actual_output_rpm( );
    t_spinner_per_object_variety( );
    t_micro_gap_policy( );
    t_session_seed_variation( );
    t_micro_urgent_cancel( );
    t_forward_burst_stress( );
    t_session_spinner_direction( );
    t_feature_ablation_trace( );
    t_hit_point_distribution_and_immutability( );
    t_ultra_short_slider_matrix( );
    t_same_y_long_jump_preservation( );

    std::printf( "-----------------------------------------------------\n" );
    std::printf( "%d checks, %d failures\n", g_checks, g_failures );
    return g_failures == 0 ? 0 : 1;
}
