#pragma once

#include <impl/struct/osu_types.hxx>
#include <string>
#include <cstdint>
#include <memory>

namespace osu {

    struct live_hit_object_t {
        uint64_t source_object = 0;
        int32_t start_time = 0;
        bool judged = false;
        bool hit = false;
        bool hovered = false;
        bool slider = false;
        bool tracking = false;
        float client_x = 0.f, client_y = 0.f, radius = 0.f;
        float ball_x = 0.f, ball_y = 0.f;
        double great_window = 0.0, late_window = 0.0;
        double ok_window = 0.0;
        int result = 0;
        bool result_offset_valid = false;
        double result_offset = 0.0;
    };

    struct live_playfield_t {
        uint64_t drawable_ruleset = 0;
        uint64_t sampled_wall_ms = 0;
        int32_t game_time = 0;
        bool paused = false;
        bool replay = false;
        std::vector<live_hit_object_t> objects;
    };

    struct game_snapshot_t {
        bool attached = false;
        client_kind_t client = client_kind_t::none;
        int32_t pid = 0;
        int32_t cur_time = 0;
        game_state_t cur_state = game_state_t::main_menu;
        int32_t cur_mod_state = 0;
        int32_t map_id = 0;
        int32_t set_id = 0;
        std::string map_folder;
        std::string map_file;
        std::string beatmap_hash;
        std::string beatmap_version;
        uint64_t game_base = 0;
        uint64_t player_screen = 0;
        uint64_t drawable_ruleset = 0;
        bool lazer_player_api_valid = false;
        std::string client_version;
        std::string offset_version;
        bool offset_mismatch = false;
        std::string songs_path;
        int32_t left_key = 'S';
        int32_t right_key = 'D';
        float speed_mult = 1.f;
        bool is_replay = false;
        std::shared_ptr<const live_playfield_t> live_playfield;
    };

    struct full_snapshot_t {
        game_snapshot_t game;
        beatmap_data_t beatmap;
    };

}
