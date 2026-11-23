// Port of mel/timing/window.py (brief basis) and mel/timing/calendar.py.
#include "engine.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <istream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mel {
namespace {

using std::chrono::days;

// The `len` digits starting at `pos` as a number, or -1 when they are not all digits.
int number(std::string_view text, std::size_t pos, std::size_t len) {
    int n = 0;
    for (std::size_t i = pos; i < pos + len; ++i) {
        if (i >= text.size() || text[i] < '0' || text[i] > '9') return -1;
        n = n * 10 + (text[i] - '0');
    }
    return n;
}

// "YYYY-MM-DD" -> the day, or nothing when it is not a real one (year 0, month 13, 30 February).
std::optional<Date> parse_date(std::string_view iso) {
    using namespace std::chrono;
    int y = number(iso, 0, 4), m = number(iso, 5, 2), d = number(iso, 8, 2);
    if (iso.size() != 10 || iso[4] != '-' || iso[7] != '-' || y < 1 || m < 0 || d < 0) return std::nullopt;
    year_month_day day{year{y}, month{static_cast<unsigned>(m)}, std::chrono::day{static_cast<unsigned>(d)}};
    if (!day.ok()) return std::nullopt;
    return sys_days{day};
}

// fmt_ist(): `day` at "HH:MM" India time.
std::string at(Date day, const std::string& hhmm) {
    std::chrono::year_month_day ymd{day};
    int hour = std::stoi(hhmm), minute = std::stoi(hhmm.substr(hhmm.find(':') + 1));
    char out[40];
    std::snprintf(out, sizeof out, "%04d-%02u-%02uT%02d:%02d:00+05:30", static_cast<int>(ymd.year()),
                  static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()), hour, minute);
    return out;
}

// window_days(): (start, end) day offsets from the insight's date.
std::pair<int, int> window_days(const json& insight, const json& rules) {
    const json& by_timing = rules.at("send_window_days");
    const json& wr = rules.at("window_rules");
    const std::string& timing = insight.at("verification").at("timing").get_ref<const std::string&>();
    const json& given = insight.at("action_window_days");
    int action_days = given.is_null() ? wr.at("action_window_days_if_null").get<int>() : given.get<int>();

    int start, end;
    if (timing == "none" || by_timing.value(timing, json()) == "from_action_window") {
        start = action_days + wr.at("none_start_after_action_window_days").get<int>();
        end = action_days + wr.at("none_end_after_action_window_days").get<int>();
    } else {
        start = by_timing.at(timing).at(0).get<int>();
        end = by_timing.at(timing).at(1).get<int>();
    }
    start = std::max(start, wr.at("earliest_start_day").get<int>());
    if (wr.at("start_not_before_action_window_end").get<bool>()) start = std::max(start, action_days);
    end = std::max(end, start + wr.at("min_width_days").get<int>());
    return {start, end};
}

// One CSV line -> its fields; "quoted, fields" and "" inside quotes are handled.
// ponytail: a quoted field may not contain a line break (the calendar has none); use a CSV library if one ever does.
std::vector<std::string> csv_fields(const std::string& line) {
    std::vector<std::string> fields(1);
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"' && quoted && i + 1 < line.size() && line[i + 1] == '"') {
            fields.back() += line[++i];  // "" inside quotes is one literal quote
        } else if (c == '"') {
            quoted = !quoted;
        } else if (c == ',' && !quoted) {
            fields.emplace_back();
        } else {
            fields.back() += c;
        }
    }
    return fields;
}

// _crop_key(): trimmed, lower case, aliases folded (chickpea -> gram). ASCII only, as crop names are.
std::string crop_key(std::string crop) {
    auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    crop.erase(crop.begin(), std::find_if_not(crop.begin(), crop.end(), space));
    crop.erase(std::find_if_not(crop.rbegin(), crop.rend(), space).base(), crop.end());
    std::transform(crop.begin(), crop.end(), crop.begin(), [](unsigned char c) { return std::tolower(c); });
    static const std::map<std::string, std::string> aliases{{"chickpea", "gram"}, {"chickpeas", "gram"}, {"peas", "pea"}};
    auto alias = aliases.find(crop);
    return alias == aliases.end() ? crop : alias->second;
}

}  // namespace

// A date from the config or the calendar: it has to be real, so a bad one is an error, not a decision.
Date config_date(std::string_view iso) {
    auto day = parse_date(iso);
    if (!day) throw std::invalid_argument("not a date: " + std::string(iso));
    return *day;
}

// parse_ist() for a created_at that already matched the schema pattern, so it ends in +05:30 and the India
// date is the date written. Returns the day, or nothing for an impossible date or time (Python raises there).
// ponytail: the first 5.5 hours of year 1 overflow in Python (so R1); here they are an ordinary date.
std::optional<Date> parse_ist_date(const std::string& created_at) {
    int hour = number(created_at, 11, 2), minute = number(created_at, 14, 2), second = number(created_at, 17, 2);
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return std::nullopt;
    return parse_date(std::string_view(created_at).substr(0, 10));
}

// send_window(): India-time bounds, or nothing when no usable window is left before the season ends.
std::optional<Window> send_window(const json& insight, Date created, const json& rules) {
    const json& wr = rules.at("window_rules");
    auto [start, end] = window_days(insight, rules);
    Date season_end = config_date(wr.at("season_end").get<std::string>());
    Date first = created + days{start};
    Date last = std::min(created + days{end}, season_end);
    if (first > season_end || (last - first).count() < wr.at("min_width_days").get<int>()) return std::nullopt;
    return Window{first, at(first, wr.at("not_before_time")), at(last, wr.at("not_after_time"))};
}

// _harvest_periods(), end dates only: crop -> the last day of its latest harvest row.
std::map<std::string, Date> load_harvest_end(std::istream& csv) {
    std::map<std::string, Date> harvest_end;
    std::vector<std::string> header, row;
    auto column = [&](const char* name) -> const std::string& {
        auto index = std::find(header.begin(), header.end(), name) - header.begin();
        if (index >= static_cast<std::ptrdiff_t>(row.size())) throw std::runtime_error(std::string("crop calendar: no ") + name);
        return row[index];
    };
    std::string line;
    while (std::getline(csv, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // Windows line endings
        if (line.empty()) continue;
        row = csv_fields(line);
        if (header.empty()) {
            header = row;
            continue;
        }
        // A post_harvest row describes storage, not a later standing-crop harvest.
        if (column("growth_stage") != "harvest") continue;
        Date end = config_date(column("end_date"));
        auto [known, added] = harvest_end.emplace(crop_key(column("crop")), end);
        if (!added) known->second = std::max(known->second, end);
    }
    return harvest_end;
}

// is_past_harvest(): strictly after the crop's harvest ends. A crop missing from the calendar uses the
// season end; a missing crop name stays unknown (false).
bool is_past_harvest(const json& crop, Date on, const Engine& engine) {
    if (!crop.is_string() || crop.get_ref<const std::string&>().empty()) return false;
    auto known = engine.harvest_end.find(crop_key(crop.get<std::string>()));
    if (known != engine.harvest_end.end()) return on > known->second;
    return on > config_date(engine.rules.at("window_rules").at("season_end").get<std::string>());
}

}  // namespace mel
