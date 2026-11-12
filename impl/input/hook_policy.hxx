#pragma once

namespace input {
    // Normal Relax generates keys; it never needs to intercept physical input.
    constexpr bool needs_keyboard_hook(bool relax_enabled, bool smart_mode, bool tap_enabled) {
        return (relax_enabled && smart_mode) || tap_enabled;
    }
}
