// Port of mel/decision/rules.py: the deterministic survey / no-survey cascade.
//   R1 junk_insight              quality_flag == junk, or fails the insight schema
//   R2 critical_needs_human_call severity == critical (MEL_CRITICAL_POLICY=survey skips R2)
//   R3 nothing_to_verify         resolution generic_information
//   R4 waiting_for_details       resolution needs_more_context
//   R5 no_check_needed           verification.needed is false
//   R6 window_after_season       no usable window before season end
//   R7 crop_already_harvested    standing-crop advice after harvest (not storage advice)
//   R8 survey
#include "engine.hpp"

#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace mel {
namespace {

struct Reason {
    const char* schema_code;  // no_survey.schema.json only allows these two codes
    const char* text;
};

// window_passed is not here: it needs the simulated clock, which is not ported.
const std::map<std::string, Reason> REASONS{
    {"junk_insight", {"generic_information", "Broken or empty insight; nothing reliable to ask about."}},
    {"critical_needs_human_call",
     {"needs_more_context", "Critical case: a person should call the farmer instead of an automated survey."}},
    {"nothing_to_verify",
     {"generic_information", "General information only. No action was advised, so there is nothing to check."}},
    {"waiting_for_details", {"needs_more_context", "FarmerChat asked for more details; no advice was given yet."}},
    {"no_check_needed", {"generic_information", "The insight says this advice does not need verification."}},
    {"window_after_season",
     {"generic_information", "No usable send window remains before the rabi season ends (31 March)."}},
    {"crop_already_harvested",
     {"generic_information", "The crop will already be harvested by the time the survey would go out."}},
};

Decision no(const char* rule, const char* code) { return {false, rule, code, std::nullopt}; }

// object.get(key) == text
bool field_is(const json& object, const char* key, const char* text) {
    auto field = object.find(key);
    return field != object.end() && field->is_string() && field->get_ref<const std::string&>() == text;
}

// A setting from the environment; empty counts as unset.
// ponytail: the environment only. The Python also reads the repo's .env; read it here if settings move there.
std::string setting(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}

}  // namespace

Engine load_engine(const std::string& rules_path, const std::string& calendar_path) {
    std::ifstream rules(rules_path), calendar(calendar_path);
    if (!rules) throw std::runtime_error("cannot open " + rules_path);
    if (!calendar) throw std::runtime_error("cannot open " + calendar_path);
    if (setting("MEL_TIMING_BASIS", "brief") != "brief")
        throw std::invalid_argument("only MEL_TIMING_BASIS=brief is ported to C++");
    std::string policy = setting("MEL_CRITICAL_POLICY", "human_call");
    if (policy != "human_call" && policy != "survey")
        throw std::invalid_argument("MEL_CRITICAL_POLICY must be 'human_call' or 'survey'");
    return {json::parse(rules), load_harvest_end(calendar), policy == "human_call"};
}

Decision decide(const json& insight, const Engine& engine) {
    // R1. After this every schema field exists with the right type, so the rules below read them directly.
    if (!insight.is_object() || field_is(insight, "quality_flag", "junk") || !is_valid_insight(insight))
        return no("R1", "junk_insight");
    // The schema pattern cannot see an impossible date (30 February): that is still R1, not a crash.
    auto created = parse_ist_date(insight.at("created_at").get_ref<const std::string&>());
    if (!created) return no("R1", "junk_insight");

    if (engine.human_call && field_is(insight, "severity", "critical")) return no("R2", "critical_needs_human_call");

    const std::string& resolution = insight.at("resolution_type").get_ref<const std::string&>();
    const json& by_resolution = engine.rules.at("survey_by_resolution");
    auto entry = by_resolution.find(resolution);
    if (entry == by_resolution.end() || !entry->value("survey", false))
        return resolution == "needs_more_context" ? no("R4", "waiting_for_details") : no("R3", "nothing_to_verify");

    if (!insight.at("verification").at("needed").get<bool>()) return no("R5", "no_check_needed");

    auto window = send_window(insight, *created, engine.rules);
    if (!window) return no("R6", "window_after_season");

    const json& asset = insight.at("asset");
    bool stored = field_is(asset, "growth_stage", "post_harvest") || field_is(insight, "concern", "storage_post_harvest");
    if (field_is(asset, "category", "crop") && !stored && is_past_harvest(asset.at("name"), window->first_day, engine))
        return no("R7", "crop_already_harvested");

    return {true, "R8", "", window};
}

// Decision.no_survey_record(): the record that validates against no_survey.schema.json.
json no_survey_record(const Decision& decision, const json& insight) {
    auto record_id = [&](const char* key) -> std::string {
        auto field = insight.find(key);
        if (field == insight.end() || !field->is_string() || field->get_ref<const std::string&>().empty()) return "unknown";
        return utf8_prefix(field->get_ref<const std::string&>(), 64);
    };
    const Reason& reason = REASONS.at(decision.reason_code);
    return {
        {"insight_id", record_id("insight_id")},
        {"farmer_id", record_id("farmer_id")},
        {"decision", "no_survey"},
        {"reason_code", reason.schema_code},
        {"reason", utf8_prefix("[" + decision.rule + " " + decision.reason_code + "] " + reason.text, 300)},
    };
}

}  // namespace mel
