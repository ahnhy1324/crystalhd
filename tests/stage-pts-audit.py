#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Offline whole-pad PTS audit against caller-frozen expectations.

Expect schema=1, attachments=[{stage, token, buffers, first_pts, period_ns}].
Expectation schema=2 supplies a bounded independent source/segment/CAPS goal
for every epoch. Neither mode derives expectations from observed detail/ledgers.
Include every attachment. Source provenance and picture-to-pad mapping remain
caller obligations. This does not decode pixels or measure presentation/A/V.
"""
import argparse
import heapq
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
    if type(expected) is dict and type(expected.get("schema")) is int and expected["schema"] == 2:
        return audit_epochs(report, expected, require_player=require_player)
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


EPOCH_ZERO = ("unqualified_buffers", "unknown_pts", "unknown_duration", "invalid_duration",
              "duplicate_events", "reverse_events", "off_grid_events", "gap_events", "gap_steps")
CAPS_KEYS = ("format", "width", "height", "fps_n", "fps_d", "interlace_mode",
             "field_order", "raw_video", "video_info_valid")
SEGMENT_KEYS = ("format", "rate", "applied_rate", "start", "stop", "time", "base", "offset")
EPOCH_LIMIT = 16


def segment_signature(value):
    require(type(value) is dict and set(SEGMENT_KEYS) <= value.keys(), "incomplete segment signature")
    fields(value, {"format": 3}, "segment")
    for key in ("rate", "applied_rate"):
        require(type(value[key]) in (int, float) and value[key] == 1, "unsupported segment rate")
    result = {key: integer(value[key]) for key in ("start", "time", "base", "offset")}
    result.update(format=3, rate=1, applied_rate=1)
    result["stop"] = None if value["stop"] is None else integer(value["stop"])
    require(result["stop"] is None or result["stop"] > result["start"], "invalid segment stop")
    return result


def caps_signature(value, period):
    require(type(value) is dict and set(CAPS_KEYS) <= value.keys(), "incomplete CAPS signature")
    fields(value, {"raw_video": True, "video_info_valid": True}, "CAPS")
    require(type(value["format"]) is str and 0 < len(value["format"]) <= 64 and
            value["format"] not in ("UNKNOWN", "ENCODED"), "unknown raw video format")
    result = {"format": value["format"], "raw_video": True, "video_info_valid": True}
    for name in ("width", "height", "fps_n", "fps_d"):
        result[name] = integer(value[name], 1)
        require(result[name] <= (1 << 31) - 1, "CAPS integer outside native range")
    for name, maximum in (("interlace_mode", 4), ("field_order", 2)):
        result[name] = integer(value[name])
        require(result[name] <= maximum, "invalid CAPS mode")
    numerator = integer(1_000_000_000 * result["fps_d"])
    require(numerator % result["fps_n"] == 0 and numerator // result["fps_n"] == period,
            "CAPS cadence differs from independent period")
    return result


def expected_epochs(goal):
    """Validate caller-frozen source expectations without consulting the trace."""
    rows = goal.get("epochs")
    require(type(rows) is list and 1 <= len(rows) <= EPOCH_LIMIT, "invalid expected epoch coverage")
    requests, failures = integer(goal.get("seek_requests")), integer(goal.get("seek_failures"))
    require(failures <= requests, "seek failures exceed requests")
    result, total, eos, first_caps = [], 0, 0, None
    for index, row in enumerate(rows):
        require(type(row) is dict, "expected epoch must be an object")
        stream = integer(row.get("stream_generation"), 1)
        segment = integer(row.get("segment_generation"), 1)
        flush = integer(row.get("flush_generation"))
        count, first = integer(row.get("buffers"), 1), integer(row.get("first_pts"))
        period = integer(row.get("period_ns"), 1)
        require(period in (20_000_000, 40_000_000), "unsupported epoch cadence")
        last = integer(first + integer((count - 1) * period))
        end_pts = integer(last + period)
        signature = segment_signature(row.get("segment"))
        caps = caps_signature(row.get("caps"), period)
        require(first >= signature["start"], "epoch PTS precedes segment start")
        require(signature["stop"] is None or end_pts <= signature["stop"], "epoch exceeds finite segment")
        running_first = integer(signature["base"] + first - signature["start"] - signature["offset"])
        running_last = integer(running_first + last - first)
        integer(running_last + period)
        if first_caps is None:
            first_caps = caps
        require(caps == first_caps, "changed CAPS are outside epoch qualification")
        reason = row.get("end_reason")
        require(reason in ("segment", "flush-start", "stream-start", "eos"), "unsupported epoch close reason")
        if index == 0:
            require((stream, segment, flush) == (1, 1, 0), "invalid initial epoch identity")
        else:
            previous = result[-1]
            stream_delta = stream - previous["stream_generation"]
            flush_delta = flush - previous["flush_generation"]
            require(segment == index + 1 and stream_delta in (0, 1) and flush_delta in (0, 1),
                    "nonconsecutive epoch generations")
            if previous["end_reason"] == "segment":
                require(stream_delta == flush_delta == 0, "segment close hides another boundary")
            elif previous["end_reason"] == "flush-start":
                require(flush_delta == 1, "flush close without paired flush generation")
            elif previous["end_reason"] == "stream-start":
                require(stream_delta == 1, "stream close without fresh stream generation")
            else:
                require(flush_delta == 1, "EOS restart requires a paired flush")
        seqnum = integer(row["seqnum"]) if "seqnum" in row else None
        require(seqnum is None or seqnum <= (1 << 32) - 1, "segment seqnum outside uint32")
        result.append({"index": index, "stream_generation": stream, "segment_generation": segment,
                       "flush_generation": flush, "buffers": count, "covered": count,
                       "first_ordinal": integer(total + 1), "last_ordinal": integer(total + count),
                       "first_pts": first, "last_pts": last, "period_ns": period,
                       "first_running_time": running_first, "last_running_time": running_last,
                       "end_reason": reason, "segment": signature, "caps": caps, "seqnum": seqnum})
        total = integer(total + count)
        eos += reason == "eos"
    require(result[-1]["end_reason"] == "eos", "final epoch must close EOS")
    require(requests - failures <= result[-1]["flush_generation"], "successful seek lacks paired flush")
    return result, total, eos, requests, failures


def event_capacity(jobs, reserved):
    """Necessary unit-event feasibility, with bulk EDF over interval endpoints.

    A job is (inclusive release, inclusive deadline, count). Reserved boundary
    orders cannot also hold buffers/context/control records. Each iteration
    completes a job or reaches a release/reservation/deadline; uint64 counts
    are never expanded into individual events. This is not a causal proof.
    """
    tasks = []
    for number, (release, deadline, count) in enumerate(jobs):
        count = integer(count)
        if not count:
            continue
        release, deadline = integer(release, 1), integer(deadline, 1)
        require(release <= deadline, "event capacity: empty required interval")
        tasks.append((release, deadline, number, count))
    tasks.sort()
    blocked = sorted({integer(order, 1) for order in reserved})
    ready, index, blocked_index, cursor = [], 0, 0, 1
    while ready or index < len(tasks):
        if not ready:
            cursor = max(cursor, tasks[index][0])
        while index < len(tasks) and tasks[index][0] <= cursor:
            release, deadline, number, count = tasks[index]
            heapq.heappush(ready, (deadline, number, count))
            index += 1
        while blocked_index < len(blocked) and blocked[blocked_index] < cursor:
            blocked_index += 1
        if blocked_index < len(blocked) and blocked[blocked_index] == cursor:
            cursor += 1
            blocked_index += 1
            continue
        deadline, number, count = heapq.heappop(ready)
        require(deadline >= cursor, "event capacity: overlapping demands exceed available slots")
        end = deadline + 1
        if index < len(tasks):
            end = min(end, tasks[index][0])
        if blocked_index < len(blocked):
            end = min(end, blocked[blocked_index])
        amount = min(count, end - cursor)
        require(amount > 0, "event capacity: no available event slot")
        cursor += amount
        if amount < count:
            heapq.heappush(ready, (deadline, number, count - amount))


def audit_epochs(report, expected, *, require_player=True):
    result = {"schema": 2, "status": "FAIL", "attachments": [], "errors": [],
              "scope": "bounded-segment PTS metadata only",
              "source_provenance": "caller-supplied expectations; not decoded here",
              "pixels_presentation_av": "NOT_MEASURED"}
    try:
        fields(expected, {"schema": 2}, "expectation")
        fields(report, {"schema": 1, "closed": True, "counters_saturated": False,
                        "buffer_list_ambiguity": False, "allocation_failed": False,
                        "attachment_limit_reached": False, "rejected_attachments": 0,
                        "configuration_rejections": 0}, "trace")
        observed_order = integer(report.get("observed"), 1)
        wanted, observed = indexed(expected.get("attachments")), indexed(report.get("attachments"))
        require(wanted.keys() == observed.keys(), "attachment coverage differs")
        ids = [integer(item.get("id")) for item in observed.values()]
        require(ids == list(range(len(ids))), "invalid ordered attachment IDs")
        jobs, boundary_orders, helper_requests, helper_failures = [], {}, 0, 0
        for key, goal in wanted.items():
            rows, count, eos, requests, failures = expected_epochs(goal)
            helper_requests = max(helper_requests, requests)
            helper_failures = max(helper_failures, failures)
            terminal, where, owner = rows[-1], repr(key), observed[key]
            fields(owner, {"active": False, "flushing": False, "segment_valid": True,
                           "buffer_list_ambiguity": False, "buffer_lists": 0, "list_items": 0,
                           "list_missing_pts": 0, "missing_pts": 0, "buffers": count,
                           "flush_generation": terminal["flush_generation"], "eos_count": eos,
                           "stream_generation": terminal["stream_generation"],
                           "segment_generation": len(rows)}, where)
            ledger = owner.get("pts_epochs")
            fields(ledger, {"version": 1, "scope": "bounded-segment-pts", "limit": EPOCH_LIMIT,
                            "qualified": True, "overflow": False, "first_violation": None,
                            "buffers": count, "unassigned_buffers": 0,
                            "stream_events": terminal["stream_generation"], "segment_events": len(rows),
                            "flush_starts": terminal["flush_generation"],
                            "flush_stops": terminal["flush_generation"], "seek_requests": requests,
                            "seek_failures": failures, "pending_seek": False}, where + ".pts_epochs")
            actual = ledger.get("rows")
            require(type(actual) is list and len(actual) == len(rows), "epoch coverage differs")
            previous_end = None
            for index, (row, goal_row) in enumerate(zip(actual, rows)):
                row_where = where + ".epoch[" + str(index) + "]"
                compared = {name: value for name, value in goal_row.items()
                            if name not in ("segment", "caps", "seqnum")}
                fields(row, {**compared, **dict.fromkeys(EPOCH_ZERO, 0), "qualified": True,
                             "arithmetic_overflow": False, "counters_saturated": False,
                             "first_violation": None}, row_where)
                seqnum = integer(row.get("seqnum"))
                require(seqnum <= (1 << 32) - 1, "segment seqnum outside uint32")
                require(goal_row["seqnum"] is None or seqnum == goal_row["seqnum"],
                        "independent segment seqnum differs")
                require(segment_signature(row.get("segment")) == goal_row["segment"],
                        "independent segment signature differs")
                require(caps_signature(row.get("caps"), goal_row["period_ns"]) == goal_row["caps"],
                        "independent CAPS signature differs")
                start, end = integer(row.get("start_order"), 1), integer(row.get("end_order"), 1)
                require(end < observed_order and end >= integer(start + goal_row["buffers"] + 1),
                        "invalid epoch event-order span")
                jobs.append((start + 1, end - 1, goal_row["buffers"]))
                if previous_end is not None:
                    if rows[index - 1]["end_reason"] == "segment":
                        require(start == previous_end, "segment boundary order differs")
                    else:
                        require(start > previous_end, "overlapping or missing boundary order")
                        previous = rows[index - 1]
                        stream_delta = goal_row["stream_generation"] - previous["stream_generation"]
                        flush_delta = goal_row["flush_generation"] - previous["flush_generation"]
                        if previous["end_reason"] == "flush-start":
                            metadata = 1 + 2 * stream_delta
                        elif previous["end_reason"] == "stream-start":
                            metadata = 1 + 2 * flush_delta
                        else:
                            metadata = 2 + 2 * stream_delta
                        jobs.append((previous_end + 1, start - 1, metadata))
                else:
                    jobs.append((1, start - 1, 3))
                for order in (start, end):
                    if order in boundary_orders:
                        require(boundary_orders[order] == key and order == start == previous_end and
                                rows[index - 1]["end_reason"] == "segment",
                                "global boundary order reused by another event")
                    boundary_orders[order] = key
                previous_end = end
            result["attachments"].append({"stage": key[0], "token": key[1], "buffers": count,
                                          "epochs": [{name: row[name] for name in
                                                      ("index", "buffers", "first_pts", "last_pts",
                                                       "period_ns", "end_reason")} for row in rows]})
        fields(report, {"seek_requests": helper_requests, "seek_failures": helper_failures}, "helper")
        jobs.append((1, observed_order - 1, integer(helper_requests + helper_failures)))
        event_capacity(jobs, set(boundary_orders) | {observed_order})
        player = report.get("player")
        require(player is not None or not require_player, "missing Player completion evidence")
        if player is not None:
            fields(player, dict.fromkeys(("attachment_errors", "control_errors", "trigger_errors", "exit_code"), 0),
                   "player")
            require(indexed(player.get("attachments")).keys() == observed.keys(),
                    "Player/native attachment coverage differs")
        result["status"] = "PASS"
    except (ValueError, TypeError, KeyError, OverflowError) as error:
        result["errors"].append(str(error))
    return result


if __name__ == "__main__":
    raise SystemExit(main())
