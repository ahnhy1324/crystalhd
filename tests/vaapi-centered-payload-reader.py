#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Offline checker for private centered NV12 snapshots and OWN trace bindings.

Only explicitly named, frozen, trusted diagnostic artifacts are read. No media,
device, pointer or DMA access is performed. This checks consistency of observed
CPU bytes and trace identities, not authenticity, stock acceptance, actual media
PTS, external GPU-reader completion, or scanout.
"""

import argparse
import json
import os
from pathlib import Path
import stat
import sys


SCHEMA = "crystalhd-centered-payload-v1"
TRACE_SCHEMA = "crystalhd-vaapi-trace-v4"
SMALL_WIDTH, SMALL_HEIGHT = 1280, 720
CANVAS_WIDTH, VISIBLE_HEIGHT = 1920, 1080
CANVAS_HEIGHTS = (1080, 1088)
ROI = [320, 180, 1280, 720]
SLOTS_REQUIRED = 2
BINDING_KEYS = ("context", "generation", "token", "decode_identity",
                "submission_ordinal", "surface", "owner")
GEOMETRY_KEYS = ("surface_serial", "storage_epoch", "width", "height",
                 "pitch", "plane_address", "fourcc")
STAGE_FLAGS = ("compose_copied", "compose_committed", "public_copied",
               "public_committed", "materialized")
MAX_MANIFEST_BYTES = 128 * 1024
MAX_TRACE_BYTES = 64 * 1024 * 1024
MAX_CANVAS_BYTES = CANVAS_WIDTH * max(CANVAS_HEIGHTS) * 3 // 2


def require(condition, message):
    if not condition:
        raise ValueError(message)


def integer(value, label, minimum=0):
    require(type(value) is int and minimum <= value < 2 ** 64,
            label + " must be an integer in range")
    return value


def object_value(value, label):
    require(type(value) is dict, label + " must be an object")
    return value


def strict_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON key: " + key)
        result[key] = value
    return result


def filename(value):
    require(type(value) is str and value not in ("", ".", "..") and
            "/" not in value and "\\" not in value and "\x00" not in value,
            "payload path must be one relative filename")
    return value


def geometry(value, label, small=False):
    value = object_value(value, label)
    require(all(key in value for key in GEOMETRY_KEYS),
            label + " is missing geometry fields")
    for key in ("surface_serial", "storage_epoch", "width", "height"):
        integer(value[key], label + "." + key, 1)
    width = SMALL_WIDTH if small else CANVAS_WIDTH
    heights = (SMALL_HEIGHT,) if small else CANVAS_HEIGHTS
    require(value["fourcc"] == "NV12" and value["width"] == width and
            value["height"] in heights,
            label + " has unsupported physical NV12 geometry")
    for key in ("pitch", "plane_address"):
        pair = value[key]
        require(type(pair) is list and len(pair) == 2,
                label + "." + key + " must have two values")
        for item in pair:
            integer(item, label + "." + key, width if key == "pitch" else 1)
    extents = []
    for plane, rows in enumerate((value["height"], value["height"] // 2)):
        begin = value["plane_address"][plane]
        end = begin + (rows - 1) * value["pitch"][plane] + width
        require(end < 2 ** 64, label + " plane address extent overflows")
        extents.append((begin, end))
    require(extents[0][1] <= extents[1][0] or extents[1][1] <= extents[0][0],
            label + " physical NV12 planes overlap")
    # Extra allocation metadata is descriptive. Serial/epoch identify our
    # storage view; neither addresses nor this tuple prove global DMA aliasing.
    return tuple(tuple(value[key]) if type(value[key]) is list else value[key]
                 for key in GEOMETRY_KEYS)


def binding_key(binding):
    return tuple(binding[key] for key in BINDING_KEYS)


def trace_index(rows, pid, instance):
    require(type(rows) is list and rows, "trace rows are required")
    selected = []
    for row in rows:
        row = object_value(row, "trace row")
        require(row.get("schema") == TRACE_SCHEMA, "unsupported trace schema")
        if row.get("pid") == pid and row.get("driver_instance") == instance:
            selected.append(row)
    require(selected, "manifest session has no trace rows")
    previous_ns = 0
    for seq, row in enumerate(selected, 1):
        for field in ("pid", "driver_instance", "session"):
            integer(row.get(field), "trace " + field, 1)
        require(row.get("session") == instance, "trace session/instance mismatch")
        require(type(row.get("seq")) is int and row["seq"] == seq,
                "selected trace sequence is incomplete or duplicated")
        now = integer(row.get("ts_monotonic_ns"), "trace timestamp", 1)
        require(now >= previous_ns, "trace timestamp regressed")
        previous_ns = now
        require(type(row.get("event")) is str, "trace event must be a string")
        for key in BINDING_KEYS + ("operation",):
            integer(row.get(key), "trace " + key)
        require(type(row.get("outcome")) is int, "trace outcome must be an integer")
    destroyed = [row for row in selected if row["event"] == "session_destroy"]
    require(not destroyed or (len(destroyed) == 1 and destroyed[0] is selected[-1]),
            "session_destroy must be unique and terminal when present")
    return selected


def matching(rows, event, key):
    return [row for row in rows
            if row["event"] == event and binding_key(row) == key]


def check_call(rows, exit_row, event, key, begin, end):
    """A successful API exit must retain its original admitted binding."""
    require(begin < exit_row["seq"] < end,
            event + " crosses binding_end or decoder lifetime")
    operation = integer(exit_row["operation"], event + " operation", 1)
    enters = [row for row in rows
              if row["event"] == event.replace("_exit", "_enter") and
              row["operation"] == operation]
    exits = [row for row in rows
             if row["event"] == event and row["operation"] == operation]
    require(len(enters) == 1 and len(exits) == 1 and
            binding_key(enters[0]) == key and
            begin < enters[0]["seq"] < exit_row["seq"],
            event + " has missing, duplicated, or swapped API binding")


def join_binding(rows, binding, destroy):
    key = binding_key(binding)
    submits = matching(rows, "submit_bind", key)
    require(len(submits) == 1 and submits[0]["outcome"] == 0,
            "binding needs one successful submit_bind")
    submit = submits[0]
    # Do not accept a second original merely because one public surface or
    # owner differs. Token, decode identity and ordinal each identify one
    # source submission within this decoder lifetime.
    lifetime = key[:2]
    lifetime_submits = [row for row in rows if row["event"] == "submit_bind" and
                        (row["context"], row["generation"]) == lifetime]
    for identity in ("token", "decode_identity", "submission_ordinal"):
        require(sum(row[identity] == binding[identity] for row in lifetime_submits) == 1,
                "ambiguous or doubled original " + identity)
    require(submit["seq"] < destroy["seq"], "submission follows decoder retirement")
    limits = [destroy["seq"]]
    for row in rows:
        if row["seq"] <= submit["seq"]:
            continue
        if row["event"] == "binding_end" and (
                binding_key(row) == key or row["surface"] == binding["surface"] or
                row["owner"] == binding["owner"]):
            limits.append(row["seq"])
        if (row["event"] == "context_reset" and
                (row["context"], row["generation"]) == lifetime):
            limits.append(row["seq"])
        if row["event"] == "submit_bind" and (
                row["surface"] == binding["surface"] or
                row["owner"] == binding["owner"]):
            limits.append(row["seq"])
    end = min(limits)
    materialized = matching(rows, "materialized", key)
    require(len(materialized) == 1 and materialized[0]["outcome"] == 0,
            "binding needs one successful public materialized observation")
    materialized = materialized[0]
    require(submit["seq"] < materialized["seq"] < end,
            "materialized crosses binding_end or decoder lifetime")
    syncs = [row for row in matching(rows, "sync_exit", key) if row["outcome"] == 0]
    require(syncs, "binding has no successful matching sync_exit")
    for row in syncs:
        check_call(rows, row, "sync_exit", key, submit["seq"], end)
        require(row["seq"] > materialized["seq"],
                "successful sync_exit precedes public materialization")
    exports = [row for row in matching(rows, "export_exit", key) if row["outcome"] == 0]
    for row in exports:
        check_call(rows, row, "export_exit", key, submit["seq"], end)
        require(row["seq"] > materialized["seq"],
                "successful export_exit precedes public materialization")
    return {"submit_seq": submit["seq"], "materialized_seq": materialized["seq"],
            "successful_syncs": len(syncs), "successful_exports": len(exports),
            "binding_end_seq": end if end != destroy["seq"] else None}


def compare_canvas(slot_number, stage, small, canvas, height):
    """Compute centered Y/UV rows independently, including coded bottom rows."""
    x, y, width, picture_height = ROI
    small_offset, canvas_offset = 0, 0
    compared = {}
    for plane, divisor, black in (("Y", 1, 16), ("UV", 2, 128)):
        source_rows = picture_height // divisor
        top = y // divisor
        border = bytes((black,)) * CANVAS_WIDTH
        left, right = border[:x], border[x + width:]
        for row in range(height // divisor):
            if top <= row < top + source_rows:
                begin = small_offset + (row - top) * SMALL_WIDTH
                expected = left + small[begin:begin + SMALL_WIDTH] + right
            else:
                expected = border
            begin = canvas_offset + row * CANVAS_WIDTH
            actual = canvas[begin:begin + CANVAS_WIDTH]
            if actual != expected:
                column = next(index for index, pair in enumerate(zip(expected, actual))
                              if pair[0] != pair[1])
                component = plane if plane == "Y" else ("U" if column % 2 == 0 else "V")
                raise ValueError("slot %d %s %s mismatch at row %d byte %d" %
                                 (slot_number, stage, component, row, column))
        compared[plane] = CANVAS_WIDTH * (height // divisor)
        small_offset += SMALL_WIDTH * source_rows
        canvas_offset += CANVAS_WIDTH * (height // divisor)
    return compared


def check_document(doc, blobs, trace_rows):
    """Check an already frozen manifest/blob/trace set; no I/O occurs here."""
    doc = object_value(doc, "manifest")
    require(doc.get("schema") == SCHEMA, "unsupported payload schema")
    pid = integer(doc.get("pid"), "pid", 1)
    instance = integer(doc.get("driver_instance"), "driver_instance", 1)
    require(type(doc.get("slots_required")) is int and
            doc["slots_required"] == SLOTS_REQUIRED,
            "manifest slots_required must be two")
    require(doc.get("media_pts_observed") is False,
            "actual media PTS is not observed by this protocol")
    slots = doc.get("slots")
    require(type(slots) is list and len(slots) == SLOTS_REQUIRED,
            "exactly two selected payload slots are required")
    require(type(blobs) is dict, "payload blobs must be a dictionary")
    publication = object_value(doc.get("publication"), "publication")
    require(publication.get("kind") == "selected-decoder-context-retirement",
            "publication must name selected decoder-context retirement")
    require(publication.get("after_driver_unlock") is True and
            publication.get("all_slots_frozen") is True,
            "publication requires unlocked caller and frozen slots")
    context = integer(publication.get("context"), "publication context", 1)
    generation = integer(publication.get("generation"), "publication generation", 1)
    require(type(publication.get("status")) is int and publication["status"] == 0,
            "publication retirement must be successful")
    rows = trace_index(trace_rows, pid, instance)
    retirements = [row for row in rows if row["event"] == "context_destroy" and
                   (row["context"], row["generation"]) == (context, generation)]
    require(len(retirements) == 1 and retirements[0]["outcome"] == 0,
            "selected lifetime needs one successful raw context_destroy")
    destroy = retirements[0]
    require(all(destroy[key] == 0 for key in BINDING_KEYS[2:] + ("operation",)),
            "context_destroy has non-context binding fields")
    require(not any(row["event"] == "context_reset" and
                    (row["context"], row["generation"]) == (context, generation) and
                    row["seq"] < destroy["seq"] for row in rows),
            "selected decoder lifetime was reset before retirement")
    files, seen_tokens, seen_identities, seen_ordinals = set(), set(), set(), set()
    observations = []
    compared = {stage: {plane: 0 for plane in ("Y", "UV")}
                for stage in ("composed", "public")}
    previous_materialized = 0
    for number, slot in enumerate(slots):
        slot = object_value(slot, "slot")
        for flag in STAGE_FLAGS:
            require(slot.get(flag) is True, "slot " + flag + " must be true")
        require(type(slot.get("direct_public")) is bool,
                "slot direct_public must be a boolean")
        require(slot.get("limited_range") is True, "only limited-range NV12 is supported")
        for field, expected in (("roi", ROI), ("visible", [CANVAS_WIDTH, VISIBLE_HEIGHT])):
            value = slot.get(field)
            require(type(value) is list and all(type(item) is int for item in value) and
                    value == expected, "slot " + field + " is outside centered observation scope")
        binding = object_value(slot.get("binding"), "binding")
        for key in BINDING_KEYS:
            integer(binding.get(key), "binding." + key, 1)
        require((binding["context"], binding["generation"]) == (context, generation),
                "slots must share the published decoder lifetime")
        for field, seen in (("token", seen_tokens), ("decode_identity", seen_identities),
                            ("submission_ordinal", seen_ordinals)):
            require(binding[field] not in seen, "duplicate selected original " + field)
            seen.add(binding[field])
        small = geometry(slot.get("small"), "small", small=True)
        composed = geometry(slot.get("composed"), "composed")
        copy_source = geometry(slot.get("copy_source"), "copy_source")
        public = geometry(slot.get("public_canvas"), "public_canvas")
        require(composed == copy_source, "copy_source changed composed storage epoch/view")
        require(slot["small"]["surface_serial"] not in
                (slot["composed"]["surface_serial"], slot["public_canvas"]["surface_serial"]),
                "small picture and canvases must use distinct storage views")
        if slot["direct_public"]:
            require(composed == public, "direct public canvas storage epoch/view changed")
        else:
            require(slot["composed"]["surface_serial"] != slot["public_canvas"]["surface_serial"],
                    "indirect public canvas must have a separate storage view")
        for stage, physical in (("small", slot["small"]), ("composed", slot["composed"]),
                                ("public", slot["public_canvas"])):
            name = filename(slot.get(stage + "_file"))
            require(name not in files, "payload filenames must be unique")
            files.add(name)
            blob = blobs.get(name)
            require(type(blob) is bytes, "missing byte payload: " + name)
            declared = integer(slot.get(stage + "_bytes"), stage + "_bytes", 1)
            expected = physical["width"] * physical["height"] * 3 // 2
            require(declared == expected and len(blob) == declared,
                    stage + " packed payload length mismatch")
        for stage, descriptor in (("composed", "composed"), ("public", "public_canvas")):
            counts = compare_canvas(number, stage, blobs[slot["small_file"]],
                                    blobs[slot[stage + "_file"]], slot[descriptor]["height"])
            for plane, count in counts.items():
                compared[stage][plane] += count
        observation = join_binding(rows, binding, destroy)
        # Slots are selected when completed outputs are composed. Decode
        # submission order can differ from that completion order (B pictures),
        # so ordinals are unique identities, never a media/presentation clock.
        require(observation["materialized_seq"] > previous_materialized,
                "selected public materialization order must increase")
        previous_materialized = observation["materialized_seq"]
        observations.append(dict(observation, slot=number, binding=dict(binding)))
    return {
        "status": "PASS_SCOPED_OWN_CENTERED_NV12_PAYLOAD", "schema": SCHEMA,
        "pid": pid, "driver_instance": instance, "slots": SLOTS_REQUIRED,
        "publication": dict(publication), "compared_bytes": compared,
        "observations": observations, "trace_records_checked": len(rows),
        "scope": "Each full coded-height composed and actual public NV12 canvas independently matches the small source at ROI [320,180,1280,720], with limited black outside; successful OWN bindings and decoder retirement joined.",
        "coded_bottom_rows": "COMPARED_WHEN_PRESENT",
        "pitch_padding": "EXCLUDED_FROM_PAYLOAD_COMPARISON",
        "trace_scope": "Contiguous frozen selected-session prefix through selected decoder-context retirement.",
        "allocation_identity": "Surface storage-view lifetime only; no global DMA-BUF alias identity.",
        "token_identity": "SYNTHETIC_VA_BINDING_TOKEN",
        "actual_media_pts": "NOT_OBSERVED", "source_ffprobe_pts_join": "NOT_OBSERVED",
        "gpu_reader_return": "NOT_OBSERVED", "displayed_pixels": "NOT_OBSERVED",
        "stock_compatibility": "NOT_QUALIFIED_BY_PRIVATE_OBSERVER",
    }


def parse_trace(raw):
    require(type(raw) is bytes and len(raw) <= MAX_TRACE_BYTES, "trace exceeds bounded size")
    rows = []
    for line in raw.decode("utf-8", errors="strict").splitlines(keepends=True):
        if TRACE_SCHEMA not in line:
            continue
        require(line.endswith("\n"), "unterminated OWN trace line")
        row = json.loads(line, object_pairs_hook=strict_object)
        require(type(row) is dict and row.get("schema") == TRACE_SCHEMA,
                "malformed OWN trace line")
        rows.append(row)
    return rows


def read_regular(path, maximum):
    # Frozen trusted directory scope, matching the older VPP reader. Reject
    # terminal links and verify identity/size/timestamps on the open descriptor.
    path = Path(path)
    before = path.lstat()
    require(stat.S_ISREG(before.st_mode), "artifact must be a regular file: " + str(path))
    descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        opened = os.fstat(descriptor)
        require(stat.S_ISREG(opened.st_mode) and
                (before.st_dev, before.st_ino) == (opened.st_dev, opened.st_ino) and
                opened.st_size <= maximum,
                "artifact changed or exceeds bound: " + str(path))
        with os.fdopen(descriptor, "rb", closefd=False) as stream:
            data = stream.read(maximum + 1)
        after = os.fstat(descriptor)
        require(len(data) == opened.st_size and len(data) <= maximum and
                (after.st_size, after.st_mtime_ns, after.st_ctime_ns) ==
                (opened.st_size, opened.st_mtime_ns, opened.st_ctime_ns),
                "artifact changed while reading: " + str(path))
        return data
    finally:
        os.close(descriptor)


def audit(manifest_path, trace_path):
    """Return a JSON-compatible scoped PASS or FAIL report for explicit files."""
    try:
        manifest_path = Path(manifest_path)
        doc = json.loads(read_regular(manifest_path, MAX_MANIFEST_BYTES),
                         object_pairs_hook=strict_object)
        object_value(doc, "manifest")
        require(type(doc.get("slots")) is list and len(doc["slots"]) <= SLOTS_REQUIRED,
                "manifest slots exceed quota")
        blobs = {}
        for slot in doc["slots"]:
            slot = object_value(slot, "slot")
            for stage in ("small", "composed", "public"):
                name = filename(slot.get(stage + "_file"))
                if name not in blobs:
                    bound = (SMALL_WIDTH * SMALL_HEIGHT * 3 // 2
                             if stage == "small" else MAX_CANVAS_BYTES)
                    blobs[name] = read_regular(manifest_path.parent / name, bound)
        return check_document(doc, blobs, parse_trace(read_regular(trace_path, MAX_TRACE_BYTES)))
    except (ValueError, OSError, KeyError, TypeError, OverflowError) as error:
        return {"status": "FAIL", "error": str(error)}


def write_new_report(path, result):
    data = (json.dumps(result, sort_keys=True) + "\n").encode("utf-8")
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                         os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, "wb") as output:
        output.write(data)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest_path", nargs="?", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--results", type=Path, help="create a new report; never overwrite")
    args = parser.parse_args(argv)
    if (args.manifest is None) == (args.manifest_path is None):
        parser.error("supply one manifest, positionally or with --manifest")
    result = audit(args.manifest or args.manifest_path, args.trace)
    if args.results is not None:
        try:
            write_new_report(args.results, result)
        except OSError as error:
            print(json.dumps({"status": "FAIL", "error": "cannot create results: " + str(error)},
                             sort_keys=True))
            return 1
    print(json.dumps(result, sort_keys=True))
    return 0 if result["status"].startswith("PASS_") else 1


if __name__ == "__main__":
    sys.exit(main())
