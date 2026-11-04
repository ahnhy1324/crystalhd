#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Semantic offline controls for maintained-cost parent/clock evidence."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


_spec=importlib.util.spec_from_file_location(
    "maintained_cost_reader",Path(__file__).with_name("vaapi-maintained-cost-reader.py"))
R=importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(R)
PID=123
DECODE=dict(driver_instance=1,context=2,generation=1,token=0,decode_identity=0,
            submission_ordinal=0,surface=0,owner=0)
FULL=dict(driver_instance=1,context=2,generation=1,token=100,decode_identity=11,
          submission_ordinal=1,surface=3,owner=3)


def event(number,parent,stage,wall,cpu,kind="full",phase="inherited",detail=0,flags=0,result=None,tid=PID):
    binding=DECODE if kind=="decode" else FULL
    return dict(type="event",seq=number-1,id=number,parent_id=parent,pid=PID,tid=tid,end_tid=tid,stage=stage,
        wall_begin_ns=wall[0],wall_end_ns=wall[1],cpu_begin_ns=cpu[0],cpu_end_ns=cpu[1],
        wall_ns=wall[1]-wall[0],cpu_ns=cpu[1]-cpu[0],binding_kind=kind,binding_phase=phase,
        detail=detail,flags=flags,result=result,**binding)


def document(events):
    return [dict(type="header",schema=R.SCHEMA,pid=PID,fork_policy=R.FORK_POLICY,scope=R.SCOPE,
        capacity=65536,clock_names=dict(R.CLOCKS),stages=list(R.STAGES),flags=dict(R.FLAGS)),
        *events,dict(type="footer",events=len(events),attempted_events=len(events),dropped=0,inflight=0,
                    unpublished=0,clock_failures=0,lifecycle_failures=0,fork_seen=False,complete=True)]


def fixture():
    return document([
        event(1,0,"receive_one",(100,1000),(10,610),"decode","entry",result=0),
        event(2,1,"dil_acquire",(150,250),(30,90),"decode",result=0),
        event(3,1,"process_output",(300,800),(120,500),phase="resolved_after_output",result=0),
        event(4,3,"field_convert",(350,500),(160,260),detail=3,result=1),
        event(5,4,"decoded_cpu_begin",(360,380),(170,180),"surface","entry",result=1),
        event(6,4,"field_convert_pixels",(400,460),(200,240),detail=3),
        event(7,4,"decoded_cpu_end",(470,490),(245,255),"surface","entry",result=1),
        event(8,3,"canvas_compose",(550,750),(300,460),flags=2|4|8,result=1),
        event(9,8,"cpu_access_begin",(555,565),(305,310),"surface","entry",detail=1,flags=32,result=1),
        event(10,8,"canvas_pixels",(580,700),(325,420),flags=2|4),
        event(11,8,"diagnostic_capture",(705,720),(425,435),detail=1,flags=8),
        event(12,8,"cpu_access_end",(725,735),(440,445),"surface","entry",detail=1,flags=32,result=1),
        event(13,1,"dil_release",(820,850),(520,535),"decode",result=0)])


