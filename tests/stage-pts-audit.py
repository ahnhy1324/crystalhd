#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Offline whole-pad PTS audit against caller-frozen expectations.

Expect schema=1, attachments=[{stage, token, buffers, first_pts, period_ns}].
Include every attachment. Source provenance and picture-to-pad mapping remain
caller obligations. This does not decode pixels or measure presentation/A/V.
"""
import argparse
import json
from pathlib import Path

ZERO = ("unknown_pts", "unknown_duration", "invalid_duration", "unqualified_buffers",
        "duplicate_events", "reverse_events", "off_grid_events", "gap_events",
        "gap_steps", "caps_changes", "flush_events", "seek_requests")
STAGES = {"decoder", "deinterlace", "output-pack", "output-scale", "sink"}
MAX = (1 << 64) - 2


def require(condition, message):
    if not condition:
        raise ValueError(message)


def fields(obj, wanted, where):
    require(type(obj) is dict, where + " must be an object")
    for key, value in wanted.items():
        require(key in obj and type(obj[key]) is type(value) and obj[key] == value,
                where + "." + key + " differs from " + repr(value))


def integer(value, minimum=0):
    require(type(value) is int and minimum <= value <= MAX, "invalid integer")
    return value


def indexed(items):
    require(type(items) is list and 1 <= len(items) <= 64, "invalid attachment list")
    result = {}
    for item in items:
        require(type(item) is dict and item.get("stage") in STAGES, "invalid stage")
        key = (item["stage"], integer(item.get("token")))
        require(key not in result, "duplicate stage/token")
        result[key] = item
    return result


def load(path):
    def pairs(items):
        obj = {}
        for key, value in items:
            require(key not in obj, "duplicate JSON key: " + key)
            obj[key] = value
        return obj
    def constant(value):
        raise ValueError("non-finite JSON value: " + value)
    return json.loads(Path(path).read_text(encoding="utf-8"),
                      object_pairs_hook=pairs, parse_constant=constant)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("expectation", type=Path)
    parser.add_argument("--native-only", action="store_true",
                        help="CPU controls: permit missing Player completion evidence")
    args = parser.parse_args()
    try:
        result = audit(load(args.trace), load(args.expectation),
                       require_player=not args.native_only)
    except (OSError, UnicodeError, ValueError) as error:
        result = {"schema": 1, "status": "FAIL", "errors": [str(error)]}
    print(json.dumps(result, indent=2, allow_nan=False))
    return 0 if result["status"] == "PASS" else 1


def audit(report, expected, *, require_player=True):
    result = {"schema": 1, "status": "FAIL", "attachments": [], "errors": [],
              "scope": "whole-attachment fixed-cadence PTS metadata only",
              "source_provenance": "caller-supplied expectations; not decoded here",
              "pixels_presentation_av": "NOT_MEASURED"}
    try:
        fields(expected, {"schema": 1}, "expectation")
        fields(report, {"schema": 1, "closed": True, "counters_saturated": False,
                        "buffer_list_ambiguity": False, "allocation_failed": False,
                        "attachment_limit_reached": False, "rejected_attachments": 0,
                        "configuration_rejections": 0}, "trace")
        wanted, observed = indexed(expected.get("attachments")), indexed(report.get("attachments"))
        require(wanted.keys() == observed.keys(), "attachment coverage differs")
        ids = [integer(item.get("id")) for item in observed.values()]
        require(len(set(ids)) == len(ids) and max(ids) < 64, "invalid attachment IDs")
        for key, goal in wanted.items():
            count, first = integer(goal.get("buffers"), 1), integer(goal.get("first_pts"))
            period = integer(goal.get("period_ns"), 1)
            require(period in (20_000_000, 40_000_000), "unsupported cadence")
            last = integer(first + (count - 1) * period)
            integer(last + period)
            where, owner = repr(key), observed[key]
            fields(owner, {"active": False, "flushing": False, "segment_valid": True,
                           "buffer_list_ambiguity": False, "buffer_lists": 0, "list_items": 0,
                           "list_missing_pts": 0, "missing_pts": 0, "flush_generation": 0,
                           "eos_count": 1, "stream_generation": 1, "segment_generation": 1,
                           "buffers": count}, where)
            fields(owner.get("pts_summary"), {"version": 1,
                   "scope": "whole-attachment-observation", "qualified": True,
                   "arithmetic_overflow": False, "counters_saturated": False,
                   "first_violation": None, "epochs": 1, "segment_events": 1,
                   "buffers": count, "covered": count, "period_ns": period,
                   "first_pts": first, "last_pts": last, **dict.fromkeys(ZERO, 0)}, where)
            result["attachments"].append({"stage": key[0], "token": key[1],
                                          "buffers": count, "first_pts": first,
                                          "last_pts": last, "period_ns": period})
        player = report.get("player")
        require(player is not None or not require_player, "missing Player completion evidence")
        if player is not None:
            fields(player, dict.fromkeys(("attachment_errors", "control_errors",
                                          "trigger_errors", "exit_code"), 0), "player")
            require(indexed(player.get("attachments")).keys() == observed.keys(),
                    "Player/native attachment coverage differs")
        result["status"] = "PASS"
    except (ValueError, TypeError, KeyError) as error:
        result["errors"].append(str(error))
    return result


if __name__ == "__main__":
    raise SystemExit(main())
