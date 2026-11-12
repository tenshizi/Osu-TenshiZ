#pragma once

#include <impl/memory/process.hxx>
#include <impl/struct/game_snapshot.hxx>
#include <algorithm>
#include <cmath>

namespace game {

    // Read-only layout verified against the installed 2026.1005.0 assemblies.
    // These are native hit areas and pool-entry judgments, not estimated paths.
    class c_lazer_live_feedback {
        static bool pointer( uint64_t p ) { return p >= 0x10000 && p < 0x0000800000000000ull && !(p & 7); }
        static bool read_list( memory::c_process& p, uint64_t object, std::vector<uint64_t>& out ) {
            if ( !pointer(object) ) return false;
            int count = 0;
            if ( !p.read_buffer(object + 0x10, &count, sizeof count) || count < 0 || count > 512 ) return false;
            const auto array = p.read<uint64_t>(object + 8);
            if ( count && !pointer(array) ) return false;
            out.resize(count);
            return !count || p.read_buffer(array + 0x10, out.data(), count * sizeof(uint64_t));
        }
        static bool bounds( memory::c_process& p, uint64_t area, float& x, float& y, float& radius ) {
            if ( !pointer(area) ) return false;
            const auto draw = p.read<uint64_t>(area + 0xb0);
            const auto size = p.read<uint64_t>(area + 0x70);
            if ( !pointer(draw) || !pointer(size) ) return false;
            float m[9]{}, s[2]{};
            if ( !p.read_buffer(draw + 0x2c, m, sizeof m) || !p.read_buffer(size + 0x2c, s, sizeof s) ) return false;
            for ( float v : m ) if ( !std::isfinite(v) ) return false;
            if ( !std::isfinite(s[0]) || !std::isfinite(s[1]) || s[0] <= 0 || s[1] <= 0 ) return false;
            x = .5f * (s[0]*m[0] + s[1]*m[3]) + m[6];
            y = .5f * (s[0]*m[1] + s[1]*m[4]) + m[7];
            radius = .5f * std::min(s[0]*std::hypot(m[0],m[1]), s[1]*std::hypot(m[3],m[4]));
            return std::isfinite(x) && std::isfinite(y) && std::isfinite(radius) && radius >= 1.f && radius < 2000.f;
        }
        static uint64_t model( memory::c_process& p, uint64_t drawable, uint64_t& result ) {
            const auto entry = p.read<uint64_t>(drawable + 0x320);
            if ( !pointer(entry) ) return 0;
            result = p.read<uint64_t>(entry + 0x48);
            const auto object = p.read<uint64_t>(entry + 0x38);
            return pointer(object) ? object : 0;
        }
        static bool start( memory::c_process& p, uint64_t object, double& time ) {
            const auto bindable = p.read<uint64_t>(object + 0x10);
            return pointer(bindable) && p.read_buffer(bindable + 0x40, &time, sizeof time) &&
                std::isfinite(time) && time >= -10000 && time <= 3600000;
        }
    public:
        std::shared_ptr<const osu::live_playfield_t> read( memory::c_process& p, uint64_t ruleset, int time ) const {
            if ( !pointer(ruleset) ) return {};
            const auto lazy = p.read<uint64_t>(ruleset + 0x360);
            const auto field = pointer(lazy) ? p.read<uint64_t>(lazy + 0x18) : 0;
            const auto container_lazy = pointer(field) ? p.read<uint64_t>(field + 0x320) : 0;
            const auto container = pointer(container_lazy) ? p.read<uint64_t>(container_lazy + 0x18) : 0;
            const auto sorted = pointer(container) ? p.read<uint64_t>(container + 0x230) : 0;
            const auto list = pointer(sorted) ? p.read<uint64_t>(sorted + 8) : 0;
            std::vector<uint64_t> children;
            if ( !read_list(p,list,children) ) return {};
            auto frame = std::make_shared<osu::live_playfield_t>();
            frame->drawable_ruleset = ruleset;
            frame->sampled_wall_ms = GetTickCount64();
            frame->game_time = time;
            const auto paused = p.read<uint64_t>(ruleset + 0x318);
            const auto replay = p.read<uint64_t>(ruleset + 0x310);
            uint8_t pause_value = 0, replay_value = 0;
            if ( !pointer(paused) || !pointer(replay) ||
                 !p.read_buffer(paused + 0x40,&pause_value,1) || pause_value > 1 ||
                 !p.read_buffer(replay + 0x40,&replay_value,1) || replay_value > 1 ) return {};
            frame->paused = pause_value != 0;
            frame->replay = replay_value != 0;
            for ( auto drawable : children ) {
                if ( !pointer(drawable) || !p.read<uint8_t>(drawable + 0x31a) ) return {};
                uint64_t parent_result = 0;
                const auto object = model(p,drawable,parent_result);
                double parent_time = 0;
                if ( !object || !start(p,object,parent_time) ) return {};
                osu::live_hit_object_t state{};
                state.source_object = object;
                state.start_time = static_cast<int>(std::lround(parent_time));
                uint64_t head = drawable;
                const auto manager = p.read<uint64_t>(drawable + 0x430);
                state.slider = pointer(manager) && p.read<uint64_t>(manager + 0x208) == drawable;
                std::vector<uint64_t> nested;
                if ( !read_list(p,p.read<uint64_t>(drawable + 0x350),nested) ) return {};
                if ( state.slider ) {
                    head = 0;
                    for ( auto child : nested ) {
                        uint64_t r = 0; double child_time = 0;
                        const auto child_object = model(p,child,r);
                        if ( child_object && start(p,child_object,child_time) && std::abs(child_time-parent_time) < .5 ) {
                            head = child; break;
                        }
                    }
                    if ( !head ) return {};
                    state.tracking = p.read<uint8_t>(manager + 0x1f4) != 0;
                    float ball_radius = 0;
                    if ( !bounds(p,p.read<uint64_t>(drawable + 0x408),state.ball_x,state.ball_y,ball_radius) ) return {};
                }
                else if ( !nested.empty() ) continue; // spinner; duration holds use the existing scheduler
                uint64_t result = 0;
                const auto head_object = model(p,head,result);
                if ( !head_object || !pointer(result) ) return {};
                int grade = 0;
                if ( !p.read_buffer(result + 0x20,&grade,sizeof grade) || grade < 0 || grade > 50 ) return {};
                state.judged = grade != 0;
                state.hit = grade > 1;
                state.result = grade;
                double result_time = 0, head_start = 0;
                if ( grade && p.read<uint8_t>(result + 0x38) == 1 &&
                     p.read_buffer(result + 0x40,&result_time,sizeof result_time) &&
                     start(p,head_object,head_start) && std::isfinite(result_time) && std::abs(result_time-head_start) <= 500 ) {
                    state.result_offset_valid = true;
                    state.result_offset = result_time-head_start;
                }
                const auto area = p.read<uint64_t>(head + 0x410);
                if ( !bounds(p,area,state.client_x,state.client_y,state.radius) ) return {};
                uint8_t hovered = 0;
                if ( !p.read_buffer(area + 0x144,&hovered,1) || hovered > 1 ) return {};
                state.hovered = hovered != 0;
                const auto windows = p.read<uint64_t>(head_object + 0x20);
                if ( !pointer(windows) ||
                     !p.read_buffer(windows + 8,&state.great_window,sizeof(double)) ||
                     !p.read_buffer(windows + 0x10,&state.ok_window,sizeof(double)) ||
                     !p.read_buffer(windows + 0x18,&state.late_window,sizeof(double)) ||
                     !std::isfinite(state.great_window) || state.great_window < 5 || state.great_window > 100 ||
                     !std::isfinite(state.ok_window) || state.ok_window < state.great_window || state.ok_window > state.late_window ||
                     !std::isfinite(state.late_window) || state.late_window < state.great_window || state.late_window > 250 ) return {};
                frame->objects.push_back(state);
            }
            std::sort(frame->objects.begin(),frame->objects.end(),[](const auto& a,const auto& b){return a.start_time < b.start_time;});
            return frame;
        }
    };
}
