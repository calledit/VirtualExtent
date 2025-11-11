#include "pch.h"
#include "controller_config.h"
#include "nlohmann/json.hpp"
#include <fstream>
#include <iostream>
#include <Windows.h>
#include <sstream>

using json = nlohmann::json;

void print_controller_map(const ControllerProfile& profile) {
    std::stringstream ss;

    ss << "=== Controller Profile ===\n";
    ss << "Name: " << profile.name << "\n";

    for (const auto& action : profile.map) {
        ss << "  Mapping: " << action.name << "\n";

        if (action.type)
            ss << "    Type: " << *action.type << "\n";
        if (action.is_x)
            ss << "    is_x: " << (*action.is_x ? "true" : "false") << "\n";
        if (action.if_down_action)
            ss << "    if_down: " << *action.if_down_action
            << (action.key_down ? (" (key: " + std::to_string(*action.key_down) + ")") : "") << "\n";
        if (action.if_up_action)
            ss << "    if_up: " << *action.if_up_action
            << (action.key_up ? (" (key: " + std::to_string(*action.key_up) + ")") : "") << "\n";

        if (action.if_passed_over) {
            const auto& a = *action.if_passed_over;
            ss << "    if_passed_over: " << a.action
                << " at " << a.value
                << (a.key ? (" (key: " + std::to_string(*a.key) + ")") : "") << "\n";
        }

        if (action.if_passed_under) {
            const auto& a = *action.if_passed_under;
            ss << "    if_passed_under: " << a.action
                << " at " << a.value
                << (a.key ? (" (key: " + std::to_string(*a.key) + ")") : "") << "\n";
        }
    }


    std::string str = ss.str();
    OutputDebugStringA(str.c_str());
}



static bool NeedsExtended(WORD vk) {
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
    case VK_PRIOR:  case VK_NEXT:   // PageUp/PageDown
    case VK_RIGHT:  case VK_LEFT:   case VK_UP:   case VK_DOWN:
    case VK_NUMLOCK: case VK_DIVIDE: case VK_RMENU: case VK_RCONTROL:
        // NOTE: main Enter is not extended; numpad Enter would be, but shares VK_RETURN.
        // If you specifically want numpad Enter-as-extended, set the flag yourself.
        return true;
    default: return false;
    }
}

static void SendKeyDown_Scan(WORD vk) {
    HKL kl = GetKeyboardLayout(0);
    UINT sc = MapVirtualKeyEx(vk, MAPVK_VK_TO_VSC_EX, kl);
    if (sc == 0) return;
    DWORD flags = KEYEVENTF_SCANCODE | (NeedsExtended(vk) ? KEYEVENTF_EXTENDEDKEY : 0);

    INPUT in{}; in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)sc;
    in.ki.dwFlags = flags;
    SendInput(1, &in, sizeof(in));
}

static void MouseClick(bool right) {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_MOUSE;
    inputs[1].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    inputs[1].mi.dwFlags = right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
    SendInput(2, inputs, sizeof(INPUT));
}

static void MouseDown(bool right) {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &input, sizeof(INPUT));
}

static void MouseUp(bool right) {
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));
}

static void SendWheel(int wheelDelta) {

    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = wheelDelta; // +120 = scroll up, -120 = scroll down
    SendInput(1, &in, sizeof(INPUT));
}

static void SendKeyUp_Scan(WORD vk) {
    HKL kl = GetKeyboardLayout(0);
    UINT sc = MapVirtualKeyEx(vk, MAPVK_VK_TO_VSC_EX, kl);
    if (sc == 0) return;
    DWORD flags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP | (NeedsExtended(vk) ? KEYEVENTF_EXTENDEDKEY : 0);

    INPUT in{}; in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)sc;
    in.ki.dwFlags = flags;
    SendInput(1, &in, sizeof(in));
}

void handle_action(const MappingAction& m, std::string act) {

    std::stringstream ss; ss << "Handling action: " << act << "\n"; OutputDebugStringA(ss.str().c_str());
    if (act == "mouse_scrool_down") {
        int amount = 120;
        if (m.amount_down.has_value()) {
            amount = m.amount_down.value();
		}
        SendWheel(-amount); // Left down
    }
    else if (act == "mouse_scrool_up") {
        int amount = 120;
        if (m.amount_down.has_value()) {
            amount = m.amount_down.value();
        }
        SendWheel(amount);  // Right down
    }
    if (act == "left_mouse_down") {
        MouseDown(false); // Left down
    }
    else if (act == "right_mouse_down") {
        MouseDown(true);  // Right down
    }
    if (act == "left_mouse_up") {
        MouseUp(false); // Left down
    }
    else if (act == "right_mouse_up") {
        MouseUp(true);  // Right down
    }
    else if (act == "key_down") {
        if (m.key_down.has_value()) {
            SendKeyDown_Scan((WORD)m.key_down.value());
        }
    }
    else if (act == "key_up") {
        if (m.key_up.has_value()) {
            SendKeyUp_Scan((WORD)m.key_up.value());
        }
    }    
    else {
        OutputDebugStringA(("Unknown down action: " + act + "\n").c_str());
    }
}

void deal_with_bool_action(const MappingAction& m, bool state) {
    if (state) {
        

        // Handle down
        if (m.if_down_action.has_value()) {
            const std::string& act = m.if_down_action.value();
            handle_action(m, act);
        }
    }
    else {
        // Handle down
        if (m.if_up_action.has_value()) {
            const std::string& act = m.if_up_action.value();
            handle_action(m, act);
        }
    }
}

