#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Device-free distinguishing controls for OWN packet-admission evidence."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


_spec = importlib.util.spec_from_file_location(
    "packet_reader", Path(__file__).with_name("vaapi-packet-admission-reader.py"))
R = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(R)
PID = 123
H264, VIDEO, VAAPI = 27, 0, 44


class Builder:
    def __init__(self):
        self.header = dict(type="header", schema=R.SCHEMA, pid=PID, fork_policy=R.FORK_POLICY,
            observation_scope=R.SCOPE, capacity=32768, lifecycle_capacity=256, max_streams=1024,
            provider_ok=True, h264_codec_id=H264, video_media_type=VIDEO, vaapi_format=VAAPI,
            discard_none=-16, discard_default=0, discard_nonref=8)
        for family in R.PROVIDERS:
            self.header[family+"_provider"] = R.PROVIDERS[family]
            self.header[family+"_version"] = R.VERSIONS[family]
        self.events=[]
        self.format_open=False
        self.codec_open=False
        self.info=False
        self.epoch=1
        self.skip=0

    def codec(self):
        return dict(codec_type=VIDEO,codec_id=H264,width=1920,height=1080,pix_fmt=VAAPI,
                    skip_frame=self.skip,skip_idct=self.skip,skip_loop_filter=self.skip,
                    pkt_timebase=[0,1],thread_count=1,thread_type=0,active_thread_type=0,has_b_frames=2)

    def push(self,api,**extra):
        seq=len(self.events)
        e=dict(type="event",seq=seq,pid=PID,tid=PID+1 if api in R.FORMAT_APIS else PID+2,
            begin_ns=100+10*seq,end_ns=105+10*seq,api=api,ret=None if api in R.VOID_APIS else 0,
            errno_before=17,errno_after=18,format_instance=0,codec_instance=0,codec_epoch=0,
            format_context="0x0",codec_context="0x0",caller="0x800",concurrent=False,
            opened_before=False,unopened_free=False,flush_old_epoch=0,flush_new_epoch=0,
            packet_null=False,input_url=None,format_url=None,format=None,codec=None,stream=None,packet=None,frame=None)
        if api in R.FORMAT_APIS:
            e.update(format_instance=1,format_context="0x100",opened_before=self.format_open,
                     format=dict(start_time=45400,nb_streams=2,info_complete=self.info))
            if api==R.FOPEN:
                e.update(input_url="/fixture.mp4",format_url="/fixture.mp4")
        else:
            e.update(codec_instance=1,codec_context="0x200",codec_epoch=self.epoch,
                     opened_before=self.codec_open,codec=self.codec())
        if api==R.FLUSH:
            e.update(flush_old_epoch=self.epoch,flush_new_epoch=self.epoch+1)
        e.update(extra)
        self.events.append(e)
        if api==R.FOPEN and e["ret"]>=0:self.format_open=True
        if api==R.COPEN and e["ret"]>=0:self.codec_open=True
        if api==R.INFO and e["ret"]>=0:self.info=True
        if api==R.FLUSH:self.epoch+=1
        if api==R.CFREE:self.codec_open=False
        if api==R.FCLOSE:self.format_open=False
        return e

    def document(self):
        return [self.header,*self.events,dict(type="footer",events=len(self.events),
            attempted_events=len(self.events),dropped=0,lifecycle_failures=0,string_truncations=0,
            clock_failure=False,fork_seen=False,inflight=0,unpublished=0,open_formats=0,open_codecs=0,complete=True)]


def packet(index=0,send=False):
    # Deliberately distinct clocks and default-like repackaged send metadata.
    return dict(pts=900000+index if send else 6006+3003*index,
        dts=800000+index if send else 3003*index,duration=0 if send else 3003,
        pos=-1 if send else index*188,size=100,flags=0 if send else 1,
        stream_index=0,time_base=[0,1],data_null=False,side_data_elems=0)


def stream():
    return dict(index=0,id=256,codec_type=VIDEO,codec_id=H264,width=1920,height=1080,
                time_base=[1,90000],start_time=6006,duration=2702700)


