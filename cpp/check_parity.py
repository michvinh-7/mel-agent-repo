"""Parity check: the C++ port (cpp/build/mel_decide) must decide exactly like mel.decision.decide.

cmake -S cpp -B cpp/build && cmake --build cpp/build
.venv/bin/python cpp/check_parity.py
"""

import copy
import itertools
import json
import os
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
os.environ["MEL_TIMING_BASIS"] = "brief"  # the only basis the port covers

from mel.config import DATA, PACK_DATA, RULES_DIR  # noqa: E402
from mel.data import load_scenario  # noqa: E402
from mel.decision import decide  # noqa: E402

BINARY = ROOT / "cpp" / "build" / "mel_decide"


def changed(base: dict, path: str, value) -> dict:
    """A copy of `base` with one field replaced; "asset.name" reaches into a nested object."""
    out = copy.deepcopy(base)
    *parents, last = path.split(".")
    target = out
    for key in parents:
        target = target[key]
    target[last] = value
    return out


def edge_cases(base: dict) -> list[dict]:
    late = "2027-03-17T09:00:00+05:30"
    one_field = [
        # tests/test_decision.py: every rule, broken input, storage and livestock
        ("quality_flag", "junk"),
        ("severity", "critical"),
        ("resolution_type", "generic_information"),
        ("resolution_type", "needs_more_context"),
        ("verification", {"needed": False, "timing": "none", "method": "none"}),
        ("created_at", "2027-03-31T09:00:00+05:30"),
        ("created_at", "2027-03-22T09:00:00+05:30"),
        ("created_at", "2027-02-30T09:00:00+05:30"),
        ("created_at", "2026-13-01T09:00:00+05:30"),
        ("created_at", "2026-12-01T25:00:00+05:30"),
        ("farmer_id", None),
        ("insight_id", 123),
        ("verification", {}),
        ("action_window_days", -1),
        # past what the tests cover: timestamps
        ("created_at", "2026-12-01T09:00:60+05:30"),
        ("created_at", "2026-12-01T09:00:00.123456789+05:30"),
        ("created_at", "2026-12-01T09:00:00+05:30\n"),
        ("created_at", "2026-12-01T09:00:00Z"),
        ("created_at", "2024-02-29T09:00:00+05:30"),
        ("created_at", "0000-01-01T09:00:00+05:30"),
        # numbers: 3.0 is an integer to JSON Schema, true is not a number
        ("action_window_days", 3.0),
        ("action_window_days", 3.5),
        ("action_window_days", True),
        ("action_window_days", 180),
        ("action_window_days", 181),
        ("action_window_days", "3"),
        # lengths are counted in characters, not bytes
        ("insight_id", "é" * 64),
        ("insight_id", "é" * 65),
        ("insight_id", ""),
        ("farmer_id", "किसान" * 30),
        ("query_summary", "x" * 400),
        ("query_summary", "x" * 401),
        # patterns, enums, types
        ("conversation_language", "hi-IN\n"),
        ("conversation_language", "hi-IN\nx"),
        ("conversation_language", "hindi"),
        ("location.country", "in"),
        ("location", "Bihar"),
        ("asset.name", "Wheat"),
        ("asset.name", "wheat (late)"),
        ("asset.name", "wheat\n"),
        ("asset.name", ""),
        ("asset", None),
        ("severity", None),
        ("severity", "urgent"),
        ("concern", None),
        ("behavioral_intent", None),
        ("verification.needed", 1),
        ("verification.timing", None),
        ("quality_flag", "clean"),
        ("quality_flag", "weird"),
        ("unexpected_field", 1),
    ]
    cases = [changed(base, path, value) for path, value in one_field]
    cases.append({k: v for k, v in base.items() if k != "severity"})  # a required field is missing
    for asset, concern in [
        ({"name": "potato", "category": "crop", "growth_stage": "post_harvest"}, "pest_disease"),
        ({"name": "potato", "category": "crop", "growth_stage": None}, "storage_post_harvest"),
        ({"name": "cow", "category": "livestock", "growth_stage": None}, "livestock_health"),
        ({"name": "potato", "category": "crop", "growth_stage": "vegetative"}, "pest_disease"),
    ]:
        cases.append({**base, "created_at": late, "asset": asset, "concern": concern})
    # Window arithmetic and the harvest check: every timing x action window x date x crop.
    timings = ["none", "same_day", "next_day", "two_to_five_days", "three_to_ten_days", "seven_to_fourteen_days"]
    days = ["2026-11-15", "2027-02-20", "2027-03-10", "2027-03-25", "2027-03-28", "2027-03-29", "2027-03-31"]
    crops = ["wheat", "potato", "chickpeas", "peas ", "dragonfruit", None]
    for timing, window, day, crop in itertools.product(timings + ["seasonal"], [None, 0, 1, 5, 30, 180], days, crops):
        case = changed(base, "verification.timing", timing)
        case.update(action_window_days=window, created_at=f"{day}T18:45:00+05:30")
        case["asset"] = {"name": crop, "category": "crop", "growth_stage": "vegetative"}
        cases.append(case)
    return cases


def python_decision(insight: dict) -> dict:
    d = decide(insight)
    return {
        "survey": d.survey,
        "rule": d.rule,
        "reason_code": d.reason_code,
        "send_window": d.send_window,
        "no_survey": None if d.survey else d.no_survey_record(insight),
    }


def main() -> None:
    if not BINARY.exists():
        sys.exit(f"{BINARY} is missing: cmake -S cpp -B cpp/build && cmake --build cpp/build")
    lines = [x for x in (PACK_DATA / "insights.jsonl").read_text(encoding="utf-8").split("\n") if x.strip()]
    golden = (PACK_DATA / "golden_set.jsonl").read_text(encoding="utf-8").split("\n")
    lines += [json.dumps(json.loads(x)["insight"], ensure_ascii=False) for x in golden if x.strip()]
    lines += [json.dumps(x, ensure_ascii=False) for x in edge_cases(load_scenario("1-mustard-aphid-spray")["insight"])]
    insights = [json.loads(x) for x in lines]
    command = [str(BINARY), str(RULES_DIR / "rules.json"), str(DATA / "crop_calendar_bihar_rabi.csv"), "/dev/stdin"]

    differ = 0
    for policy in ("human_call", "survey"):
        os.environ["MEL_CRITICAL_POLICY"] = policy  # read by decide() and inherited by the binary
        run = subprocess.run(command, input="\n".join(lines) + "\n", capture_output=True, encoding="utf-8", check=True)
        cpp = [json.loads(x) for x in run.stdout.split("\n") if x]
        assert len(cpp) == len(insights), f"{len(cpp)} decisions for {len(insights)} insights"
        rules = Counter()
        for line, insight, got in zip(lines, insights, cpp, strict=True):
            want = python_decision(insight)
            rules[want["rule"]] += 1
            got.pop("insight_id")
            if got != want:
                differ += 1
                if differ <= 5:
                    print(f"DIFFERENT ({policy})\n  insight {line[:300]}\n  python  {want}\n  c++     {got}")
        print(f"{policy}: {len(insights)} insights, rules {dict(sorted(rules.items()))}")
    if differ:
        sys.exit(f"FAIL: {differ} decisions differ")
    print("OK: C++ and Python agree on every decision")


if __name__ == "__main__":
    main()
