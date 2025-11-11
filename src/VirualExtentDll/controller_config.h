#pragma once
#include <string>
#include <vector>
#include <optional>
#include <openxr/openxr.h>

struct MappingAction {
    std::string name;

    XrPath xr_path = XR_NULL_PATH;
    XrAction xr_action = XR_NULL_HANDLE;
    XrActionType xr_actionType;
    XrBool32 xr_boolean = XR_FALSE;
    XrSpace xr_space = XR_NULL_HANDLE;
    XrPosef xr_posef = { {0, 0, 0, 1}, {0, 0, 0} };

	bool last_bool_state = false;
	float last_float_state = 0.0f;

    std::optional<std::string> type;
    std::optional<bool> is_x;

    // Actions for digital input
    std::optional<int> key_down;
    std::optional<int> key_up;

    std::optional<int> amount_up;
    std::optional<int> amount_down;

    // Threshold actions for analog
    struct ThresholdAction {
        float value;
        std::string action;
        std::optional<int> key;
        std::optional<int> amount;
    };
    std::optional<ThresholdAction> if_passed_over;
    std::optional<ThresholdAction> if_passed_under;
    std::optional<ThresholdAction> if_over;
    std::optional<ThresholdAction> if_under;
    std::optional<std::string> if_down_action;
    std::optional<std::string> if_up_action;
};

struct ControllerProfile {
    std::string name; // "/interaction_profiles/khr/simple_controller"
    std::vector<MappingAction> map;
};

struct ControllerConfig {
    std::string profile_name;
    std::string exe_name_hash;
    std::optional<int> render_frame_funcnr;
    std::optional<int> game_loop_update_funcnr;
    std::vector<ControllerProfile> controller_maps;
};

// Function declaration
bool load_controller_config(const std::string& path, ControllerConfig& outConfig);
void print_controller_map(const ControllerProfile& profile);
void deal_with_float_action(const MappingAction& m, float value, float last_value);
void deal_with_bool_action(const MappingAction& m, bool state);