def frame(index=0):
    return dict(pts=7000000+index,best_effort_timestamp=7000000+index,duration=0,time_base=[0,1],
                format=VAAPI,width=1920,height=1080,flags=24,va_surface=100+index)


def fixture(packets=3,returned=None,before_info_reads=0,flush=False,find_info=True):
    b=Builder()
    b.push(R.FOPEN)
    for i in range(before_info_reads):b.push(R.READ,packet=packet(i),stream=stream())
    if find_info:b.push(R.INFO)
    b.push(R.COPEN)
    if flush:b.push(R.FLUSH)
    returned=packets if returned is None else returned
    for i in range(packets):
        b.push(R.READ,packet=packet(i),stream=stream())
        b.skip=0 if i==0 else 8
        b.push(R.SEND,packet=packet(i,True))
        b.push(R.RECEIVE,**(dict(frame=frame(i)) if i<returned else dict(ret=R.EAGAIN)))
    b.push(R.READ,ret=R.EOF)
    b.push(R.SEND,packet_null=True)
    b.push(R.RECEIVE,ret=R.EOF)
    b.push(R.CFREE)
    b.push(R.FCLOSE)
    return b.document()


def renumber(rows):
    for seq,e in enumerate(rows[1:-1]):
        e.update(seq=seq,begin_ns=100+10*seq,end_ns=105+10*seq)
    rows[-1].update(events=len(rows)-2,attempted_events=len(rows)-2)


def combine(first,second):
    latter=copy.deepcopy(second[1:-1])
    for e in latter:
        if e["format_instance"]:e["format_instance"]+=1
        if e["codec_instance"]:e["codec_instance"]+=1
    result=[copy.deepcopy(first[0]),*copy.deepcopy(first[1:-1]),*latter,copy.deepcopy(first[-1])]
    renumber(result)
    return result


