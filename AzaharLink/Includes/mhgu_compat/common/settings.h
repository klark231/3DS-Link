#pragma once
// The bridge's settings. In Citra MMJ they come from the app's "mmj_mhgu" preferences (hook.cpp).
#include <string>
namespace Settings {
template <typename T>
struct Value {
    T value;
    const T& GetValue() const { return value; }
};
struct Values {
    Value<bool> mhgu_hub_enabled{false};
    Value<std::string> mhgu_hub_servers{""};
    Value<std::string> mhgu_hub_key{""};
    Value<bool> mhgu_hub_convert{true};
    Value<bool> mhgu_hub_log{false};
};
extern Values values;
} // namespace Settings
