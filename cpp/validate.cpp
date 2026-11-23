// Port of is_valid("insight", ...) in mel/data/validate.py: config/schemas/insight.schema.json written out by hand.
// ponytail: only this one schema, hand-written; if the schema changes this file changes with it. A JSON Schema
// library is the upgrade once more than one schema needs checking.
#include "engine.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <regex>

namespace mel {
namespace {

using Names = std::initializer_list<const char*>;

// In UTF-8 every character starts with one byte that is not 10xxxxxx.
bool starts_char(unsigned char byte) { return (byte & 0xC0) != 0x80; }

bool has(Names names, const std::string& name) {
    return std::any_of(names.begin(), names.end(), [&](const char* each) { return name == each; });
}

// type object + required + additionalProperties false.
bool is_record(const json& value, Names required, Names optional = {}) {
    if (!value.is_object()) return false;
    for (const char* name : required)
        if (!value.contains(name)) return false;
    for (const auto& item : value.items())
        if (!has(required, item.key()) && !has(optional, item.key())) return false;
    return true;
}

// type string + minLength 1 + maxLength. JSON Schema counts characters, not bytes.
bool is_text(const json& value, std::size_t max_chars) {
    if (!value.is_string()) return false;
    const std::string& text = value.get_ref<const std::string&>();
    auto length = static_cast<std::size_t>(std::count_if(text.begin(), text.end(), starts_char));
    return length >= 1 && length <= max_chars;
}

// enum of names; `nullable` when the enum also lists null.
bool is_one_of(const json& value, Names names, bool nullable = false) {
    if (value.is_null()) return nullable;
    return value.is_string() && has(names, value.get_ref<const std::string&>());
}

// type string + pattern.
bool matches(const json& value, const std::regex& pattern) {
    return value.is_string() && std::regex_search(value.get_ref<const std::string&>(), pattern);
}

// type [integer, null] + minimum 0 + maximum 180. JSON Schema counts 3.0 as an integer; true is not a number.
bool is_action_window(const json& value) {
    if (value.is_null()) return true;
    if (!value.is_number()) return false;
    double days = value.get<double>();
    return days == std::floor(days) && days >= 0 && days <= 180;
}

// Python's `$` also matches just before one trailing newline ("hi-IN\n" passes there); `\n?` keeps that.
// created_at needs no `\n?`: with a trailing newline Python's date parser rejects it, so it is R1 either way.
const std::regex CREATED_AT{R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?\+05:30$)"};
const std::regex LANGUAGE{R"(^[a-z]{2,3}-[A-Z]{2}\n?$)"};
const std::regex COUNTRY{R"(^[A-Z]{2}\n?$)"};
const std::regex ASSET_NAME{R"(^[a-z][a-z ()-]*\n?$)"};

}  // namespace

// text[:max_chars] in Python: the first max_chars characters, never half of one.
std::string utf8_prefix(const std::string& text, std::size_t max_chars) {
    std::size_t seen = 0;
    for (std::size_t i = 0; i < text.size(); ++i)
        if (starts_char(text[i]) && seen++ == max_chars) return text.substr(0, i);
    return text;
}

bool is_valid_insight(const json& insight) {
    if (!is_record(insight,
                   {"insight_id", "farmer_id", "conversation_id", "created_at", "conversation_language", "location",
                    "asset", "concern", "behavioral_intent", "decision_episode_type", "severity", "query_summary",
                    "advice_summary", "resolution_type", "action_window_days", "verification", "preferred_channel"},
                   {"quality_flag"}))
        return false;
    // Every required key exists from here on. The nested ones are read only after their own is_record passed
    // (&& stops at the first false).
    const json& location = insight["location"];
    const json& asset = insight["asset"];
    const json& check = insight["verification"];
    return is_text(insight["insight_id"], 64) && is_text(insight["farmer_id"], 64) &&
           is_text(insight["conversation_id"], 64) && matches(insight["created_at"], CREATED_AT) &&
           matches(insight["conversation_language"], LANGUAGE) &&
           is_record(location, {"country", "state", "district"}) && matches(location["country"], COUNTRY) &&
           is_text(location["state"], 80) && is_text(location["district"], 80) &&
           is_record(asset, {"name", "category", "growth_stage"}) &&
           (asset["name"].is_null() || (is_text(asset["name"], 60) && matches(asset["name"], ASSET_NAME))) &&
           is_one_of(asset["category"], {"crop", "livestock"}, true) &&
           is_one_of(asset["growth_stage"],
                     {"germination", "seedling", "vegetative", "tillering", "flowering", "fruiting", "maturity",
                      "harvest", "post_harvest", "dormant"},
                     true) &&
           is_one_of(insight["concern"],
                     {"pest_disease", "weather_climate", "growth_issue", "nutrient_deficiency", "variety_selection",
                      "soil_health", "water_stress", "storage_post_harvest", "livestock_nutrition",
                      "livestock_health", "market_price", "weed_management", "breeding_reproduction", "yield_loss"},
                     true) &&
           is_one_of(insight["behavioral_intent"],
                     {"procedural_guidance", "information_seeking", "diagnosing", "planning", "profile_update",
                      "exploring_options", "symptom_reporting", "confirming", "decision_making", "market_inquiry",
                      "following_up", "comparing", "emergency"}) &&
           is_one_of(insight["decision_episode_type"],
                     {"decision", "problem_resolution", "general_knowledge", "casual", "exploration"}, true) &&
           is_one_of(insight["severity"], {"low", "medium", "high", "critical"}, true) &&
           is_text(insight["query_summary"], 400) && is_text(insight["advice_summary"], 600) &&
           is_one_of(insight["resolution_type"],
                     {"direct_action", "generic_information", "plan_created", "needs_more_context", "do_not_act",
                      "wait_and_monitor", "escalate_to_expert"}) &&
           is_action_window(insight["action_window_days"]) &&
           is_record(check, {"needed", "timing", "method"}) && check["needed"].is_boolean() &&
           is_one_of(check["timing"],
                     {"none", "same_day", "next_day", "two_to_five_days", "three_to_ten_days",
                      "seven_to_fourteen_days", "seasonal"}) &&
           is_one_of(check["method"], {"none", "push", "whatsapp", "ivr_callback", "repeat_photo", "agent_followup"}) &&
           is_one_of(insight["preferred_channel"], {"app", "web", "whatsapp", "telegram", "voice"}) &&
           (!insight.contains("quality_flag") || is_one_of(insight["quality_flag"], {"clean", "junk"}));
}

}  // namespace mel
