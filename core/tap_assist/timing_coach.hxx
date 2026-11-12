#pragma once
#include <impl/struct/game_snapshot.hxx>
#include <mutex>
#include <deque>
#include <set>
#include <cmath>
#include <algorithm>

namespace timing {
struct summary_t {
    size_t hits = 0, missed = 0, recent = 0;
    double last = 0, mean = 0;
    bool manual = false, live = false;
};
class coach_t {
    mutable std::mutex mutex;
    summary_t stats;
    std::deque<double> offsets;
    std::set<size_t> seen;
    uint64_t ruleset = 0;
    int previous_time = 0, map_id = 0;
    bool was_play = false;
public:
    void update(const osu::game_snapshot_t& game, const osu::beatmap_data_t& map, bool manual) {
        std::lock_guard<std::mutex> lock(mutex);
        const bool play = game.cur_state == osu::game_state_t::play;
        if (play && (!was_play || ruleset != game.drawable_ruleset || map_id != game.map_id ||
                     game.cur_time < previous_time - 200)) {
            stats = {}; offsets.clear(); seen.clear();
        }
        was_play = play; ruleset = game.drawable_ruleset; map_id = game.map_id;
        previous_time = game.cur_time;
        stats.manual = manual;
        const auto& frame = game.live_playfield;
        stats.live = play && map.loaded && frame && frame->drawable_ruleset == game.drawable_ruleset &&
            std::abs(frame->game_time - game.cur_time) <= 32 && !frame->replay && !game.is_replay;
        if (!stats.live) return;
        // Mark assisted judgments as seen as well, so switching modes cannot import them.
        for (const auto& hit : frame->objects) {
            if (!hit.judged) continue;
            size_t index = map.objects.size(), matches = 0, timestamp_index = 0;
            for (size_t i = 0; i < map.objects.size(); ++i) {
                const auto& object = map.objects[i];
                if (hit.source_object && object.source_object == hit.source_object) { index = i; break; }
                if (object.start_time == hit.start_time) { ++matches; timestamp_index = i; }
            }
            if (index == map.objects.size() && matches == 1) index = timestamp_index;
            if (index == map.objects.size() || seen.count(index)) continue;
            if (!manual) { seen.insert(index); continue; }
            if (!hit.hit) { seen.insert(index); ++stats.missed; continue; }
            if (!hit.result_offset_valid || !std::isfinite(hit.result_offset) ||
                !std::isfinite(game.speed_mult) || game.speed_mult <= 0) continue;
            seen.insert(index);
            stats.last = hit.result_offset / game.speed_mult;
            ++stats.hits;
            offsets.push_back(stats.last);
            if (offsets.size() > 50) offsets.pop_front();
            stats.recent = offsets.size();
            double total = 0;
            for (double value : offsets) total += value;
            stats.mean = total / offsets.size();
        }
    }
    summary_t summary() const {
        std::lock_guard<std::mutex> lock(mutex);
        return stats;
    }
};
}
