#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Strict offline OWN packet-admission reader; raw clocks stay unjoined.

This observes complete outer public calls in one original process. Demuxed
packets, accepted sends, returned frames and VA submissions are distinct
populations. No media, foreign pointer, payload, device or private Kodi state
is accessed by this reader.
"""
import argparse
import collections
import importlib.util
import json
from pathlib import Path
import sys


_spec = importlib.util.spec_from_file_location(
    "_packet_reader_safe", Path(__file__).with_name("vaapi-postfilter-reader.py"))
P = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(P)
require, integer, signed, pointer, rational = P.require, P.integer, P.signed, P.pointer, P.rational
SCHEMA = "crystalhd-packet-admission-v1"
FORK_POLICY = "PARENT_PID_ONLY_CHILD_FORWARD_ONLY"
SCOPE = "OUTERMOST_PUBLIC_CALLS_PARENT_PID"
PROVIDERS = {"format": "/app/lib/libavformat.so.63.1.102",
             "codec": "/app/lib/libavcodec.so.63.1.102",
             "util": "/app/lib/libavutil.so.61.1.102"}
VERSIONS = {"format": 4129126, "codec": 4129126, "util": 3998054}
# Confirmed from the unchanged original 12cc1f42 public-header closure for
# these exact providers. A different ABI needs an explicit schema revision.
ENUMS = dict(h264_codec_id=27, video_media_type=0, vaapi_format=44,
             discard_none=-16, discard_default=0, discard_nonref=8)
FOPEN, INFO, READ, FCLOSE = ("avformat_open_input", "avformat_find_stream_info",
                            "av_read_frame", "avformat_close_input")
COPEN, SEND, RECEIVE, FLUSH, CFREE = ("avcodec_open2", "avcodec_send_packet",
                                    "avcodec_receive_frame", "avcodec_flush_buffers",
                                    "avcodec_free_context")
FORMAT_APIS = {FOPEN, INFO, READ, FCLOSE}
CODEC_APIS = {COPEN, SEND, RECEIVE, FLUSH, CFREE}
VOID_APIS = {FCLOSE, FLUSH, CFREE}
EAGAIN, EOF = -11, -541478725
MAX_REPORT_BYTES = 64 * 1024 * 1024
LIMITS = [
    "OWN metadata diagnosis only, not stock compatibility or release acceptance.",
    "Parent PID only; inherited children and nested public forwarding calls are excluded.",
    "Demux/read, codec/send, frame/receive and VA submission populations are not packet/frame identity joins.",
    "Raw packet/frame/stream/context clocks stay distinct; private Kodi transport/start offsets are unobserved.",
    "Aggregate skip-state/counters do not identify each missing picture or prove the cause of the 687/690 source failures."]


def object_value(value, label, keys):
    require(type(value) is dict and all(k in value for k in keys), label + " is incomplete")
    return value


def format_value(value, header):
    object_value(value, "format metadata", ("start_time", "nb_streams", "info_complete"))
    signed(value["start_time"], "format.start_time", 64)
    integer(value["nb_streams"], "format.nb_streams", 0, header["max_streams"])
    require(type(value["info_complete"]) is bool, "format.info_complete must be boolean")


def codec_value(value):
    keys = ("codec_type", "codec_id", "width", "height", "pix_fmt", "skip_frame", "skip_idct",
            "skip_loop_filter", "pkt_timebase", "thread_count", "thread_type", "active_thread_type", "has_b_frames")
    object_value(value, "codec metadata", keys)
    for key in keys:
        if key != "pkt_timebase":
            signed(value[key], "codec." + key)
    rational(value["pkt_timebase"], "codec.pkt_timebase")
    require(all(value[k] >= 0 for k in ("width", "height", "thread_count", "has_b_frames")),
            "negative codec geometry/thread state")


def packet_value(value):
    keys = ("pts", "dts", "duration", "pos", "size", "flags", "stream_index", "time_base",
            "data_null", "side_data_elems")
    object_value(value, "packet metadata", keys)
    for key in ("pts", "dts", "duration", "pos"):
        signed(value[key], "packet." + key, 64)
    for key in ("size", "flags", "stream_index"):
        signed(value[key], "packet." + key)
    integer(value["side_data_elems"], "packet.side_data_elems", 0, (1 << 31)-1)
    require(type(value["data_null"]) is bool, "packet.data_null must be boolean")
    rational(value["time_base"], "packet.time_base")


def stream_value(value, packet, fmt):
    keys = ("index", "id", "codec_type", "codec_id", "width", "height", "time_base", "start_time", "duration")
    object_value(value, "stream metadata", keys)
    for key in ("index", "id", "codec_type", "codec_id", "width", "height"):
        signed(value[key], "stream." + key)
    for key in ("start_time", "duration"):
        signed(value[key], "stream." + key, 64)
    rational(value["time_base"], "stream.time_base")
    require(value["index"] == packet["stream_index"] and
            0 <= value["index"] < fmt["nb_streams"] and packet["size"] >= 0 and
            value["width"] >= 0 and value["height"] >= 0, "invalid packet/stream association")


def frame_value(value, header):
    keys = ("pts", "best_effort_timestamp", "duration", "time_base", "format", "width", "height", "flags", "va_surface")
    object_value(value, "frame metadata", keys)
    for key in ("pts", "best_effort_timestamp", "duration"):
        signed(value[key], "frame." + key, 64)
    for key in ("format", "width", "height", "flags"):
        signed(value[key], "frame." + key)
    rational(value["time_base"], "frame.time_base")
    require(value["width"] >= 0 and value["height"] >= 0, "negative frame geometry")
    if value["format"] == header["vaapi_format"]:
        integer(value["va_surface"], "descriptive VA surface value")
        require(value["width"] > 0 and value["height"] > 0, "VAAPI frame has empty geometry")
    else:
        require(value["va_surface"] is None, "software frame claims a VA surface")


def parse_report(raw):
    require(type(raw) is bytes and len(raw) <= MAX_REPORT_BYTES and raw.endswith(b"\n"),
            "report exceeds quota or lacks final newline")
    lines = raw.splitlines()
    require(2 <= len(lines) <= 32770 and all(lines), "invalid bounded JSONL records")
    rows = [json.loads(line, object_pairs_hook=P.strict_object) for line in lines]
    require(all(type(row) is dict for row in rows), "report records must be objects")
    return rows


def check_document(rows, expected_pid, expected_providers=None, report_path=None):
    """Pure document validation; malformed or incomplete evidence raises."""
    integer(expected_pid, "expected PID", 1, (1 << 31)-1)
    require(type(rows) is list and 2 <= len(rows) <= 32770 and
            all(type(r) is dict for r in rows), "invalid report document")
    header, footer = rows[0], rows[-1]
    require(header.get("type") == "header" and header.get("schema") == SCHEMA and
            footer.get("type") == "footer", "unsupported schema or missing header/footer")
    require(type(header.get("pid")) is int and header["pid"] == expected_pid,
            "foreign header PID")
    require(header.get("provider_ok") is True and header.get("fork_policy") == FORK_POLICY and
            header.get("observation_scope") == SCOPE, "unqualified provider/process observation scope")
    providers = dict(PROVIDERS if expected_providers is None else expected_providers)
    require(set(providers) == set(PROVIDERS), "all three expected providers are required")
    for family, expected in providers.items():
        require(type(expected) is str and "\x00" not in expected and Path(expected).is_absolute() and
                Path(expected).name == Path(PROVIDERS[family]).name and
                header.get(family + "_provider") == expected and
                type(header.get(family + "_version")) is int and header[family + "_version"] == VERSIONS[family],
                "foreign provider/version: " + family)
    capacity = integer(header.get("capacity"), "event capacity", 1, 32768)
    lifecycle_capacity = integer(header.get("lifecycle_capacity"), "lifecycle capacity", 1, 256)
    integer(header.get("max_streams"), "stream capacity", 1, 1024)
    for key in ("h264_codec_id", "video_media_type", "vaapi_format", "discard_none", "discard_default", "discard_nonref"):
        signed(header.get(key), "header." + key)
        require(header[key] == ENUMS[key], "original-header enum identity differs: " + key)
    events = rows[1:-1]
    require(len(events) <= capacity, "event capacity exceeded")
    for key in ("events", "attempted_events", "dropped", "lifecycle_failures", "string_truncations",
                "inflight", "unpublished", "open_formats", "open_codecs"):
        integer(footer.get(key), "footer." + key)
    require(footer["events"] == footer["attempted_events"] == len(events), "footer event counts disagree")
    require(type(footer.get("fork_seen")) is bool and footer.get("clock_failure") is False and
            footer.get("complete") is True and all(footer[k] == 0 for k in
                ("dropped", "lifecycle_failures", "string_truncations", "inflight", "unpublished", "open_formats", "open_codecs")),
            "incomplete/overflowed/lifecycle-failed observation")
    families = {"format": {}, "codec": {}}
    thread_end = {}
    for seq, e in enumerate(events):
        require(e.get("type") == "event" and type(e.get("seq")) is int and e["seq"] == seq,
                "missing/duplicate/reordered event sequence")
        require(type(e.get("pid")) is int and e["pid"] == expected_pid, "foreign event PID")
        tid = integer(e.get("tid"), "TID", 1, (1 << 31)-1)
        begin = integer(e.get("begin_ns"), "begin_ns", 1)
        end = integer(e.get("end_ns"), "end_ns", begin)
        require(begin >= thread_end.get(tid, 0), "same-thread calls overlap or regress")
        thread_end[tid] = end
        api = e.get("api")
        require(api in FORMAT_APIS | CODEC_APIS, "unknown outer public API")
        if api in VOID_APIS:
            require(e.get("ret") is None, "void API has an invented result")
        else:
            signed(e.get("ret"), "original API result")
        for key in ("errno_before", "errno_after"):
            signed(e.get(key), key)
        for key in ("format_instance", "codec_instance", "codec_epoch", "flush_old_epoch", "flush_new_epoch"):
            integer(e.get(key), key, 0, (1 << 32)-1)
        for key in ("format_context", "codec_context", "caller"):
            pointer(e.get(key), key, key == "caller")
        for key in ("concurrent", "opened_before", "unopened_free", "packet_null"):
            require(type(e.get(key)) is bool, key + " must be boolean")
        require(e["concurrent"] is False, "concurrent public calls invalidate context observation")
        for key in ("input_url", "format_url"):
            require(key in e, "missing nullable URL field")
            if e[key] is not None:
                P.text(e[key], key, 1024)
        for key in ("format", "codec", "stream", "packet", "frame"):
            require(key in e, "missing nullable metadata field: " + key)
        family = "format" if api in FORMAT_APIS else "codec"
        other = "codec" if family == "format" else "format"
        instance = e[family + "_instance"]
        address = pointer(e[family + "_context"], family + " context")
        require(e[other + "_instance"] == 0 and pointer(e[other + "_context"], "other context") == 0 and
                e[other] is None, "foreign family identity/metadata on API")
        if family == "format":
            require(e["codec_epoch"] == 0, "format API has codec epoch")
        if api != FOPEN:
            require(e["input_url"] is None and e["format_url"] is None, "URL capture belongs only to open_input")
        if api == FOPEN and e["ret"] < 0:
            require(e["format_url"] is None, "failed open claims a successful context URL")
        require(api == SEND or e["packet_null"] is False, "packet_null belongs only to send_packet")
        if api != FLUSH:
            require(e["flush_old_epoch"] == e["flush_new_epoch"] == 0, "flush epoch marker on another API")
        opening, closing = ((FOPEN, FCLOSE) if family == "format" else (COPEN, CFREE))
        table = families[family]
        if instance == 0:
            require(not e["opened_before"] and e["codec_epoch"] == 0 and
                    ((api == opening and e["ret"] < 0 and not e["unopened_free"]) or
                     (api == closing and e["unopened_free"])),
                    "unknown lifetime used by a data/flush/claimed-open call")
        else:
            require(address > 0 and not e["unopened_free"], "successful lifetime has null/unopened identity")
            if instance not in table:
                addresses = {life["address"] for life in table.values()}
                require((address in addresses or len(addresses) < lifecycle_capacity) and
                        api == opening and e["ret"] >= 0 and not e["opened_before"],
                        "lifetime lacks an observed successful initial open")
                table[instance] = dict(instance=instance, address=address, context=e[family + "_context"],
                                       events=[], closed=False, epoch=1 if family == "codec" else 0,
                                       info_complete=False)
            life = table[instance]
            require(not life["closed"] and life["address"] == address, "call after retirement or changed context address")
            require(e["opened_before"] == bool(life["events"]), "opened_before contradicts lifetime entry state")
            if life["events"]:
                require(life["events"][-1]["end_ns"] <= begin, "overlapping calls on one context lifetime")
            if family == "codec":
                require(e["codec_epoch"] == life["epoch"], "codec epoch changed without observed flush")
            life["events"].append(e)
        if e["format"] is not None:
            format_value(e["format"], header)
        if e["codec"] is not None:
            codec_value(e["codec"])
        if instance:
            require(e[family] is not None, "known context lacks public metadata at the recorded API boundary")
            if family == "format":
                require(e["format"]["info_complete"] == life["info_complete"],
                        "info completion changed without successful find_stream_info")
                if api == INFO and e["ret"] >= 0:
                    life["info_complete"] = True
            if api == FLUSH:
                require(e["flush_old_epoch"] == life["epoch"] and
                        e["flush_new_epoch"] == life["epoch"] + 1, "invalid codec flush transition")
                life["epoch"] += 1
            if api == closing:
                life["closed"] = True
        elif api == closing:
            require(e[family] is None, "unopened close/free must not claim context metadata")
        has_packet = (api == SEND and not e["packet_null"]) or (api == READ and e["ret"] >= 0)
        has_frame = api == RECEIVE and e["ret"] >= 0
        require((e["packet"] is not None) == has_packet and (e["frame"] is not None) == has_frame,
                "packet/frame metadata does not match original ownership/result")
        require((e["stream"] is not None) == (api == READ and e["ret"] >= 0),
                "stream metadata must accompany exactly successful reads")
        if has_packet:
            packet_value(e["packet"])
        if e["stream"] is not None:
            stream_value(e["stream"], e["packet"], e["format"])
        if has_frame:
            frame_value(e["frame"], header)
    lifetimes = {}
    for family, table in families.items():
        require(set(table) == set(range(1, len(table)+1)), "lifetime IDs have gaps")
        values = [table[i] for i in sorted(table)]
        for i, life in enumerate(values):
            require(life["closed"], "successful context lifetime lacks retirement")
            life["begin_ns"] = life["events"][0]["begin_ns"]
            life["end_ns"] = life["events"][-1]["end_ns"]
            for other in values[i+1:]:
                if life["address"] == other["address"]:
                    require(life["end_ns"] <= other["events"][0]["begin_ns"] or
                            other["events"][-1]["end_ns"] <= life["begin_ns"],
                            "context address reused before retirement")
            if family == "codec":
                life["epochs"] = [dict(epoch=epoch, events=[e for e in life["events"] if e["codec_epoch"] == epoch])
                                  for epoch in range(1, life["epoch"]+1)]
        lifetimes[family + "s"] = values
    path = str(Path(report_path).absolute()) if report_path is not None else None
    if path is not None:
        require(Path(path).suffix == ".jsonl" and Path(path).stem.endswith("-" + str(expected_pid)),
                "report filename lost original PID binding")
    return dict(header=header, footer=footer, events=events, lifetimes=lifetimes,
                formats=lifetimes["formats"], codecs=lifetimes["codecs"], report_path=path, providers=providers,
                process_scope=dict(pid=expected_pid, fork_policy=FORK_POLICY,
                                   fork_seen=footer["fork_seen"], children_observed=False),
                source_clock_comparison=False, packet_to_va_identity="NOT_OBSERVED")


def read_report(path, expected_pid, expected_providers=None):
    return check_document(parse_report(P.read_regular(path, MAX_REPORT_BYTES)), expected_pid,
                          expected_providers, report_path=path)


def histogram(values):
    return dict(sorted(collections.Counter(str(v) for v in values).items()))


def census_play(validated, begin_ns, end_ns, expected_video_packets=900, expected_url="/fixture.mp4"):
    """Enumerate actual per-play populations; exact source selection can fail.

    Caller supplies frozen controller windows. Counts alone do not associate a
    decoder context with a demux stream, nor a packet with a frame/VA binding.
    """
    integer(begin_ns, "play begin", 1)
    integer(end_ns, "play end", begin_ns+1)
    integer(expected_video_packets, "expected video packets", 1)
    P.text(expected_url, "expected source URL", 1024)
    require(bool(expected_url), "expected source URL cannot be empty")
    events = [e for e in validated["events"] if begin_ns <= e["begin_ns"] and e["end_ns"] <= end_ns]
    require(not any(e["begin_ns"] < end_ns and e["end_ns"] > begin_ns and
                    not (begin_ns <= e["begin_ns"] and e["end_ns"] <= end_ns)
                    for e in validated["events"]), "play window cuts through a public call")
    header = validated["header"]
    formats, codecs = [], []
    for life in validated["formats"]:
        selected = [e for e in events if e["format_instance"] == life["instance"]]
        if not selected:
            continue
        reads = [e for e in selected if e["api"] == READ]
        successes = [e for e in reads if e["ret"] >= 0]
        video = [e for e in successes if e["stream"]["codec_type"] == header["video_media_type"] and
                 e["stream"]["codec_id"] == header["h264_codec_id"]]
        before_info = [e for e in video if not e["format"]["info_complete"]]
        after_info = [e for e in video if e["format"]["info_complete"]]
        url_evidence = [{k: e[k] for k in ("seq", "input_url", "format_url")} for e in life["events"]
                        if e["api"] == FOPEN and e["ret"] >= 0]
        urls = {e[k] for e in url_evidence for k in ("input_url", "format_url") if e[k]}
        url_status = ("EXACT_PUBLIC_URL_MATCH" if urls == {expected_url} else
                      "NO_PUBLIC_URL" if not urls else "NONMATCHING_OR_CONFLICTING_PUBLIC_URL")
        stream_variants = {}
        for e in video:
            raw = e["stream"]
            stream_variants[json.dumps(raw, sort_keys=True)] = raw
        identities = {tuple(e["stream"][k] for k in ("index", "id", "codec_type", "codec_id")) for e in video}
        formats.append(dict(format_instance=life["instance"], context=life["context"],
            url_evidence=url_evidence, url_status=url_status,
            read_attempts=len(reads), read_successes=len(successes),
            read_eagain=sum(e["ret"] == EAGAIN for e in reads),
            read_eof=sum(e["ret"] == EOF for e in reads),
            read_other_errors=sum(e["ret"] < 0 and e["ret"] not in (EAGAIN, EOF) for e in reads),
            h264_video_outer_reads=len(video), h264_video_before_observed_info_reads=len(before_info),
            h264_video_after_observed_info_reads=len(after_info),
            other_stream_reads=len(successes)-len(video), video_stream_variants=list(stream_variants.values()),
            video_stream_identities=[dict(zip(("index", "id", "codec_type", "codec_id"), identity))
                                        for identity in sorted(identities)],
            successful_find_stream_info=sum(e["api"] == INFO and e["ret"] >= 0 for e in selected),
            info_phase_scope="DESCRIPTIVE_ONLY_NOT_DISCOVERY_OR_PLAYBACK_IDENTITY",
            nested_discovery_calls="EXCLUDED_BY_OUTERMOST_OBSERVER"))
    candidates = [f for f in formats if f["h264_video_outer_reads"] > 0 and
                  f["url_status"] == "EXACT_PUBLIC_URL_MATCH"]
    source = dict(status="NOT_QUALIFIED_AMBIGUOUS_OR_UNBOUND_SOURCE", expected_video_packets=expected_video_packets,
                  expected_url=expected_url, selected_format_instance=None, observed_video_packets=None,
                  selected_stream=None, exact_original_source_frame_mapping=False)
    if len(candidates) == 1 and len(candidates[0]["video_stream_identities"]) == 1:
        f = candidates[0]
        source.update(selected_format_instance=f["format_instance"],
                      observed_video_packets=f["h264_video_outer_reads"],
                      selected_stream=f["video_stream_identities"][0], observed_read_eof=f["read_eof"],
                      selected_stream_metadata_variants=f["video_stream_variants"])
        if f["read_eof"] == 0:
            source["status"] = "NOT_QUALIFIED_ORIGINAL_READ_EOF_ABSENT"
        else:
            source["status"] = ("PASS_EXACT_VIDEO_DEMUX_POPULATION" if
                                f["h264_video_outer_reads"] == expected_video_packets else
                                "FAIL_VIDEO_DEMUX_POPULATION")
    for life in validated["codecs"]:
        selected = [e for e in events if e["codec_instance"] == life["instance"]]
        for epoch in sorted({e["codec_epoch"] for e in selected}):
            calls = [e for e in selected if e["codec_epoch"] == epoch]
            data = [e for e in calls if e["api"] in (SEND, RECEIVE)]
            if not data:
                continue
            sends = [e for e in data if e["api"] == SEND]
            receives = [e for e in data if e["api"] == RECEIVE]
            payload_sends = [e for e in sends if not e["packet_null"] and
                             e["packet"]["size"] > 0 and not e["packet"]["data_null"]]
            empty = [e for e in sends if not e["packet_null"] and e["packet"]["size"] == 0]
            zero_data_null = [e for e in empty if e["packet"]["data_null"]]
            empty_drain = [e for e in zero_data_null if e["packet"]["side_data_elems"] == 0]
            data_sends = [e for e in sends if not e["packet_null"] and e not in empty_drain]
            frames = [e for e in receives if e["ret"] >= 0]
            states = [e["codec"] for e in data]
            identities = sorted({(c["codec_type"], c["codec_id"]) for c in states})
            skip_states = collections.Counter((e["api"], e["codec"]["skip_frame"],
                        e["codec"]["skip_idct"], e["codec"]["skip_loop_filter"], e["ret"]) for e in data)
            codecs.append(dict(codec_instance=life["instance"], codec_epoch=epoch, context=life["context"],
                codec_identities=[list(i) for i in identities],
                h264_video_candidate=identities == [(header["video_media_type"], header["h264_codec_id"])],
                send_attempts=len(sends), send_payload_attempts=len(payload_sends),
                send_data_attempts=len(data_sends), send_data_accepted=sum(e["ret"] >= 0 for e in data_sends),
                send_data_eagain=sum(e["ret"] == EAGAIN for e in data_sends),
                send_data_other_errors=sum(e["ret"] < 0 and e["ret"] != EAGAIN for e in data_sends),
                send_payload_accepted=sum(e["ret"] >= 0 for e in payload_sends),
                send_payload_eagain=sum(e["ret"] == EAGAIN for e in payload_sends),
                send_payload_other_errors=sum(e["ret"] < 0 and e["ret"] != EAGAIN for e in payload_sends),
                send_null_attempts=sum(e["packet_null"] for e in sends),
                send_null_accepted=sum(e["packet_null"] and e["ret"] >= 0 for e in sends),
                send_nonnull_zero_size=len(empty), send_zero_size_data_nonnull=sum(not e["packet"]["data_null"] for e in empty),
                send_zero_size_with_side_data=sum(e["packet"]["side_data_elems"] > 0 for e in empty),
                send_empty_drain_shaped_attempts=len(empty_drain),
                send_empty_drain_shaped_accepted=sum(e["ret"] >= 0 for e in empty_drain),
                send_negative_size_attempts=sum(not e["packet_null"] and e["packet"]["size"] < 0 for e in sends),
                send_return_histogram=histogram(e["ret"] for e in sends),
                receive_attempts=len(receives), receive_successes=len(frames),
                receive_vaapi=sum(e["frame"]["format"] == header["vaapi_format"] for e in frames),
                receive_other_format=sum(e["frame"]["format"] != header["vaapi_format"] for e in frames),
                receive_eagain=sum(e["ret"] == EAGAIN for e in receives), receive_eof=sum(e["ret"] == EOF for e in receives),
                receive_other_errors=sum(e["ret"] < 0 and e["ret"] not in (EAGAIN, EOF) for e in receives),
                frame_format_histogram=histogram(e["frame"]["format"] for e in frames),
                entry_skip_states=[dict(api=k[0],skip_frame=k[1],skip_idct=k[2],skip_loop_filter=k[3],ret=k[4],calls=v)
                                   for k,v in sorted(skip_states.items())],
                exact_demux_stream_association="NOT_OBSERVED", exact_packet_to_frame_or_va_mapping="NOT_OBSERVED"))
    all_h264_candidates = {c["codec_instance"] for c in codecs if c["h264_video_candidate"] and
                           (c["send_data_attempts"] > 0 or c["receive_successes"] > 0)}
    # This bounded case independently requests/reports ff-h264-vaapi. Actual
    # successful frame format separates a software thumbnail decoder without
    # guessing by picture count, timestamps or the largest population.
    codec_candidates = {c["codec_instance"] for c in codecs if c["h264_video_candidate"] and c["receive_vaapi"] > 0}
    reasons = []
    if source["selected_format_instance"] is None:
        reasons.append("No unique public-URL format and H264 video stream selection")
    if len(codec_candidates) != 1:
        reasons.append("No unique H264 decoder lifetime with actually returned VAAPI frames")
    else:
        target = next(iter(codec_candidates))
        selected_epochs = [c for c in codecs if c["codec_instance"] == target and c["h264_video_candidate"]]
        if not any(c["send_payload_accepted"] > 0 for c in selected_epochs) or not any(
                c["receive_successes"] > 0 for c in selected_epochs):
            reasons.append("Selected H264 decoder lacks accepted nonempty payload data or returned frames")
    return dict(status="NOT_QUALIFIED_PACKET_ADMISSION_CENSUS" if reasons else "PASS_SCOPED_PACKET_ADMISSION_CENSUS",
                not_qualified_reasons=reasons, selected_codec_instance=next(iter(codec_candidates)) if len(codec_candidates) == 1 else None,
                decoder_selector=dict(codec_type=header["video_media_type"],codec_id=header["h264_codec_id"],
                                      successful_frame_format=header["vaapi_format"]),
                all_h264_data_candidate_instances=sorted(all_h264_candidates),
                actual_vaapi_candidate_instances=sorted(codec_candidates),
                begin_ns=begin_ns, end_ns=end_ns,
                source_population=source, formats=formats, codecs=codecs,
                source_clock_comparison=False, packet_to_va_identity="NOT_OBSERVED", limits=LIMITS)


def census_case(validated, intervals, expected_video_packets=900, expected_url="/fixture.mp4"):
    require(type(intervals) is dict and bool(intervals), "explicit play windows are required")
    windows = []
    plays = {}
    for key, interval in intervals.items():
        require(type(interval) is dict, "play window must be an object")
        begin, end = interval["begin_ns"], interval["end_ns"]
        plays[str(key)] = census_play(validated, begin, end, expected_video_packets, expected_url)
        windows.append((begin, end))
    ordered = sorted(windows)
    require(all(a[1] <= b[0] for a,b in zip(ordered, ordered[1:])), "play windows overlap")
    return dict(status="PASS_SCOPED_PACKET_ADMISSION_CENSUS" if all(
                p["status"] == "PASS_SCOPED_PACKET_ADMISSION_CENSUS" for p in plays.values()) else
                "NOT_QUALIFIED_PACKET_ADMISSION_CENSUS", plays=plays, limits=LIMITS,
                source_clock_comparison=False, packet_to_va_identity="NOT_OBSERVED")


def audit_report(path, expected_pid, expected_providers=None):
    try:
        valid = read_report(path, expected_pid, expected_providers)
        return dict(status="PASS_SCOPED_PACKET_ADMISSION_OBSERVATION", header=valid["header"],
                    footer=valid["footer"], process_scope=valid["process_scope"],
                    events=len(valid["events"]),format_lifetimes=len(valid["formats"]),
                    codec_lifetimes=len(valid["codecs"]), source_clock_comparison=False,
                    packet_to_va_identity="NOT_OBSERVED", limits=LIMITS)
    except (ValueError, OSError, KeyError, TypeError, OverflowError, UnicodeError) as exc:
        return dict(status="FAIL", reason=str(exc), limits=LIMITS)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report")
    parser.add_argument("--pid", type=int, required=True)
    args = parser.parse_args()
    result = audit_report(args.report, args.pid)
    print(json.dumps(result, sort_keys=True))
    return 1 if result["status"] == "FAIL" else 0


if __name__ == "__main__":
    sys.exit(main())
