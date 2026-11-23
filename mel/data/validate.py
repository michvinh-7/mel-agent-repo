"""JSON Schema validation for insight / survey / schedule_handoff / response / no_survey / bottom_sheet. Owner: P1."""

import hashlib
import json
from functools import lru_cache

from jsonschema import Draft202012Validator
from referencing import Registry, Resource

from mel.config import SCHEMAS

KINDS = ("insight", "survey", "schedule_handoff", "response", "no_survey", "bottom_sheet")


@lru_cache
def _registry() -> Registry:
    reg = Registry()
    for p in SCHEMAS.glob("*.schema.json"):
        s = json.loads(p.read_text(encoding="utf-8"))
        res = Resource.from_contents(s)
        reg = reg.with_resource(s.get("$id", p.name), res).with_resource(p.name, res)
    return reg


@lru_cache
def _validator(kind: str) -> Draft202012Validator:
    if kind not in KINDS:
        raise ValueError(f"unknown kind {kind!r}; expected one of {KINDS}")
    schema = json.loads((SCHEMAS / f"{kind}.schema.json").read_text(encoding="utf-8"))
    return Draft202012Validator(schema, registry=_registry())


_SEEN: dict[tuple[str, bytes], tuple[str, ...]] = {}
_SEEN_MAX = 50_000  # the same bound as the scheduler gate's cache (mel/schedule/broadcast.py)


def errors(kind: str, obj: dict) -> list[str]:
    """Schema errors of `obj`, remembered by content. One run checks the same content more than once (a draft in
    the Verifier and again for its self-evaluation, a finished survey when it is written and again at the
    scheduler's gate, an insight in the decision and again in the junk report), and the validation is the slow
    part of a run. The result depends only on the schema and the content, so a changed object is checked again."""
    # repr, not json.dumps: it keeps what the validator tells apart (a tuple is not an array, the key 1 is not "1").
    # A 16-byte digest, not the text: a full-pack run remembers about 25,000 objects.
    key = (kind, hashlib.blake2b(repr(obj).encode("utf-8", "backslashreplace"), digest_size=16).digest())
    found = _SEEN.get(key)
    if found is None:
        found = tuple(
            f"{'/'.join(map(str, e.path)) or '<root>'}: {e.message}" for e in _validator(kind).iter_errors(obj)
        )
        if len(_SEEN) >= _SEEN_MAX:
            _SEEN.clear()  # ponytail: forgets everything when full; an LRU if a long-lived API ever churns through it
        _SEEN[key] = found
    return list(found)


def failed_fields(kind: str, obj: dict) -> list[str]:
    """Every field that fails the schema, sorted, as dotted paths (``query_summary``,
    ``verification.needed``). A missing required field is named, not reported as the parent."""
    fields: set[str] = set()
    for e in _validator(kind).iter_errors(obj):
        path = [str(p) for p in e.path]
        if e.validator == "required" and isinstance(e.instance, dict):
            fields.update(".".join(path + [k]) for k in e.validator_value if k not in e.instance)
        else:
            fields.add(".".join(path) or "<root>")
    return sorted(fields)


def is_valid(kind: str, obj: dict) -> bool:
    return not errors(kind, obj)


def why_junk(insight: dict) -> list[str]:
    """Plain reasons an insight should get no survey. Empty list means it looks clean.

    Two sources of 'junk': the practice label ``quality_flag == "junk"``, and any way the
    row fails the insight schema (junk rows are broken on purpose and may miss required
    fields or carry out-of-range values). P1 reports these so a batch run can say *why* a
    row was dropped instead of silently skipping it.
    """
    reasons: list[str] = []
    if insight.get("quality_flag") == "junk":
        reasons.append("labelled junk (quality_flag)")
    reasons.extend(f"schema: {e}" for e in errors("insight", insight))
    return reasons


def validate(kind: str, obj: dict) -> dict:
    errs = errors(kind, obj)
    if errs:
        raise ValueError(f"{kind} invalid:\n  " + "\n  ".join(errs[:10]))
    return obj