class MaintainedCostReaderTests(unittest.TestCase):
    def setUp(self):self.rows=fixture()

    def valid(self):return R.check_document(self.rows,PID)

    def invalid(self,pattern="."):
        with self.assertRaisesRegex(ValueError,pattern):self.valid()

    def census(self):return R.census_case(self.valid(),{"1":dict(begin_ns=1,end_ns=1100)})["plays"]["1"]

    def test_actual_parent_tree_subtracts_only_direct_children(self):
        valid=self.valid();d=valid["durations"]
        self.assertEqual(d[1]["direct_child_ids"],[2,3,13])
        self.assertEqual((d[1]["exclusive_wall_ns"],d[1]["exclusive_cpu_ns"]),(270,145))
        self.assertEqual((d[3]["exclusive_wall_ns"],d[3]["exclusive_cpu_ns"]),(150,120))
        self.assertEqual((d[8]["exclusive_wall_ns"],d[8]["exclusive_cpu_ns"]),(45,45))
        self.assertFalse(valid["native_completion_claim"])

    def test_selected_capture_is_outside_pixels_and_visible_in_inclusive_parent(self):
        valid=self.valid();parent=valid["durations"][8]
        self.assertEqual(parent["direct_child_ids"],[9,10,11,12])
        self.assertLess(self.rows[10]["wall_end_ns"],self.rows[11]["wall_begin_ns"])
        stages=self.census()["stages"]
        self.assertEqual(stages["canvas_pixels"]["metrics"]["inclusive_wall_ns"]["sum_ns"],120)
        self.assertEqual(stages["canvas_compose"]["flag_event_counts"]["selected_capture"],1)
        self.assertEqual(stages["diagnostic_capture"]["events"],1)

    def test_missing_stage_not_reported_as_free_zero_cost(self):
        stage=self.census()["stages"]["public_copy"]
        self.assertEqual(stage["status"],"NOT_OBSERVED")
        self.assertEqual(stage["metrics"]["inclusive_cpu_ns"]["count"],0)
        self.assertIsNone(stage["metrics"]["inclusive_cpu_ns"]["sum_ns"])
        self.assertIsNone(stage["metrics"]["exclusive_offcpu_ns"]["p50_ns"])

    def test_early_return_closes_scope_without_fabricating_missing_inner_work(self):
        self.rows=document([
            event(1,0,"receive_one",(100,200),(10,30),"decode","entry",result=0),
            event(2,1,"dil_acquire",(110,160),(15,25),"decode",result=8)])
        census=self.census()
        self.assertEqual(census["status"],"PASS_SCOPED_MAINTAINED_COST_CENSUS")
        self.assertEqual(census["stages"]["dil_acquire"]["raw_result_event_counts"],{"8":1})
        self.assertEqual(census["stages"]["process_output"]["status"],"NOT_OBSERVED")
        self.assertEqual(census["full_binding_candidates"],[])

    def test_exclusive_offcpu_endpoint_mismatch_is_unavailable_not_clamped(self):
        self.rows=document([
            event(1,0,"receive_one",(100,200),(0,100),"decode","entry"),
            event(2,1,"dil_acquire",(110,190),(20,80),"decode")])
        valid=self.valid();d=valid["durations"][1]
        self.assertEqual((d["exclusive_wall_ns"],d["exclusive_cpu_ns"]),(20,40))
        self.assertIsNone(d["exclusive_offcpu_ns"])
        metric=self.census()["stages"]["receive_one"]["metrics"]["exclusive_offcpu_ns"]
        self.assertEqual(metric["status"],"UNAVAILABLE_ENDPOINT_SUBTRACTION")
        self.assertEqual(metric["negative_sample_count"],1)
        self.assertIsNone(metric["sum_ns"])

    def test_observed_zero_interval_is_distinct_from_absent_stage(self):
        self.rows=document([event(1,0,"sync_body",(100,100),(0,0),"decode","entry")])
        stage=self.census()["stages"]["sync_body"]
        self.assertEqual(stage["status"],"OBSERVED")
        self.assertEqual(stage["metrics"]["inclusive_cpu_ns"]["sum_ns"],0)

    def test_foreign_process_and_end_thread_are_rejected(self):
        for row,key,value in ((self.rows[0],"pid",PID+1),(self.rows[2],"pid",PID+1),
                              (self.rows[2],"end_tid",PID+1)):
            with self.subTest(key=key):
                old=row[key];row[key]=value;self.invalid();row[key]=old

    def test_exact_clock_stage_flag_scope_contract_is_required(self):
        for key,value in (("clock_names",dict(wall="CLOCK_REALTIME",cpu="CLOCK_PROCESS_CPUTIME_ID")),
                          ("scope","NORMAL_BACKEND"),("fork_policy","ALL_CHILDREN"),
                          ("stages",list(R.STAGES[:-1])),("flags",dict(R.FLAGS,read=64))):
            with self.subTest(key=key):
                old=self.rows[0][key];self.rows[0][key]=value;self.invalid();self.rows[0][key]=old

    def test_fork_descriptive_parent_scope_still_rejects_child_pid(self):
        self.rows[-1]["fork_seen"]=True
        self.assertTrue(self.valid()["process_scope"]["fork_seen"])
        self.rows[3]["pid"]+=1
        self.invalid("foreign event PID")

    def test_every_incomplete_publication_gate_rejects(self):
        for key in ("dropped","inflight","unpublished","clock_failures","lifecycle_failures","complete"):
            with self.subTest(key=key):
                old=self.rows[-1][key];self.rows[-1][key]=not old if type(old) is bool else 1
                self.invalid();self.rows[-1][key]=old

    def test_capacity_overflow_and_boolean_counter_reject(self):
        self.rows[0]["capacity"]=2;self.invalid("capacity")
        self.rows=fixture();self.rows[-1]["events"]=True;self.invalid("integer")

    def test_missing_duplicate_or_reordered_scope_sequence_rejected(self):
        self.rows[3]["id"]=1;self.invalid("scope ID")
        self.rows=fixture();self.rows[2],self.rows[3]=self.rows[3],self.rows[2]
        self.invalid("scope ID")

    def test_declared_delta_must_equal_both_actual_endpoint_pairs(self):
        for key in ("wall_ns","cpu_ns"):
            with self.subTest(key=key):
                old=self.rows[4][key];self.rows[4][key]=old+1;self.invalid("clock arithmetic");self.rows[4][key]=old

    def test_short_inclusive_cpu_accounting_above_wall_is_preserved_not_clamped(self):
        self.rows=document([
            event(1,0,"receive_one",(100,5000),(0,6000),"decode","entry"),
            event(2,1,"cpu_access_end",(200,2155),(100,2516),"surface","entry",detail=2,flags=64)])
        valid=self.valid()
        self.assertEqual(valid["by_id"][2]["wall_ns"],1955)
        self.assertEqual(valid["by_id"][2]["cpu_ns"],2416)
        self.assertIsNone(valid["durations"][2]["inclusive_offcpu_ns"])
        stage=R.stage_summary(valid["events"],valid["durations"])["cpu_access_end"]
        metric=stage["metrics"]["inclusive_offcpu_ns"]
        self.assertEqual(metric["status"],"UNAVAILABLE_ENDPOINT_SUBTRACTION")
        self.assertEqual(metric["negative_sample_count"],1)
        self.assertIsNone(metric["sum_ns"])
        self.assertEqual(stage["metrics"]["inclusive_cpu_ns"]["sum_ns"],2416)
        self.assertEqual(stage["metrics"]["inclusive_wall_ns"]["sum_ns"],1955)

    def test_cpu_above_wall_does_not_allow_a_regressing_clock(self):
        self.rows=document([event(1,0,"cpu_access_end",(100,200),(50,40),"surface","entry",detail=2,flags=64)])
        self.invalid("CPU end")

    def test_reversed_clock_and_wrong_clock_type_rejected(self):
        self.rows[2]["cpu_end_ns"]=self.rows[2]["cpu_begin_ns"]-1;self.invalid()
        self.rows=fixture();self.rows[2]["wall_begin_ns"]=True;self.invalid()

    def test_child_must_use_actual_prior_parent_not_itself_or_later_id(self):
        for value in (4,9,999):
            with self.subTest(value=value):
                self.rows[4]["parent_id"]=value;self.invalid()

    def test_parent_from_another_thread_is_not_inferred_by_time_proximity(self):
        self.rows[2]["tid"]=self.rows[2]["end_tid"]=PID+1
        self.invalid("parent identity")

    def test_child_wall_or_cpu_cannot_escape_parent(self):
        self.rows[6].update(wall_begin_ns=340,wall_ns=120)
        self.invalid("child escapes parent wall")
        self.rows=fixture();self.rows[6].update(cpu_begin_ns=150,cpu_end_ns=190,cpu_ns=40)
        self.invalid("child escapes parent cpu")

    def test_direct_sibling_overlap_is_not_double_subtracted(self):
        self.rows[11].update(wall_begin_ns=690,wall_ns=30)
        self.invalid("siblings overlap")

    def test_nonoverlapping_wall_siblings_cannot_regress_thread_cpu(self):
        self.rows[11].update(cpu_begin_ns=415,cpu_end_ns=425,cpu_ns=10)
        self.invalid("CPU clock regresses")

    def test_wrong_root_parent_cannot_hide_a_nested_scope(self):
        self.rows[4]["parent_id"]=0;self.rows[4]["binding_phase"]="entry"
        self.invalid("siblings overlap")

    def test_overlapping_different_threads_are_valid_without_shared_cpu_origin(self):
        extra=event(14,0,"sync_body",(50,1000),(5000,5500),"decode","entry",tid=456)
        self.rows.insert(-1,extra);self.rows[-1].update(events=14,attempted_events=14)
        valid=self.valid()
        self.assertEqual(valid["by_id"][14]["cpu_begin_ns"],5000)
        self.assertEqual(self.census()["stages"]["sync_body"]["tids"],[456])

    def test_none_binding_cannot_claim_context_and_full_cannot_have_unknown_or_invalid_surface(self):
        self.rows[3]["binding_kind"]="none";self.invalid("none binding")
        self.rows=fixture();self.rows[3]["token"]=0;self.invalid("full binding")
        self.rows=fixture();self.rows[3]["surface"]=(1<<32)-1;self.invalid("invalid VA")

    def test_declared_partial_identity_is_never_promoted_to_full(self):
        for e in self.rows[1:-1]:
            e.update(binding_kind="surface",binding_phase="entry",**FULL)
        census=self.census()
        self.assertEqual(census["status"],"PASS_SCOPED_MAINTAINED_COST_CENSUS")
        self.assertEqual(census["full_binding_candidates"],[])
        self.assertEqual(census["full_native_binding_correlation"],"NOT_PERFORMED_BY_COST_READER")

    def test_unknown_surface_identity_remains_unqualified_census(self):
        for e in self.rows[1:-1]:
            e.update(binding_kind="surface",binding_phase="entry",**{k:0 for k in R.BINDING})
        census=self.census()
        self.assertEqual(census["status"],"NOT_QUALIFIED_MAINTAINED_COST_CENSUS")

    def test_bootstrap_surface_resolved_after_admission_is_partial_not_none_or_full(self):
        e=event(1,0,"sync_body",(100,200),(10,50),"surface","resolved_after_admission")
        e.update(context=0,generation=0,token=0,decode_identity=0,submission_ordinal=0,surface=9,owner=9)
        self.rows=document([e])
        valid=self.valid()
        self.assertEqual(valid["by_id"][1]["binding_kind"],"surface")
        census=self.census()
        self.assertEqual(census["full_binding_candidates"],[])
        self.assertEqual(census["status"],"NOT_QUALIFIED_MAINTAINED_COST_CENSUS")

    def test_later_parent_identity_refinement_does_not_upgrade_earlier_child(self):
        self.rows[1].update(binding_kind="full",binding_phase="resolved_after_output",**FULL)
        self.rows[13].update(binding_kind="full",**FULL)
        valid=self.valid()
        self.assertEqual(valid["by_id"][2]["binding_kind"],"decode")
        self.assertEqual(valid["by_id"][2]["token"],0)

    def test_inherited_known_identity_must_agree_even_after_parent_refinement(self):
        self.rows[1].update(binding_kind="full",binding_phase="resolved_after_output",**FULL)
        self.rows[2]["token"]=99
        self.invalid("known scalar conflicts")

    def test_inherited_child_cannot_swap_source_identity(self):
        self.rows[6]["token"]=101;self.invalid("inherited binding")

    def test_explicit_nested_harvest_can_process_another_picture_than_sync_target(self):
        self.rows=document([
            event(1,0,"sync_body",(100,1000),(10,610),phase="resolved_after_admission"),
            event(2,1,"receive_one",(150,800),(30,500),phase="resolved_after_output")])
        self.rows[2].update(token=200,decode_identity=22,submission_ordinal=2,surface=4,owner=4)
        valid=self.valid()
        self.assertEqual(valid["by_id"][1]["token"],100)
        self.assertEqual(valid["by_id"][2]["token"],200)
        self.assertEqual(len(self.census()["full_binding_candidates"]),2)

    def test_explicit_surface_identity_can_have_different_public_target(self):
        self.rows[5].update(surface=9,owner=9)
        self.assertEqual(self.valid()["by_id"][5]["surface"],9)

    def test_branch_direction_details_and_unknown_bits_cannot_be_forged(self):
        for row,key,value in ((9,"flags",64),(9,"detail",0),(4,"detail",4),
                              (11,"detail",4),(1,"detail",1),(8,"flags",128)):
            with self.subTest(row=row,key=key):
                old=self.rows[row][key];self.rows[row][key]=value;self.invalid();self.rows[row][key]=old

    def test_rejected_invalid_field_mask_is_observed_only_with_actual_false_result(self):
        for mask in (0,4):
            with self.subTest(mask=mask):
                # A real rejected path has no conversion-pixel child.
                self.rows=document([
                    event(1,0,"receive_one",(100,1000),(10,610),"decode","entry"),
                    event(2,1,"process_output",(300,800),(120,500),phase="resolved_after_output"),
                    event(3,2,"field_convert",(350,500),(160,260),detail=mask,result=0)])
                self.assertEqual(self.valid()["by_id"][3]["result"],0)
                for result in (1,None,False):
                    self.rows[3]["result"]=result
                    self.invalid("invalid field mask")

    def test_invalid_field_mask_never_qualifies_a_pixel_span_even_with_false_result(self):
        self.rows[6].update(detail=0,result=0)
        self.invalid("field/capture detail")

    def test_raw_unknown_result_is_not_inferred_success(self):
        self.assertIsNone(self.rows[6]["result"])
        c=self.census()["stages"]["field_convert_pixels"]
        self.assertEqual(c["raw_result_event_counts"],{"null":1})
        self.rows[6]["result"]=True;self.invalid("raw integer")

    def test_empty_complete_report_can_be_valid_but_is_not_a_play_census(self):
        self.rows=document([])
        valid=self.valid()
        c=R.census_case(valid,{"1":dict(begin_ns=1,end_ns=100)})
        self.assertEqual(c["status"],"NOT_QUALIFIED_MAINTAINED_COST_CENSUS")
        self.assertEqual(c["plays"]["1"]["stages"]["field_convert"]["status"],"NOT_OBSERVED")

    def test_strict_play_windows_reject_overlap_and_partial_parent(self):
        valid=self.valid()
        with self.assertRaisesRegex(ValueError,"windows overlap"):
            R.census_case(valid,{"1":dict(begin_ns=1,end_ns=1100),"2":dict(begin_ns=500,end_ns=1200)})
        with self.assertRaisesRegex(ValueError,"cuts through"):
            R.census_case(valid,{"1":dict(begin_ns=120,end_ns=950)})

    def test_partial_json_duplicate_keys_and_stage_table_duplicate_rejected(self):
        for raw in (b'{"type":"header","type":"footer"}\n{}\n',b'{}\n{}'):
            with self.assertRaises(ValueError):R.parse_report(raw)
        self.rows[0]["stages"][-1]=self.rows[0]["stages"][0];self.invalid("stage contract")

    def test_file_audit_refuses_symlink_and_remains_explicit_diagnostic_scope(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/f"maintained-cost-{PID}.jsonl"
            path.write_text("".join(json.dumps(r)+"\n" for r in self.rows))
            result=R.audit_report(path,PID)
            self.assertEqual(result["status"],"PASS_SCOPED_MAINTAINED_COST_OBSERVATION")
            self.assertFalse(result["native_completion_claim"])
            alias=Path(directory)/f"alias-{PID}.jsonl";alias.symlink_to(path)
            self.assertEqual(R.audit_report(alias,PID)["status"],"FAIL")


if __name__=="__main__":
    unittest.main()
