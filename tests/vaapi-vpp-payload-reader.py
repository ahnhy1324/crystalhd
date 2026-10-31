#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Offline, source-row model for the private completed-VPP payload observer.

This reader accepts only eight full-canvas 1280x720 NV12 Bob checkpoints.  It
compares CPU payloads with independent row arithmetic and joins OWN trace
bindings.  It does not infer source PTS, presentation order, or GPU lifetime.
Input artifacts must be frozen and trusted: this is an evidence consistency
checker, not authentication of a diagnostic backend or an untrusted uploader.
"""

import argparse
import json
import os
from pathlib import Path
import stat
import sys


SCHEMA = "crystalhd-vpp-payload-v1"
TRACE_SCHEMA = "crystalhd-vaapi-trace-v4"
WIDTH, HEIGHT, QUOTA = 1280, 720, 8
BINDING_KEYS = ("context", "generation", "token", "decode_identity",
                "submission_ordinal", "surface", "owner")
PARENT_KEYS = BINDING_KEYS[:5]
GEOMETRY_KEYS = ("surface_serial", "storage_epoch", "width", "height",
                 "pitch", "plane_address", "fourcc")
MAX_TRACE_BYTES = 64 * 1024 * 1024


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


def geometry(value, label, expected_height):
    value = object_value(value, label)
    require(all(key in value for key in GEOMETRY_KEYS),
            label + " is missing geometry fields")
    for key in ("surface_serial", "storage_epoch", "width", "height"):
        integer(value[key], label + "." + key, 1)
    require(value["fourcc"] == "NV12" and value["width"] == WIDTH and
            value["height"] == expected_height,
            label + " must be physical 1280x" + str(expected_height) + " NV12")
    for key in ("pitch", "plane_address"):
        pair = value[key]
        require(type(pair) is list and len(pair) == 2,
                label + "." + key + " must have two values")
        for item in pair:
            integer(item, label + "." + key, WIDTH if key == "pitch" else 1)
    extents = []
    for plane, rows in enumerate((expected_height, expected_height // 2)):
        extent = (rows - 1) * value["pitch"][plane] + WIDTH
        begin = value["plane_address"][plane]
        require(begin + extent < 2 ** 64,
                label + " plane address extent overflows")
        extents.append((begin, begin + extent))
    require(extents[0][1] <= extents[1][0] or extents[1][1] <= extents[0][0],
            label + " physical NV12 planes overlap")
    # Diagnostic extras may describe the allocation, but cannot relax bounds.
    # No pointer dereference or inferred DMA-BUF alias identity occurs here.
    return tuple(tuple(value[key]) if type(value[key]) is list else value[key]
                 for key in GEOMETRY_KEYS)


def packed_bytes(geom):
    return geom["width"] * geom["height"] * 3 // 2


def trace_index(trace_rows, pid, instance):
    require(type(trace_rows) is list and trace_rows, "trace rows are required")
    selected = []
    for row in trace_rows:
        row = object_value(row, "trace row")
        require(row.get("schema") == TRACE_SCHEMA, "unsupported trace schema")
        if row.get("pid") == pid and row.get("driver_instance") == instance:
            selected.append(row)
    require(selected, "manifest session has no trace rows")
    previous_ns = 0
    commits, cancels, submits = {}, {}, {}
    for seq, row in enumerate(selected, 1):
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
        event = row["event"]
        if event in ("vpp_commit", "vpp_cancel"):
            table = commits if event == "vpp_commit" else cancels
            table.setdefault(row["operation"], []).append(row)
        elif event == "submit_bind":
            key = tuple(row[key] for key in BINDING_KEYS)
            submits.setdefault(key, []).append(row)
    require(selected[-1]["event"] == "session_destroy" and
            sum(row["event"] == "session_destroy" for row in selected) == 1,
            "selected trace session must end with one session_destroy")
    return commits, cancels, submits


def compare_model(slot, source, target):
    """Use physical source parity independently for Y and interleaved UV."""
    compact = slot["source_is_field"]
    parity = slot["requested_parity"]
    physical_height = slot["source"]["height"]
    source_offset, target_offset = 0, 0
    for plane, logical_rows in (("Y", HEIGHT), ("UV", HEIGHT // 2)):
        physical_rows = physical_height if plane == "Y" else physical_height // 2
        target_rows = logical_rows
        for output_row in range(target_rows):
            # Select within this plane's physical field, then convert to the
            # compact storage row only when the captured source is a field.
            field_row = output_row * (logical_rows // 2) // target_rows
            selected_row = field_row if compact else field_row * 2 + parity
            require(selected_row < physical_rows, "source row exceeds physical plane")
            expected = source[source_offset + selected_row * WIDTH:
                              source_offset + (selected_row + 1) * WIDTH]
            actual = target[target_offset + output_row * WIDTH:
                            target_offset + (output_row + 1) * WIDTH]
            if expected != actual:
                column = next(index for index, pair in enumerate(zip(expected, actual))
                              if pair[0] != pair[1])
                component = plane if plane == "Y" else ("U" if column % 2 == 0 else "V")
                raise ValueError("operation %d %s mismatch at row %d byte %d" %
                                 (slot["operation"], component, output_row, column))
        source_offset += physical_rows * WIDTH
        target_offset += target_rows * WIDTH


def compare_parent_sources(slots, blobs):
    """Bind compact first-field payload to the sibling's completed full source."""
    first, second = slots
    if not first["source_is_field"] and not second["source_is_field"]:
        require(blobs[first["source_file"]] == blobs[second["source_file"]],
                "same immutable parent has different full source bytes")
        return
    require(first["source_is_field"] != second["source_is_field"],
            "a parent needs a completed full source checkpoint")
    compact = first if first["source_is_field"] else second
    full = second if first["source_is_field"] else first
    compact_data, full_data = blobs[compact["source_file"]], blobs[full["source_file"]]
    compact_offset, full_offset = 0, 0
    parity = compact["requested_parity"]
    for rows in (HEIGHT, HEIGHT // 2):
        for row in range(rows // 2):
            expected = full_data[full_offset + (row * 2 + parity) * WIDTH:
                                 full_offset + (row * 2 + parity + 1) * WIDTH]
            actual = compact_data[compact_offset + row * WIDTH:
                                  compact_offset + (row + 1) * WIDTH]
            require(actual == expected,
                    "compact source disagrees with completed immutable parent")
        compact_offset += rows // 2 * WIDTH
        full_offset += rows * WIDTH


def check_document(doc, blobs, trace_rows):
    """Return a scoped PASS dictionary or raise ValueError; performs no I/O."""
    doc = object_value(doc, "manifest")
    require(doc.get("schema") == SCHEMA, "unsupported payload schema")
    pid = integer(doc.get("pid"), "pid", 1)
    instance = integer(doc.get("driver_instance"), "driver_instance", 1)
    require(type(doc.get("quota")) is int and doc["quota"] == QUOTA,
            "manifest quota must be eight")
    slots = doc.get("slots")
    require(type(slots) is list and len(slots) == QUOTA,
            "exactly eight selected payload slots are required")
    require(type(blobs) is dict, "payload blobs must be a dictionary")
    commits, cancels, submits = trace_index(trace_rows, pid, instance)
    groups, operations, files, seen_slots = {}, set(), set(), set()
    for slot in slots:
        slot = object_value(slot, "slot")
        for flag in ("copy_ok", "process_success", "committed", "current"):
            require(slot.get(flag) is True, "slot " + flag + " must be true")
        require(type(slot.get("status")) is int and slot["status"] == 0,
                "slot status must be successful")
        stream = integer(slot.get("stream_index"), "stream_index")
        parent = integer(slot.get("parent_index"), "parent_index")
        parity = integer(slot.get("requested_parity"), "requested_parity")
        require(stream in (0, 1) and parent in (0, 1) and parity in (0, 1),
                "stream, parent, and parity must each be zero or one")
        selection = (stream, parent, parity)
        require(selection not in seen_slots, "duplicate parent/parity slot")
        seen_slots.add(selection)
        operation = integer(slot.get("operation"), "operation", 1)
        require(operation not in operations, "duplicate VPP operation")
        operations.add(operation)
        binding = object_value(slot.get("binding"), "binding")
        for key in BINDING_KEYS:
            integer(binding.get(key), "binding." + key, 1)
        source_surface = integer(slot.get("source_surface"), "source_surface", 1)
        source_owner = integer(slot.get("source_owner"), "source_owner", 1)
        source_key = tuple(binding[key] for key in PARENT_KEYS) + (source_surface, source_owner)
        require(len(submits.get(source_key, [])) == 1 and
                submits[source_key][0]["outcome"] == 0,
                "canonical source does not match one successful submit_bind")
        matching = commits.get(operation, [])
        require(len(matching) == 1 and not cancels.get(operation),
                "operation needs one commit and no cancellation")
        commit = matching[0]
        require(commit["outcome"] == 0 and
                all(commit[key] == binding[key] for key in BINDING_KEYS),
                "successful VPP commit binding mismatch")
        require(submits[source_key][0]["seq"] < commit["seq"],
                "VPP commit precedes source submit_bind")
        require(type(slot.get("source_is_field")) is bool,
                "source_is_field must be a boolean")
        compact = slot["source_is_field"]
        for crop in ("source_crop", "target_crop"):
            rectangle = slot.get(crop)
            require(type(rectangle) is list and len(rectangle) == 4 and
                    all(type(value) is int for value in rectangle) and
                    rectangle == [0, 0, WIDTH, HEIGHT],
                    "only full origin-aligned 1280x720 crops are supported")
        effective = geometry(slot.get("source"), "source", HEIGHT // 2 if compact else HEIGHT)
        canonical = geometry(slot.get("source_canonical"), "source_canonical", HEIGHT)
        target = geometry(slot.get("target"), "target", HEIGHT)
        target_canonical = geometry(slot.get("target_canonical"), "target_canonical", HEIGHT)
        require(target == target_canonical, "target storage epoch/view changed")
        require(slot["target"]["surface_serial"] not in
                (slot["source"]["surface_serial"], slot["source_canonical"]["surface_serial"]),
                "in-place source/target storage is outside immutable-source scope")
        if not compact:
            require(effective == canonical, "full source storage epoch/view changed")
        else:
            require(slot["source"]["surface_serial"] != slot["source_canonical"]["surface_serial"],
                    "compact source must be a separate allocation view")
        first = object_value(slot.get("compact_first_field"), "compact_first_field")
        require(first.get("valid") is compact, "compact first-field validity mismatch")
        if compact:
            require(type(first.get("bottom")) is bool and
                    type(first.get("bottom_first")) is bool and
                    first["bottom"] == bool(parity) and
                    first["bottom_first"] == bool(parity),
                    "compact physical parity/first-field order mismatch")
            for key in ("generation", "decode_identity", "token"):
                require(type(first.get(key)) is int and first[key] == binding[key],
                        "compact first-field " + key + " mismatch")
            for key in ("picture_number", "sess_num", "picture_meta_payload"):
                integer(first.get(key), "compact first-field " + key)
        for kind in ("source", "target"):
            name = filename(slot.get(kind + "_file"))
            require(name not in files, "payload filenames must be unique")
            files.add(name)
            blob = blobs.get(name)
            require(type(blob) is bytes, "missing byte payload: " + name)
            expected_size = packed_bytes(slot[kind])
            declared = integer(slot.get(kind + "_bytes"), kind + "_bytes", 1)
            require(declared == expected_size and len(blob) == declared,
                    kind + " packed payload length mismatch")
        compare_model(slot, blobs[slot["source_file"]], blobs[slot["target_file"]])
        groups.setdefault((stream, parent), []).append((source_key, canonical, slot))
    expected = {(stream, parent, parity) for stream in (0, 1)
                for parent in (0, 1) for parity in (0, 1)}
    require(seen_slots == expected, "missing stream/parent/parity checkpoint")
    lifetimes = []
    for stream in (0, 1):
        stream_keys = []
        for parent in (0, 1):
            entries = groups[(stream, parent)]
            require(len(entries) == 2 and entries[0][:2] == entries[1][:2],
                    "parity pair does not share one immutable source binding/view")
            stream_keys.append(entries[0][0])
            compare_parent_sources([entry[2] for entry in entries], blobs)
        require(stream_keys[0][:2] == stream_keys[1][:2],
                "stream parents belong to different context/generation lifetimes")
        require(stream_keys[0][2:5] != stream_keys[1][2:5] and
                stream_keys[0][4] < stream_keys[1][4],
                "parent indices must name distinct increasing source ordinals")
        lifetimes.append(stream_keys[0][:2])
    require(lifetimes[0] != lifetimes[1], "two stream lifetimes must be distinct")
    return {
        "status": "PASS_SCOPED_OWN_COMPLETED_VPP_PAYLOAD",
        "schema": SCHEMA, "pid": pid, "driver_instance": instance,
        "slots": QUOTA, "source_parents": 4, "physical_parities_per_parent": 2,
        "compared_target_bytes": sum(slot["target_bytes"] for slot in slots),
        "scope": "All active Y/U/V bytes match independently selected OWN source rows; successful raw OWN bindings joined.",
        "allocation_identity": "Surface storage-view lifetime only; no global DMA-BUF alias identity.",
        "source_pts": "NOT_RUN", "chronological_field_presentation": "NOT_RUN",
        "gpu_import_fence_lifetime": "NOT_RUN", "displayed_pixels": "NOT_RUN",
    }


def parse_trace(raw):
    require(type(raw) is bytes and len(raw) <= MAX_TRACE_BYTES,
            "trace exceeds bounded size")
    rows = []
    for line in raw.decode("utf-8", errors="strict").splitlines():
        if TRACE_SCHEMA not in line:
            continue
        row = json.loads(line, object_pairs_hook=strict_object)
        require(type(row) is dict and row.get("schema") == TRACE_SCHEMA,
                "malformed OWN trace line")
        rows.append(row)
    return rows


def read_regular(path, maximum):
    # Reject links before open, then verify the opened descriptor itself.
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


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--trace", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        doc = json.loads(read_regular(args.manifest, 256 * 1024),
                         object_pairs_hook=strict_object)
        object_value(doc, "manifest")
        require(type(doc.get("slots")) is list and len(doc["slots"]) <= QUOTA,
                "manifest slots exceed quota")
        blobs = {}
        for slot in doc["slots"]:
            slot = object_value(slot, "slot")
            for kind in ("source", "target"):
                name = filename(slot.get(kind + "_file"))
                if name not in blobs:
                    blobs[name] = read_regular(args.manifest.parent / name,
                                               WIDTH * HEIGHT * 3 // 2)
        result = check_document(doc, blobs, parse_trace(read_regular(args.trace, MAX_TRACE_BYTES)))
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(json.dumps({"status": "FAIL", "error": str(error)}, sort_keys=True))
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