void deal_with_float_action(const MappingAction& m, float value, float last_value) {


    //std::stringstream ss; ss << "Handling float action for " << m.name << " with value " << value << " (last: " << last_value << ")\n"; OutputDebugStringA(ss.str().c_str());

    // Passed over threshold (value increased past boundary)
    if (m.if_passed_over.has_value()) {
        const auto& over = m.if_passed_over.value();
        if (last_value < over.value && value >= over.value) {
            handle_action(m, over.action);
        }
    }

    // Passed under threshold (value decreased past boundary)
    if (m.if_passed_under.has_value()) {
        const auto& under = m.if_passed_under.value();
        if (last_value > under.value && value <= under.value) {
            handle_action(m, under.action);
        }
    }
    
	// This function is only called when value changes, so the over and under might not allways be called unless teh user is moving the stick
    // is over threshold (value increased past boundary)
    if (m.if_over.has_value()) {
        const auto& over = m.if_over.value();
        if (value >= over.value) {
            handle_action(m, over.action);
        }
    }
    
    // is over threshold (value decreased past boundary)
    if (m.if_under.has_value()) {
        const auto& under = m.if_under.value();
        if (value <= under.value) {
            handle_action(m, under.action);
        }
    }
}


bool load_controller_config(const std::string& path, ControllerConfig& outConfig) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Failed to open controller config: " << path << "\n";
        return false;
    }

    json j;
    try {
        file >> j;
    }
    catch (const std::exception& e) {
        std::cerr << "Failed to parse JSON: " << e.what() << "\n";
        return false;
    }

    outConfig.profile_name = j.value("profile_name", "");
    outConfig.exe_name_hash = j.value("exe_name_hash", "");

    // Optional integers
    if (j.contains("render_frame_funcnr") && !j["render_frame_funcnr"].is_null())
        outConfig.render_frame_funcnr = j["render_frame_funcnr"].get<int>();
    else
        outConfig.render_frame_funcnr = std::nullopt;

    if (j.contains("game_loop_update_funcnr") && !j["game_loop_update_funcnr"].is_null())
        outConfig.game_loop_update_funcnr = j["game_loop_update_funcnr"].get<int>();
    else
        outConfig.game_loop_update_funcnr = std::nullopt;

    // Controller maps
    for (const auto& prof : j["controller_maps"]) {
        ControllerProfile cp;
        cp.name = prof["name"];

        for (const auto& mapItem : prof["map"]) {
            MappingAction m;
            m.name = mapItem["name"];

            if (mapItem.contains("type"))
                m.type = mapItem["type"];

            if (mapItem.contains("is_x"))
                m.is_x = mapItem["is_x"];

            if (mapItem.contains("if_down")) {
                auto& obj = mapItem["if_down"];
                if (obj.contains("action"))
                    m.if_down_action = obj["action"];
                if (obj.contains("key"))
                    m.key_down = obj["key"];
                if (obj.contains("amount"))
                    m.amount_down = obj["amount"];
            }

            if (mapItem.contains("if_up")) {
                auto& obj = mapItem["if_up"];
                if (obj.contains("action"))
                    m.if_up_action = obj["action"];
                if (obj.contains("key"))
                    m.key_up = obj["key"];
                if (obj.contains("amount"))
                    m.amount_up = obj["amount"];
            }

            if (mapItem.contains("if_passed_over")) {
                auto& obj = mapItem["if_passed_over"];
                MappingAction::ThresholdAction ta;
                ta.value = obj["value"];
                ta.action = obj["action"];
                if (obj.contains("key")){
                    ta.key = obj["key"];
                    if (ta.action == "key_down") {
                        m.key_down = ta.key;
                    }
                    else if (ta.action == "key_up") {
                        m.key_up = ta.key;
					}
                }
                m.if_passed_over = ta;
            }

            if (mapItem.contains("if_passed_under")) {
                auto& obj = mapItem["if_passed_under"];
                MappingAction::ThresholdAction ta;
                ta.value = obj["value"];
                ta.action = obj["action"];
                if (obj.contains("key")) {
                    ta.key = obj["key"];
                    if (ta.action == "key_down") {
                        m.key_down = ta.key;
                    }
                    else if (ta.action == "key_up") {
                        m.key_up = ta.key;
					}
                }
                    
                m.if_passed_under = ta;
            }

            if (mapItem.contains("if_over")) {
                auto& obj = mapItem["if_over"];
                MappingAction::ThresholdAction ta;
                ta.value = obj["value"];
                ta.action = obj["action"];
                if (obj.contains("key")) {
                    ta.key = obj["key"];
                    if (ta.action == "key_down") {
                        m.key_down = ta.key;
                    }
                    else if (ta.action == "key_up") {
                        m.key_up = ta.key;
                    }
                }
                if (obj.contains("amount")) {
                    ta.amount = obj["amount"];
                    if (ta.action == "key_down") {
                        m.amount_down = ta.key;
                    }
                    else if (ta.action == "key_up") {
                        m.amount_down = ta.key;
                    }
                }
                m.if_over = ta;
            }

            if (mapItem.contains("if_under")) {
                auto& obj = mapItem["if_under"];
                MappingAction::ThresholdAction ta;
                ta.value = obj["value"];
                ta.action = obj["action"];
                if (obj.contains("key")) {
                    ta.key = obj["key"];
                    if (ta.action == "key_down") {
                        m.key_down = ta.key;
                    }
                    else if (ta.action == "key_up") {
                        m.key_up = ta.key;
                    }
                }
                if (obj.contains("amount")) {
                    ta.amount = obj["amount"];
                    if (ta.action == "key_down") {
                        m.amount_down = ta.key;
                    }
                    else if (ta.action == "key_up") {
                        m.amount_down = ta.key;
                    }
                }

                m.if_under = ta;
            }

            cp.map.push_back(m);
        }

        outConfig.controller_maps.push_back(cp);
    }
    return true;
}
