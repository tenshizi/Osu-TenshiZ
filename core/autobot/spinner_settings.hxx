#pragma once
#include <algorithm>
#include <cmath>

namespace autobot {
    struct spinner_rpm_range_t { float minimum=400.f, maximum=440.f; };
    inline spinner_rpm_range_t sanitize_spinner_range(float minimum,float maximum) {
        if(!std::isfinite(minimum)) minimum=400.f;
        if(!std::isfinite(maximum)) maximum=440.f;
        minimum=std::clamp(minimum,0.f,477.f);maximum=std::clamp(maximum,0.f,477.f);
        if(minimum>maximum) std::swap(minimum,maximum);
        return {minimum,maximum};
    }
    inline spinner_rpm_range_t migrate_spinner_rpm(float legacy) {
        if(!std::isfinite(legacy)) legacy=420.f;
        legacy=std::clamp(legacy,0.f,477.f);
        const float spread=std::min(20.f,legacy*0.05f);
        return sanitize_spinner_range(legacy-spread,legacy+spread);
    }
}
