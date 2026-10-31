#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Owned offline MPEG-1 packet/picture/token proof. No decoder or hardware.

All table order is retained. Only actual container packet/frame PTS is used.
"""
import copy
from fractions import Fraction
import hashlib
import re


TOKEN_STEP, MAXIMUM_PACKETS = 100000, 10000
TYPE = {1: "I", 2: "P", 3: "B"}
CAPTURE = re.compile(r"^Captured packed output: requested-format=YUY2 frame-index=(\d+) token=(\d+) picture-number=(\d+) geometry=(\d+)x(\d+) pulldown=(\d+) flags=([0-9a-f]+) chroma-format=([0-9a-f]+) output-flags=([0-9a-f]+) aspect-ratio=(\d+) colour-primaries=(\d+)$", re.M)


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def number(value, name, optional=False):
    if optional and value in (None, "N/A"):
        return None
    require(not isinstance(value, bool) and re.fullmatch(r"-?\d+", str(value)) is not None, "invalid integer " + name)
    return int(value)


def token(index, generation=0):
    require(0 <= index < MAXIMUM_PACKETS and 0 <= generation < 1000, "token inputs outside collector bounds")
    return (generation * (MAXIMUM_PACKETS + 1) + index + 1) * TOKEN_STEP


def data_hash(value):
    require(isinstance(value, str) and re.fullmatch(r"SHA256:[0-9a-fA-F]{64}", value) is not None, "missing qualified packet SHA256")
    return value.split(":", 1)[1].lower()


def video_stream(table, container, width=352, height=288):
    streams = table.get("streams", [])
    require(len(streams) == 1 and streams[0].get("codec_type") == "video", "table must select exactly one video stream")
    stream = streams[0]
    require(stream.get("codec_name") == "mpeg1video" and stream.get("pix_fmt") == "yuv420p", "source codec or planar format differs")
    require((stream.get("width"), stream.get("height")) == (width, height), "source geometry differs")
    # Raw-ES r_frame_rate is a probe estimate, not the sequence-header rate.
    # Require its average25 and separately verify every sequence-header code3
    # plus all actual MKV packet/frame PTS and40ms durations below.
    rate = Fraction(stream.get("avg_frame_rate", "0/1"))
    require(rate == 25, "source average rate differs from25")
    require(Fraction(stream.get("r_frame_rate", "0/1")) > 0, "invalid probe nominal rate")
    if container == "MKV":
        require(Fraction(stream["r_frame_rate"]) == 25, "container nominal rate differs from25")
    base = Fraction(stream["time_base"])
    require(base > 0, "nonpositive actual time base")
    return {"index": number(stream["index"], "stream index"), "time_base": str(base),
            "metadata": {key: stream.get(key, "UNKNOWN") for key in ("codec_name", "pix_fmt", "width", "height", "r_frame_rate", "avg_frame_rate", "field_order", "chroma_location", "color_range", "color_space", "color_primaries", "color_transfer")},
            "role": container}


def packet_rows(table, count):
    rows = table.get("packets", [])
    require(len(rows) == count, "packet population differs")
    result = []
    for ordinal, row in enumerate(rows):
        require("C" not in row.get("flags", ""), "corrupt packet flag")
        result.append({"coded_ordinal": ordinal, "pos": number(row.get("pos"), "packet position"),
                       "size": number(row.get("size"), "packet size"), "sha256": data_hash(row.get("data_hash")),
                       "pts": number(row.get("pts"), "packet PTS", True),
                       "dts": number(row.get("dts"), "packet DTS", True),
                       "duration": number(row.get("duration"), "packet duration", True),
                       "stream_index": number(row.get("stream_index"), "packet stream")})
    require(all(row["pos"] >= 0 and row["size"] > 0 for row in result), "nonpositive packet extent")
    require(len({row["pos"] for row in result}) == count, "duplicate packet byte position")
    return result


def picture_header(payload):
    starts = [match.start() for match in re.finditer(b"\x00\x00\x01\x00", payload)]
    require(len(starts) == 1 and starts[0] + 6 <= len(payload), "packet must contain exactly one complete picture-header prefix")
    pos = starts[0] + 4
    word = (payload[pos] << 8) | payload[pos + 1]
    picture_type = (word >> 3) & 7
    require(picture_type in TYPE, "unsupported picture coding type")
    require(b"\x00\x00\x01\xb5" not in payload, "MPEG2 extension is not admitted as MPEG1")
    for match in re.finditer(b"\x00\x00\x01\xb3", payload):
        pos = match.start() + 4
        require(pos + 4 <= len(payload), "short sequence header")
        width = (payload[pos] << 4) | (payload[pos + 1] >> 4)
        height = ((payload[pos + 1] & 15) << 8) | payload[pos + 2]
        require((width, height, payload[pos + 3] & 15) == (352, 288, 3), "sequence-header geometry/rate differs")
    return {"temporal_reference": word >> 6, "pict_type": TYPE[picture_type],
            "sequence_headers": payload.count(b"\x00\x00\x01\xb3")}


def join_coded(es_rows, mkv_rows):
    require(len(es_rows) == len(mkv_rows) and es_rows, "coded table lengths differ")
    result, end = [], 0
    for index, (es, mkv) in enumerate(zip(es_rows, mkv_rows)):
        require(es["coded_ordinal"] == mkv["coded_ordinal"] == index, "coded table ordinal differs")
        require(es["sha256"] == mkv["sha256"] and es["size"] == mkv["size"], "ES/MKV ordered AU payload identity differs")
        require(es["pos"] == end, "ES packets must cover the exact file contiguously from0")
        require(mkv["pts"] is not None, "genuine MKV packet PTS is missing")
        end += es["size"]
        result.append({"coded_ordinal": index, "source_token": token(index), "payload_sha256": es["sha256"],
                       "size": es["size"], "es_pos": es["pos"], "mkv_pos": mkv["pos"],
                       "mkv_pts": mkv["pts"], "mkv_dts": mkv["dts"], "mkv_duration": mkv["duration"]})
    require(len({row["mkv_pts"] for row in result}) == len(result), "duplicate container picture PTS")
    return result


def join_display(es_frames, mkv_frames, coded, time_base, dimensions=(352, 288), cadence=Fraction(1, 25)):
    count = len(coded)
    require(len(es_frames) == len(mkv_frames) == count, "software frame population differs")
    lookup_es = {row["es_pos"]: row for row in coded}
    lookup_mkv = {row["mkv_pos"]: row for row in coded}
    require(len(lookup_es) == len(lookup_mkv) == count, "coded positions are not unique")
    display, seen = [], set()
    for ordinal, (es, mkv) in enumerate(zip(es_frames, mkv_frames)):
        for frame in (es, mkv):
            require(frame.get("media_type") == "video" and frame.get("pix_fmt") == "yuv420p", "software frame type/format differs")
            require((frame.get("width"), frame.get("height")) == dimensions and frame.get("interlaced_frame") == 0 and frame.get("repeat_pict", 0) == 0, "software picture geometry/progressive/repeat contract differs")
        e = lookup_es.get(number(es.get("pkt_pos"), "ES frame source position"))
        m = lookup_mkv.get(number(mkv.get("pkt_pos"), "MKV frame source position"))
        require(e is not None and m is not None and e["coded_ordinal"] == m["coded_ordinal"], "software ES/MKV display-picture source association differs")
        require(e["coded_ordinal"] not in seen, "duplicate displayed coded picture")
        seen.add(e["coded_ordinal"])
        require(es.get("pict_type") == mkv.get("pict_type") == e["pict_type"], "independent picture type differs")
        for frame in (es, mkv):
            require(number(frame.get("pkt_size"), "software frame packet size") == e["size"], "software frame packet extent differs")
        pts = number(mkv.get("pts"), "actual decoded MKV frame PTS")
        require(pts == e["mkv_pts"], "container packet/frame presentationPTS differs")
        if "best_effort_timestamp" in mkv:
            require(number(mkv["best_effort_timestamp"], "MKV best effort") == pts, "best-effort timestamp differs from actual PTS")
        duration = number(mkv.get("duration"), "decoded frame duration", True)
        require(duration is None or Fraction(duration) * time_base == cadence, "actual decoded frame duration differs")
        if display:
            require(Fraction(pts - display[-1]["pts"]) * time_base == cadence, "decoded presentationPTS cadence is not exact25fps")
        display.append({"display_ordinal": ordinal, "coded_ordinal": e["coded_ordinal"], "source_token": e["source_token"],
                        "pts": pts, "time_base": str(time_base), "pts_seconds_exact": str(Fraction(pts) * time_base),
                        "duration": duration, "pict_type": e["pict_type"], "temporal_reference": e["temporal_reference"],
                        "payload_sha256": e["payload_sha256"]})
    require(seen == set(range(count)), "display source census incomplete")
    return display


def join_container_display(frames, coded, time_base, dimensions=(352, 288), cadence=Fraction(1, 25)):
    """Actual decoded container picture positions join its exact ordered ES AUs.

    This does not manufacture an ES frame table or assign PTS to raw I420.
    """
    require(len(frames) == len(coded), "decoded container frame population differs")
    by_position = {row["mkv_pos"]: row for row in coded}
    require(len(by_position) == len(coded), "container AU positions are not unique")
    result, seen = [], set()
    for ordinal, frame in enumerate(frames):
        require(frame.get("media_type") == "video" and frame.get("pix_fmt") == "yuv420p", "container decoded frame type/format differs")
        require((frame.get("width"), frame.get("height")) == dimensions and frame.get("interlaced_frame") == 0 and frame.get("repeat_pict", 0) == 0, "container decoded progressive/geometry/repeat contract differs")
        row = by_position.get(number(frame.get("pkt_pos"), "container frame source position"))
        require(row is not None and row["coded_ordinal"] not in seen, "missing or repeated container source picture")
        seen.add(row["coded_ordinal"])
        require(frame.get("pict_type") == row["pict_type"] and number(frame.get("pkt_size"), "container frame source extent") == row["size"], "container frame/header type or extent differs")
        pts = number(frame.get("pts"), "actual decoded container PTS")
        require(pts == row["mkv_pts"], "container packet/frame PTS differs")
        if "best_effort_timestamp" in frame:
            require(number(frame["best_effort_timestamp"], "best-effort timestamp") == pts, "best-effort differs from actual frame PTS")
        duration = number(frame.get("duration"), "container decoded duration", True)
        require(duration is None or Fraction(duration)*time_base == cadence, "container decoded duration differs")
        if result:
            require(Fraction(pts-result[-1]["pts"])*time_base == cadence, "container decoded presentation cadence differs")
        result.append({"display_ordinal": ordinal, "coded_ordinal": row["coded_ordinal"], "source_token": row["source_token"],
                       "pts": pts, "time_base": str(time_base), "pts_seconds_exact": str(Fraction(pts)*time_base), "duration": duration,
                       "pict_type": row["pict_type"], "temporal_reference": row["temporal_reference"], "payload_sha256": row["payload_sha256"]})
    require(seen == set(range(len(coded))), "container displayed-picture source census incomplete")
    return result


def capture_rows(text, expected, width=352, height=288):
    require("MPEG-1 research: input-codec=mpeg1video configured-algorithm=1 route=MPEG2VIDEO selector5-not-used" in text, "explicit MPEG1 research route receipt missing")
    matches = CAPTURE.findall(text)
    require(len(matches) == expected, "captured metadata population differs")
    names = ("frame_index", "token", "picture_number", "width", "height", "pulldown", "flags", "chroma_format", "output_flags", "aspect", "colour_primaries")
    rows = [{name: int(value, 16 if name in ("flags", "chroma_format", "output_flags") else 10) for name, value in zip(names, match)} for match in matches]
    require([row["frame_index"] for row in rows] == list(range(expected)), "capture frame-index order differs")
    require({row["token"] for row in rows} == {token(index) for index in range(expected)}, "returned token population differs from this source generation")
    # bc_dts_defs.h:0=no information,9=vdecFrame_X1 (one whole frame).
    # Field patterns and X2/X3/X4 are not this one-progressive-picture contract.
    require(all((row["width"], row["height"], row["flags"], row["chroma_format"]) == (width, height, 0, 1)
                and row["pulldown"] in (0, 9) for row in rows), "returned progressive picture metadata differs")
    require(re.search(r"^Library drain: iteration=1/1 frames=" + str(expected) + "/" + str(expected) + r" pending=0 firmware-EOS=yes output-marker=(?:yes|no) ready=0 cleanup=PASS result=PASS$", text, re.M) is not None, "naturalEOS/pending/ready/cleanup footer failed")
    require(re.search(r"^Library churn: completed=1/1 .* fds=(\d+)/\1 threads=(\d+)/\2 result=PASS$", text, re.M) is not None, "FD/thread closure footer failed")
    return rows


def join_output(rows, display):
    require(len(rows) == len(display), "output/display populations differ")
    for row, frame in zip(rows, display):
        require(row["frame_index"] == frame["display_ordinal"] and row["token"] == frame["source_token"], "returned source token does not match independent software presentation order")
    return {"pictures": len(rows), "all_source_tokens_unique": True,
            "actual_container_packet_to_software_picture_to_native_output_join": True,
            "token_numeric_order_is_presentationPTS": False}


def self_test():
    names = []
    def check(name, condition):
        require(condition, "control failed: " + name)
        names.append(name)
    def rejects(name, function, *arguments):
        try:
            function(*arguments)
        except ValueError:
            names.append(name)
            return
        raise AssertionError("negative control accepted: " + name)
    payloads = [b"\0\0\1\0" + ((tr << 6) | (kind << 3)).to_bytes(2, "big") for tr, kind in ((0, 1), (3, 2), (1, 3), (2, 3))]
    es, mkv = [], []
    for index, payload in enumerate(payloads):
        row = {"coded_ordinal": index, "pos": index * 6, "size": 6, "sha256": hashlib.sha256(payload).hexdigest(), "pts": None, "dts": None, "duration": None}
        es.append(row)
        mkv.append(dict(row, pos=100 + 10 * index, pts=(0, 120, 40, 80)[index], duration=40))
    coded = join_coded(es, mkv)
    for row, payload in zip(coded, payloads):
        row.update(picture_header(payload))
    def frame(position, kind, pts):
        return {"media_type": "video", "pix_fmt": "yuv420p", "width": 352, "height": 288, "interlaced_frame": 0,
                "repeat_pict": 0, "pkt_pos": position, "pkt_size": 6, "pict_type": kind, "pts": pts, "duration": 40}
    es_frames = [frame(es[index]["pos"], coded[index]["pict_type"], None) for index in (0, 2, 3, 1)]
    mkv_frames = [frame(mkv[index]["pos"], coded[index]["pict_type"], mkv[index]["pts"]) for index in (0, 2, 3, 1)]
    display = join_display(es_frames, mkv_frames, coded, Fraction(1, 1000))
    check("actual_container_only_join_without_invented_ES_frames", join_container_display(mkv_frames, coded, Fraction(1, 1000)) == display)
    nominal = {"streams": [{"index": 0, "codec_type": "video", "codec_name": "mpeg1video", "pix_fmt": "yuv420p", "width": 352, "height": 288, "r_frame_rate": "50/1", "avg_frame_rate": "25/1", "time_base": "1/1200000"}]}
    check("retain_ES_probe_nominal50_average25", video_stream(nominal, "ES")["metadata"]["r_frame_rate"] == "50/1")
    rejects("reject_container_nominal50", video_stream, nominal, "MKV")
    wrong_average = copy.deepcopy(nominal); wrong_average["streams"][0]["avg_frame_rate"] = "50/1"
    rejects("reject_average50_even_when_nominal50", video_stream, wrong_average, "ES")
    missing_average = copy.deepcopy(nominal); missing_average["streams"][0].pop("avg_frame_rate")
    rejects("reject_missing_average_rate", video_stream, missing_average, "ES")
    check("literal_collector_token_generation0", [token(i) for i in range(4)] == [100000, 200000, 300000, 400000])
    check("literal_IPB_presentation_join", [row["source_token"] for row in display] == [100000, 300000, 400000, 200000]
          and [row["pts"] for row in display] == [0, 40, 80, 120])
    rows = [{"frame_index": i, "token": value} for i, value in enumerate((100000, 300000, 400000, 200000))]
    check("native_reordered_tokens_match_actualPTS", join_output(rows, display)["pictures"] == 4)
    text = "MPEG-1 research: input-codec=mpeg1video configured-algorithm=1 route=MPEG2VIDEO selector5-not-used\n"
    text += "\n".join("Captured packed output: requested-format=YUY2 frame-index=%d token=%d picture-number=%d geometry=352x288 pulldown=0 flags=0 chroma-format=1 output-flags=20000 aspect-ratio=1 colour-primaries=0" % (i, value, i+3) for i, value in enumerate((100000, 300000, 400000, 200000)))
    text += "\nLibrary drain: iteration=1/1 frames=4/4 pending=0 firmware-EOS=yes output-marker=no ready=0 cleanup=PASS result=PASS\nLibrary churn: completed=1/1 rss-kib=10/10 peak=10 delta=+0 fds=3/3 threads=1/1 result=PASS\n"
    check("literal_complete_capture_metadata", [row["token"] for row in capture_rows(text, 4)] == [100000, 300000, 400000, 200000])
    check("picture_number_repeat_is_diagnostic_only", len(capture_rows(text.replace("picture-number=4", "picture-number=3"), 4)) == 4)
    check("whole_frame_X1_is_one_progressive_picture", all(row["pulldown"] == 9 for row in capture_rows(text.replace("pulldown=0", "pulldown=9"), 4)))
    for pattern in (2, 3, 4, 5, 6, 7, 8, 10):
        rejects("reject_field_or_repeated_pattern_" + str(pattern), capture_rows, text.replace("pulldown=0", "pulldown=" + str(pattern), 1), 4)
    rejects("reject_duplicate_returned_token", capture_rows, text.replace("token=300000", "token=100000"), 4)
    rejects("reject_incomplete_metadata_row", capture_rows, text.replace("colour-primaries=0", "colour-primaries=", 1), 4)
    rejects("reject_nonprogressive_metadata", capture_rows, text.replace("pulldown=0", "pulldown=1", 1), 4)
    rejects("reject_missing_naturalEOS", capture_rows, text.replace("firmware-EOS=yes", "firmware-EOS=no"), 4)
    rejects("reject_FD_retirement_drift", capture_rows, text.replace("fds=3/3", "fds=3/4"), 4)
    changed = copy.deepcopy(mkv); changed[1]["sha256"] = "0" * 64
    rejects("reject_AU_payload_difference", join_coded, es, changed)
    changed = copy.deepcopy(mkv); changed[1]["pts"] = 0
    rejects("reject_duplicate_packetPTS", join_coded, es, changed)
    changed = copy.deepcopy(mkv); changed[1]["pts"] = None
    rejects("reject_missing_actual_packetPTS", join_coded, es, changed)
    changed = copy.deepcopy(es); changed[1]["pos"] += 1
    rejects("reject_noncontiguous_ES", join_coded, changed, mkv)
    changed = copy.deepcopy(mkv_frames); changed[1]["pts"] = 80
    rejects("reject_packet_framePTS_disagreement", join_display, es_frames, changed, coded, Fraction(1, 1000))
    changed = copy.deepcopy(es_frames); changed[1]["pkt_pos"] = 0
    rejects("reject_duplicate_display_picture", join_display, changed, mkv_frames, coded, Fraction(1, 1000))
    changed = copy.deepcopy(mkv_frames); changed[1]["pict_type"] = "P"
    rejects("reject_picture_type_mismatch", join_display, es_frames, changed, coded, Fraction(1, 1000))
    changed = copy.deepcopy(mkv_frames); changed[1].pop("pts")
    rejects("reject_missing_actual_container_framePTS", join_container_display, changed, coded, Fraction(1, 1000))
    changed = copy.deepcopy(mkv_frames); changed[1]["pkt_pos"] = 999
    rejects("reject_unknown_container_picture_position", join_container_display, changed, coded, Fraction(1, 1000))
    changed = copy.deepcopy(rows); changed[1]["token"] = 200000
    rejects("reject_numeric_token_sort_asPTS", join_output, changed, display)
    rejects("reject_extra_native_picture", join_output, rows + rows[:1], display)
    rejects("reject_two_picture_headers_perAU", picture_header, payloads[0] + payloads[1])
    rejects("reject_MPEG2_extension", picture_header, payloads[0] + b"\0\0\1\xb5\0")
    rejects("reject_packet_without_picture_header", picture_header, b"invalid")
    return {"status": "PASS", "control_groups": len(names), "names": names,
            "packet_order_fixture": "I0 P3 B1 B2 -> I0 B1 B2 P3", "hardware_or_actual_media": "NOT_RUN"}