class PacketReaderTests(unittest.TestCase):
    def setUp(self):
        self.rows=fixture()

    def valid(self):
        return R.check_document(self.rows,PID)

    def invalid(self,pattern="."):
        with self.assertRaisesRegex(ValueError,pattern):self.valid()

    def census(self,expected=3):
        valid=self.valid()
        return R.census_play(valid,1,valid["events"][-1]["end_ns"]+1,expected_video_packets=expected)

    def api(self,name):
        return next(e for e in self.rows[1:-1] if e["api"]==name)

    def test_complete_900_demux_and_fewer_returns_are_distinct_populations(self):
        self.rows=fixture(900,687)
        census=self.census(900)
        self.assertEqual(census["status"],"PASS_SCOPED_PACKET_ADMISSION_CENSUS")
        self.assertEqual(census["source_population"]["status"],"PASS_EXACT_VIDEO_DEMUX_POPULATION")
        codec=census["codecs"][0]
        self.assertEqual(codec["send_data_accepted"],900)
        self.assertEqual(codec["receive_vaapi"],687)
        self.assertEqual(codec["receive_eagain"],213)
        self.assertEqual(codec["exact_packet_to_frame_or_va_mapping"],"NOT_OBSERVED")
        self.assertFalse(census["source_clock_comparison"])

    def test_whole_900_requirement_is_not_lowered_by_valid_observation(self):
        census=self.census(900)
        self.assertEqual(census["status"],"PASS_SCOPED_PACKET_ADMISSION_CENSUS")
        self.assertEqual(census["source_population"]["observed_video_packets"],3)
        self.assertEqual(census["source_population"]["status"],"FAIL_VIDEO_DEMUX_POPULATION")

    def test_outer_reads_before_info_are_not_mislabeled_discovery(self):
        self.rows=fixture(before_info_reads=2)
        census=self.census(5)
        fmt=census["formats"][0]
        self.assertEqual(fmt["h264_video_before_observed_info_reads"],2)
        self.assertEqual(fmt["h264_video_after_observed_info_reads"],3)
        self.assertEqual(census["source_population"]["observed_video_packets"],5)
        self.assertEqual(fmt["nested_discovery_calls"],"EXCLUDED_BY_OUTERMOST_OBSERVER")

    def test_transport_playback_without_find_info_is_valid_outer_population(self):
        self.rows=fixture(find_info=False)
        census=self.census()
        self.assertEqual(census["status"],"PASS_SCOPED_PACKET_ADMISSION_CENSUS")
        self.assertEqual(census["source_population"]["status"],"PASS_EXACT_VIDEO_DEMUX_POPULATION")
        self.assertEqual(census["formats"][0]["h264_video_before_observed_info_reads"],3)
        self.assertEqual(census["formats"][0]["successful_find_stream_info"],0)

    def test_same_url_zero_read_probe_does_not_replace_positive_read_source(self):
        self.rows=combine(fixture(0),fixture(find_info=False))
        census=self.census()
        self.assertEqual(census["status"],"PASS_SCOPED_PACKET_ADMISSION_CENSUS")
        self.assertEqual(census["source_population"]["selected_format_instance"],2)
        self.assertEqual(census["formats"][0]["h264_video_outer_reads"],0)

    def test_missing_and_duplicate_sequence_rejected(self):
        self.rows[3]["seq"]=self.rows[2]["seq"]
        self.invalid("sequence")
        self.rows=fixture()
        del self.rows[4]
        self.rows[-1]["events"]-=1
        self.rows[-1]["attempted_events"]-=1
        self.invalid("sequence")

    def test_reordered_ownership_records_rejected(self):
        self.rows[4],self.rows[5]=self.rows[5],self.rows[4]
        self.invalid("sequence")

    def test_provider_version_pid_and_scope_fail_closed(self):
        for key,value in (("format_provider","/other/libavformat.so.63.1.102"),
                          ("codec_version",R.VERSIONS["codec"]-1),("util_version",R.VERSIONS["util"]-1),
                          ("provider_ok",False),("pid",PID+1),("observation_scope","ALL_THREADS_AND_CHILDREN"),
                          ("fork_policy","UNKNOWN")):
            with self.subTest(key=key):
                old=self.rows[0][key];self.rows[0][key]=value;self.invalid();self.rows[0][key]=old

    def test_fork_is_descriptive_but_foreign_child_event_rejected(self):
        self.rows[-1]["fork_seen"]=True
        self.assertTrue(self.valid()["process_scope"]["fork_seen"])
        self.rows[4]["pid"]=PID+1
        self.invalid("foreign event PID")

    def test_every_incomplete_footer_state_rejected(self):
        for key in ("dropped","lifecycle_failures","string_truncations","inflight","unpublished",
                    "open_formats","open_codecs","clock_failure","complete"):
            with self.subTest(key=key):
                old=self.rows[-1][key];self.rows[-1][key]=not old if type(old) is bool else 1
                self.invalid();self.rows[-1][key]=old

    def test_capacity_and_boolean_counter_do_not_hide_overflow(self):
        self.rows[0]["capacity"]=2
        self.invalid("capacity")
        self.rows=fixture();self.rows[-1]["events"]=True
        self.invalid("integer")

    def test_known_data_context_requires_successful_open(self):
        e=self.api(R.SEND)
        e.update(codec_instance=0,codec_epoch=0,opened_before=False)
        self.invalid("unknown lifetime")

    def test_retirement_is_required_even_if_footer_claims_complete(self):
        self.rows.remove(self.api(R.FCLOSE));renumber(self.rows)
        self.invalid("retirement")

    def test_call_after_free_is_not_valid_reuse(self):
        e=copy.deepcopy(self.api(R.SEND))
        self.rows.insert(-1,e);renumber(self.rows)
        self.invalid("after retirement")

    def test_opened_before_is_checked_on_each_call(self):
        self.api(R.READ)["opened_before"]=False
        self.invalid("opened_before")

    def test_overlapping_calls_on_same_context_rejected_without_probe_flag(self):
        first=self.api(R.SEND)
        second=next(e for e in self.rows[1:-1] if e["api"]==R.RECEIVE)
        second["tid"]=PID+3
        second["begin_ns"]=first["begin_ns"]+1
        self.invalid("overlapping calls on one context")

    def test_concurrent_marker_rejects_even_without_interval_overlap(self):
        self.api(R.SEND)["concurrent"]=True
        self.invalid("concurrent")

    def test_same_thread_clock_regression_rejected(self):
        self.api(R.INFO)["begin_ns"]=self.api(R.FOPEN)["begin_ns"]
        self.invalid("same-thread")

    def test_successful_address_reuse_gets_new_ids_beyond_slot_capacity(self):
        self.rows=combine(fixture(),fixture())
        self.rows[0]["lifecycle_capacity"]=1
        valid=self.valid()
        self.assertEqual([x["instance"] for x in valid["formats"]],[1,2])
        self.assertEqual([x["instance"] for x in valid["codecs"]],[1,2])

    def test_open_address_cannot_be_reused_before_retirement(self):
        opening=copy.deepcopy(self.api(R.FOPEN));opening["format_instance"]=2
        closing=copy.deepcopy(self.api(R.FCLOSE));closing["format_instance"]=2
        closing["format"]["info_complete"]=False
        self.rows.insert(2,opening);self.rows.insert(-1,closing);renumber(self.rows)
        self.invalid("address reused")

    def test_failed_open_and_unopened_free_do_not_invent_lifetimes(self):
        failed=copy.deepcopy(self.api(R.COPEN))
        failed.update(ret=-22,codec_instance=0,codec_epoch=0,codec_context="0x999",opened_before=False)
        free=copy.deepcopy(self.api(R.CFREE))
        free.update(codec_instance=0,codec_epoch=0,codec_context="0x999",opened_before=False,unopened_free=True,codec=None)
        self.rows[-1:-1]=[failed,free];renumber(self.rows)
        self.assertEqual(len(self.valid()["codecs"]),1)

    def test_repeat_open_preserves_lifetime_and_entry_state(self):
        opening=copy.deepcopy(self.api(R.FOPEN))
        opening["opened_before"]=True;opening["format"]["info_complete"]=True
        self.rows.insert(-2,opening);renumber(self.rows)
        self.assertEqual(len(self.valid()["formats"]),1)

    def test_flush_creates_new_epoch_without_new_context(self):
        self.rows=fixture(flush=True)
        valid=self.valid()
        self.assertEqual([x["epoch"] for x in valid["codecs"][0]["epochs"]],[1,2])
        self.assertEqual(self.census()["codecs"][0]["codec_epoch"],2)

    def test_unobserved_or_wrong_flush_transition_rejected(self):
        self.api(R.SEND)["codec_epoch"]=2
        self.invalid("epoch")
        self.rows=fixture(flush=True)
        self.api(R.FLUSH)["flush_new_epoch"]=3
        self.invalid("flush transition")

    def test_failed_find_info_does_not_complete_discovery(self):
        self.api(R.INFO)["ret"]=-22
        self.invalid("info completion")

    def test_read_packet_requires_valid_matching_stream_bounds(self):
        for change in ("index","bounds","stream","packet"):
            with self.subTest(change=change):
                saved=copy.deepcopy(self.rows);e=self.api(R.READ)
                if change=="index":e["stream"]["index"]=1
                elif change=="bounds":e["format"]["nb_streams"]=0
                elif change=="stream":e["stream"]=None
                else:e["packet"]=None
                self.invalid();self.rows=saved

    def test_unsuccessful_read_must_not_publish_packet_or_stream(self):
        self.api(R.READ)["ret"]=R.EAGAIN
        self.invalid("ownership/result")

    def test_unsuccessful_receive_has_no_frame_and_software_has_no_va_surface(self):
        self.api(R.RECEIVE)["ret"]=R.EAGAIN
        self.invalid("ownership/result")
        self.rows=fixture();self.api(R.RECEIVE)["frame"]["format"]=0
        self.invalid("software frame claims")

    def test_receive_formats_counted_without_fabricating_va_ownership(self):
        e=self.api(R.RECEIVE);e["frame"].update(format=0,va_surface=None)
        codec=self.census()["codecs"][0]
        self.assertEqual(codec["receive_vaapi"],2)
        self.assertEqual(codec["receive_other_format"],1)

    def test_forged_header_enum_cannot_reclassify_frames(self):
        self.rows[0]["vaapi_format"]=99
        for e in self.rows[1:-1]:
            if e["frame"] is not None:e["frame"]["format"]=99
        self.invalid("enum identity")

    def test_all_six_original_header_enum_identities_are_pinned(self):
        for key in R.ENUMS:
            with self.subTest(key=key):
                old=self.rows[0][key];self.rows[0][key]=old+32
                self.invalid("enum identity")
                self.rows[0][key]=old

    def test_send_eagain_is_attempt_not_another_accepted_packet(self):
        retry=copy.deepcopy(self.api(R.SEND));retry["ret"]=R.EAGAIN
        self.rows.insert(self.rows.index(self.api(R.SEND)),retry);renumber(self.rows)
        codec=self.census()["codecs"][0]
        self.assertEqual(codec["send_data_attempts"],4)
        self.assertEqual(codec["send_data_accepted"],3)
        self.assertEqual(codec["send_data_eagain"],1)

    def test_null_empty_side_data_and_invalid_zero_data_are_distinct(self):
        template=self.api(R.SEND)
        inserts=[]
        for data_null,side_data,ret in ((True,0,0),(True,1,0),(False,0,-22)):
            e=copy.deepcopy(template);e["packet"].update(size=0,data_null=data_null,side_data_elems=side_data);e["ret"]=ret
            inserts.append(e)
        self.rows[5:5]=inserts;renumber(self.rows)
        codec=self.census()["codecs"][0]
        self.assertEqual(codec["send_null_attempts"],1)
        self.assertEqual(codec["send_empty_drain_shaped_attempts"],1)
        self.assertEqual(codec["send_zero_size_with_side_data"],1)
        self.assertEqual(codec["send_zero_size_data_nonnull"],1)
        self.assertEqual(codec["send_data_accepted"],4)
        self.assertEqual(codec["send_data_other_errors"],1)

    def test_entry_skip_states_preserve_all_observed_values_and_results(self):
        c=self.census()["codecs"][0]
        states=c["entry_skip_states"]
        # Two data sends and the actual NULL send share this caller-entry state.
        self.assertTrue(any(x["api"]==R.SEND and x["skip_frame"]==8 and x["calls"]==3 for x in states))
        self.assertTrue(any(x["api"]==R.SEND and x["skip_frame"]==0 and x["calls"]==1 for x in states))
        self.assertEqual(c["exact_demux_stream_association"],"NOT_OBSERVED")

    def test_missing_or_conflicting_url_keeps_census_unqualified(self):
        for values in ((None,None),("/fixture.mp4","/other.mp4")):
            with self.subTest(values=values):
                e=self.api(R.FOPEN);e["input_url"],e["format_url"]=values
                census=self.census()
                self.assertEqual(census["status"],"NOT_QUALIFIED_PACKET_ADMISSION_CENSUS")
                self.assertIsNone(census["source_population"]["observed_video_packets"])

    def test_multiple_populations_never_select_largest_or_exact_count(self):
        self.rows=combine(fixture(3),fixture(7))
        census=self.census(7)
        self.assertEqual(census["status"],"NOT_QUALIFIED_PACKET_ADMISSION_CENSUS")
        self.assertIsNone(census["source_population"]["selected_format_instance"])

    def test_missing_codec_data_is_not_a_scoped_census_pass(self):
        for e in self.rows[1:-1]:
            if e["api"]==R.RECEIVE and e["ret"]>=0:e.update(ret=R.EAGAIN,frame=None)
        self.assertEqual(self.census()["status"],"NOT_QUALIFIED_PACKET_ADMISSION_CENSUS")

    def test_vaapi_result_without_accepted_nonempty_packet_data_is_unqualified(self):
        for e in self.rows[1:-1]:
            if e["api"]==R.SEND and not e["packet_null"]:
                e["packet"].update(size=0,data_null=True,side_data_elems=1)
        census=self.census()
        self.assertEqual(census["status"],"NOT_QUALIFIED_PACKET_ADMISSION_CENSUS")
        self.assertEqual(census["codecs"][0]["send_payload_accepted"],0)
        self.assertEqual(census["codecs"][0]["send_data_accepted"],3)

    def test_actual_vaapi_selector_keeps_software_thumbnail_separate_but_rejects_two_vaapi_lifetimes(self):
        baseline=copy.deepcopy(self.rows)
        for output_format in (0,VAAPI):
            with self.subTest(output_format=output_format):
                self.rows=copy.deepcopy(baseline)
                extra=[copy.deepcopy(self.api(api)) for api in (R.COPEN,R.SEND,R.RECEIVE,R.CFREE)]
                for e in extra:
                    e.update(codec_instance=2,codec_context="0x300")
                extra[2]["frame"].update(format=output_format,va_surface=None if output_format==0 else 301)
                self.rows[-2:-2]=extra;renumber(self.rows)
                census=self.census()
                self.assertEqual(census["all_h264_data_candidate_instances"],[1,2])
                self.assertEqual(census["codecs"][-1]["exact_demux_stream_association"],"NOT_OBSERVED")
                if output_format==0:
                    self.assertEqual(census["status"],"PASS_SCOPED_PACKET_ADMISSION_CENSUS")
                    self.assertEqual(census["selected_codec_instance"],1)
                else:
                    self.assertEqual(census["status"],"NOT_QUALIFIED_PACKET_ADMISSION_CENSUS")
                    self.assertIsNone(census["selected_codec_instance"])

    def test_dynamic_stream_duration_does_not_create_fake_second_stream(self):
        reads=[e for e in self.rows[1:-1] if e["api"]==R.READ and e["ret"]>=0]
        reads[-1]["stream"]["duration"]+=3003
        census=self.census()
        self.assertEqual(census["source_population"]["status"],"PASS_EXACT_VIDEO_DEMUX_POPULATION")
        self.assertEqual(len(census["formats"][0]["video_stream_variants"]),2)

    def test_absent_original_read_eof_is_not_synthesized(self):
        for e in self.rows[1:-1]:
            if e["api"]==R.READ and e["ret"]==R.EOF:e["ret"]=R.EAGAIN
        self.assertEqual(self.census()["source_population"]["status"],"NOT_QUALIFIED_ORIGINAL_READ_EOF_ABSENT")

    def test_raw_unset_clocks_and_repackaged_metadata_remain_unjoined(self):
        valid=self.valid()
        send=next(e for e in valid["events"] if e["api"]==R.SEND)
        self.assertEqual(send["packet"]["time_base"],[0,1])
        self.assertEqual(send["packet"]["pos"],-1)
        self.assertEqual(send["packet"]["flags"],0)
        self.assertEqual(self.api(R.READ)["stream"]["time_base"],[1,90000])
        self.assertEqual(valid["packet_to_va_identity"],"NOT_OBSERVED")
        self.assertFalse(valid["source_clock_comparison"])

    def test_case_windows_are_explicit_nonoverlapping_and_do_not_cut_calls(self):
        valid=self.valid();end=valid["events"][-1]["end_ns"]+1
        with self.assertRaisesRegex(ValueError,"cuts through"):
            R.census_play(valid,valid["events"][0]["begin_ns"]+1,end,3)
        with self.assertRaisesRegex(ValueError,"overlap"):
            R.census_case(valid,{"1":dict(begin_ns=1,end_ns=end),"2":dict(begin_ns=1,end_ns=end)},3)

    def test_json_duplicate_keys_and_partial_publication_rejected(self):
        for raw in (b'{"type":"header","type":"footer"}\n{}\n',b'{}\n{}'):
            with self.assertRaises(ValueError):R.parse_report(raw)

    def test_file_audit_is_scoped_and_refuses_terminal_symlinks(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/f"packet-admission-{PID}.jsonl"
            path.write_text("".join(json.dumps(r)+"\n" for r in self.rows))
            result=R.audit_report(path,PID)
            self.assertEqual(result["status"],"PASS_SCOPED_PACKET_ADMISSION_OBSERVATION")
            self.assertEqual(result["packet_to_va_identity"],"NOT_OBSERVED")
            link=Path(directory)/f"alias-{PID}.jsonl";link.symlink_to(path)
            self.assertEqual(R.audit_report(link,PID)["status"],"FAIL")


if __name__=="__main__":
    unittest.main()
