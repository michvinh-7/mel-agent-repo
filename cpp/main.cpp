// mel_decide: one insight per line (JSONL) in, one decision per line on stdout.
//   mel_decide config/rules/rules.json data/mock/crop_calendar_bihar_rabi.csv data/mock/insights.jsonl
// To read a pipe, pass /dev/stdin as the insights file. std::cin is not used on purpose: libc++ reads it one
// locked character at a time, which made the whole program as slow as the Python.
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "engine.hpp"

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: mel_decide RULES_JSON CROP_CALENDAR_CSV INSIGHTS_JSONL\n";
        return 2;
    }
    try {
        const mel::Engine engine = mel::load_engine(argv[1], argv[2]);
        std::ifstream insights(argv[3]);
        if (!insights) throw std::runtime_error(std::string("cannot open ") + argv[3]);
        std::string line;
        while (std::getline(insights, line)) {
            if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
            // A line that is not JSON comes back "discarded" instead of throwing; decide() then answers R1.
            const mel::json insight = mel::json::parse(line, nullptr, false);
            const mel::Decision decision = mel::decide(insight, engine);

            mel::json out;
            out["insight_id"] = insight.is_object() ? insight.value("insight_id", mel::json()) : mel::json();
            out["survey"] = decision.survey;
            out["rule"] = decision.rule;
            if (decision.survey) {
                out["reason_code"] = nullptr;
                out["send_window"] = {{"not_before", decision.send_window->not_before},
                                      {"not_after", decision.send_window->not_after}};
                out["no_survey"] = nullptr;
            } else {
                out["reason_code"] = decision.reason_code;
                out["send_window"] = nullptr;
                out["no_survey"] = mel::no_survey_record(decision, insight);
            }
            std::cout << out.dump() << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "mel_decide: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
