// C++ port of the survey / no-survey cascade in mel/decision/rules.py (rules R1-R8), with the parts of
// mel/timing (send window, crop calendar) and mel/data/validate.py (insight schema) that the cascade calls.
// Ported: the brief timing basis, decided as of the insight's own time.
// Not ported: pick_send_time, the simulated clock (`now`, R6 window_passed), MEL_TIMING_BASIS=answer_key.
#pragma once

#include <chrono>
#include <cstddef>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "third_party/nlohmann/json.hpp"

namespace mel {

using json = nlohmann::json;
using Date = std::chrono::sys_days;  // a calendar day: Date + days{3} and Date < Date both work

struct Window {
    Date first_day;  // the day in not_before, kept for the R7 harvest check
    std::string not_before, not_after;  // "2026-12-06T07:00:00+05:30"
};

struct Decision {
    bool survey;
    std::string rule;         // "R1" .. "R8"
    std::string reason_code;  // empty for a survey
    std::optional<Window> send_window;
};

struct Engine {
    json rules;                               // config/rules/rules.json
    std::map<std::string, Date> harvest_end;  // crop -> last harvest day, from the crop calendar
    bool human_call = true;                   // MEL_CRITICAL_POLICY: R2 on (default) or off
};

// validate.cpp
bool is_valid_insight(const json& insight);
std::string utf8_prefix(const std::string& text, std::size_t max_chars);

// timing.cpp
Date config_date(std::string_view iso);
std::optional<Date> parse_ist_date(const std::string& created_at);
std::optional<Window> send_window(const json& insight, Date created, const json& rules);
std::map<std::string, Date> load_harvest_end(std::istream& csv);
bool is_past_harvest(const json& crop, Date on, const Engine& engine);

// decision.cpp
Engine load_engine(const std::string& rules_path, const std::string& calendar_path);
Decision decide(const json& insight, const Engine& engine);
json no_survey_record(const Decision& decision, const json& insight);

}  // namespace mel
