#!/usr/bin/env python3
"""Offline bounded filter matrix; explicit local manifest, no fetch/build/device."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
READER = HERE / "mpeg1-chroma-pixels.py"
FILTERS = {"bicubic":4, "point":16, "bilinear":2, "lanczos":512}
RUNTIMES = ("pre0a41", "post6507")
FIXTURES = ("horizontal30", "checker90")
SITINGS = ("auto", "left", "center")
COMPONENTS = ("Y", "Cb", "Cr")
REVISIONS = {"pre0a41":("0a41fef0fa5a16c4b9830f7ae87f94cfe52e8b4e","98a5153bb2c55b69e1059b42747cff77324a94a6"),
    "post6507":("6507dc36e542df5fb1c0b8b394f56297ef50385e","e8faa09268e130840aedbbe4dadc7d311886284d")}
STAT_KEYS = ("st_dev","st_ino","st_size","st_mtime_ns","st_ctime_ns")
METRICS = ("samples","exact","maximum_absolute_error","absolute_error_sum","mean_absolute_error",
    "changed_samples","changed_fraction","absolute_error_histogram","signed_error_histogram_candidate_minus_reference",
    "reference_edge_samples","changed_samples_on_reference_edges","changed_samples_off_reference_edges",
    "edge_changed_fraction","changed_sample_edge_fraction","edge_mean_absolute_error")

def save(path, value):
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")

def pin(path):
    path = Path(path).resolve(strict=True)
    before, sha = path.stat(), hashlib.sha256()
    if not path.is_file():
        raise ValueError("nonregular input")
    with path.open("rb") as stream:
        for block in iter(lambda:stream.read(1024*1024),b""):
            sha.update(block)
    after = path.stat()
    if any(getattr(before,k)!=getattr(after,k) for k in STAT_KEYS):
        raise ValueError("file changed while hashing")
    return {"path":str(path),"bytes":after.st_size,"sha256":sha.hexdigest(),
            "stat":{k:getattr(after,k) for k in STAT_KEYS}}

def stable(record, content=False):
    current = Path(record["path"]).stat()
    if any(getattr(current,k)!=v for k,v in record["stat"].items()):
        raise ValueError("frozen metadata changed")
    if content and pin(record["path"])["sha256"]!=record["sha256"]:
        raise ValueError("frozen bytes changed")

class Matrix:
    def __init__(self,args):
        self.args,self.out = args,Path(args.output).resolve()
        self.start = self.progress = time.monotonic()
        self.stage,self.case = "freeze",None
        self.pins,self.generated = {},{}
        self.rows = {"identities":[],"native":[],"pre_post":[],"old711_post_bicubic_hashes":[]}
        self.counts = {"reader_controls":0,"identities":0,"conversions":0,"pre_post":0,"commands":0}

    def location(self,value):
        path = Path(value)
        return path if path.is_absolute() else Path(self.args.manifest).resolve().parent/path

    def freeze_file(self,label,entry):
        record = pin(self.location(entry["path"]))
        if ("sha256" in entry and record["sha256"]!=entry["sha256"]) or ("bytes" in entry and record["bytes"]!=entry["bytes"]):
            raise ValueError("manifest hash/extent mismatch: "+label)
        self.pins[label] = record
        return record

    def freeze(self):
        m = self.manifest = json.loads(Path(self.args.manifest).read_text())
        if m.get("schema")!="crystalhd.issue529.filter-manifest.v1":
            raise ValueError("unsupported explicit manifest")
        self.filters = m["filters"]
        if (not self.filters or len({f["name"] for f in self.filters})!=len(self.filters) or
            any(FILTERS.get(f["name"])!=f["flags"] for f in self.filters)):
            raise ValueError("filters outside unique immutable allowlist")
        if m.get("sitings")!=list(SITINGS) or set(m["fixtures"])!=set(FIXTURES) or set(m["providers"])!=set(RUNTIMES):
            raise ValueError("bounded runtime/fixture/siting contract differs")
        self.frames = {"horizontal30":30,"checker90":90}
        for fixture in FIXTURES:
            row = m["fixtures"][fixture]
            if row["frames"]!=self.frames[fixture] or row["geometry"]!=[352,288]:
                raise ValueError("fixture dimensions/population differ")
            for kind in ("ES","I420","native_YUYV"):
                record = self.freeze_file(fixture+"/"+kind,row[kind])
                size = self.frames[fixture]*(152064 if kind=="I420" else 202752)
                if kind!="ES" and record["bytes"]!=size:
                    raise ValueError("raw active extent differs")
        s = self.freeze_file("build-summary",{"path":m["source_proof"]["summary"]})
        p = self.freeze_file("provider-plan",{"path":m["source_proof"]["provider_plan"]})
        summary,plan = json.loads(Path(s["path"]).read_text()),json.loads(Path(p["path"]).read_text())
        if (summary.get("status")!="PRE02_ADOPTED_POST03_BUILT_SOURCE_UNCHANGED_RUNTIME_NOT_YET_QUALIFIED" or
            summary.get("first_failure") is not None or summary.get("secondary_final_source_failures")):
            raise ValueError("paired unchanged source build not qualified")
        self.builds = {r["label"]:r for r in summary["builds"]}
        for runtime in RUNTIMES:
            row,declared = self.builds[runtime],m["providers"][runtime]
            if ((row["commit"],row["tree"])!=REVISIONS[runtime] or not row.get("source_unchanged_after_build") or
                not row.get("final_source_unchanged") or declared["scale_version"]!="10.2.100" or declared["util_version"]!="61.10.100"):
                raise ValueError("source/API qualification differs")
            for kind in ("scale","util"):
                actual = self.freeze_file(runtime+"/"+kind,declared[kind])
                prior = row["libraries"][kind]
                if actual["sha256"]!=prior["sha256"] or actual["path"]!=prior["path"] or plan["providers"][runtime][kind]!=prior:
                    raise ValueError("paired DSO provenance differs")
            for name in ("source-before.json","source-final.json","configuration-qualified.json"):
                self.freeze_file(runtime+"/"+name,{"path":str(Path(row["evidence"])/name)})
        for label,path in (("manifest",self.args.manifest),("runner",__file__),("reader",READER),
            ("converter-source",HERE/"mpeg1-chroma-convert.c"),("converter",self.args.converter),("python",sys.executable)):
            self.freeze_file(label,{"path":str(Path(path).resolve())})
        for n,entry in enumerate(m.get("preserve",[])):
            self.freeze_file(f"preserve-{n}",entry)
        for n,case in enumerate(m.get("bicubic_cross_hash_cases",[])):
            for side in ("old711","post6507"):
                self.freeze_file(f"bicubic-{n}/{side}",case[side])
        self.env = os.environ.copy()
        for key in ("LD_PRELOAD","LD_AUDIT","LD_LIBRARY_PATH","PYTHONPATH"):
            self.env.pop(key,None)
        if m.get("pythonpath"):
            directory = self.location(m["pythonpath"]).resolve(strict=True)
            if not (directory/"numpy/__init__.py").is_file():
                raise ValueError("private NumPy provider missing")
            self.env["PYTHONPATH"] = str(directory)
            files = sorted((directory/"numpy").rglob("*.py"))+sorted((directory/"numpy").rglob("*.pyc"))+sorted((directory/"numpy").rglob("*.so"))
            for n,path in enumerate(files):
                self.freeze_file(f"numpy-{n}",{"path":str(path)})
        self.env.update(PYTHONNOUSERSITE="1",PYTHONDONTWRITEBYTECODE="1",OMP_NUM_THREADS="1",OPENBLAS_NUM_THREADS="1",MKL_NUM_THREADS="1")
        save(self.out/"frozen-pins.json",{"pins":self.pins,"filters":self.filters,"raw_order_preserved":True,"retry":False})

    def guard(self):
        for entry in list(self.pins.values())+list(self.generated.values()):
            stable(entry)
        for label in ("runner","reader","converter","converter-source"):
            stable(self.pins[label],content=True)

    def run(self,name,argv):
        self.case = name
        self.guard()
        save(self.out/(name+".command.json"),{"argv":[str(a) for a in argv],"stage":self.stage,"timeout":180,"retry":False})
        start,status,error = time.monotonic(),None,None
        logfile = self.out/(name+".log")
        with logfile.open("xb") as log:
            try:
                result = subprocess.run([str(a) for a in argv],env=self.env,stdin=subprocess.DEVNULL,
                    stdout=log,stderr=subprocess.STDOUT,timeout=180,check=False)
                status = result.returncode
            except Exception as caught:
                error = {"type":type(caught).__name__,"message":str(caught)}
        self.counts["commands"] += 1
        save(self.out/(name+".process.json"),{"returncode":status,"error":error,"elapsed_seconds":time.monotonic()-start})
        if status!=0 or error:
            raise RuntimeError("program failure retained without retry: "+name)
        self.guard()
        if time.monotonic()-self.progress>=20:
            print(json.dumps({"stage":self.stage,"case":name,"counts":self.counts}),flush=True)
            self.progress = time.monotonic()
        return logfile

    def convert(self,name,runtime,fixture,flags,mode,src,dst):
        declared = self.manifest["providers"][runtime]
        kind = "native_YUYV" if mode=="yuyv-yuyv" else "I420"
        output = self.out/(name+(".i420" if mode=="i420-i420" else ".yuyv"))
        receipt = self.out/(name+".conversion.json")
        self.run(name+"-convert",[self.pins["converter"]["path"],runtime,self.pins[runtime+"/scale"]["path"],
            self.pins[runtime+"/util"]["path"],declared["scale_version"],declared["util_version"],str(flags),
            mode,src,dst,str(self.frames[fixture]),self.pins[fixture+"/"+kind]["path"],output,receipt])
        data = json.loads(receipt.read_text())
        if (data["status"]!="CONVERSION_COMPLETE" or not data["providers_verified"] or not data["color_verified"] or
            data["completed_frames"]!=self.frames[fixture] or data["identity_changed_frames"] or
            data["declared_flags_integer"]!=flags or data["scale_version_int"]!=((10<<16)|(2<<8)|100) or
            data["util_version_int"]!=((61<<16)|(10<<8)|100)):
            raise ValueError("conversion/full API/filter qualification differs")
        allowed = {self.pins[runtime+"/scale"]["path"],self.pins[runtime+"/util"]["path"]}
        if not data["symbols"] or any(s["dladdr_provider_realpath"] not in allowed for s in data["symbols"]):
            raise ValueError("unexpected dynamic provider")
        options = {r["name"]:r for r in data["options"]}
        for key,value in (("sws_flags",flags),("src_range",0),("dst_range",0),("threads",1)):
            if any(options[key][when]!=value for when in ("requested","after_init","after_scale")):
                raise ValueError("declared filter/range/thread option changed")
        self.generated[str(output.resolve())] = pin(output)
        return output

    def read(self,name,candidate,reference,fixture,fmt="yuyv",identity=False,planar=False):
        report = self.out/(name+".metrics.json")
        argv = [sys.executable,READER,"--candidate",candidate,"--reference",reference,"--format",fmt,
            "--frames",str(self.frames[fixture]),"--output",report]
        if identity:
            argv.append("--require-identical")
        if planar:
            argv += ["--planar-luma",self.pins[fixture+"/I420"]["path"]]
        self.run(name+"-read",argv)
        data = json.loads(report.read_text())
        if data["status"]!="MEASURED" or data["frames_compared"]!=self.frames[fixture] or (identity and not data["same_bytes"]):
            raise ValueError("reader population/identity mismatch")
        known = {p["path"]:p["sha256"] for p in list(self.pins.values())+list(self.generated.values())}
        if any(known.get(v["path"])!=v["sha256"] for v in data["input_identity"].values()):
            raise ValueError("reader bytes differ from frozen inputs")
        row = {"case":name,"fixture":fixture,"same_bytes":data["same_bytes"],"Y_exact":data["all_frame_Y_exact"],
            "input_sha256":{k:v["sha256"] for k,v in data["input_identity"].items()},"metrics":report.name,
            "components":{k:{field:v[field] for field in METRICS} for k,v in data["components_all_frames"].items()}}
        if planar:
            row["planar_luma_exact"] = {k:v["exact"] for k,v in data["planar_luma_controls_all_frames"].items()}
        return row

    def execute(self):
        self.stage = "reader_controls"
        log = self.run("reader-controls",[sys.executable,READER,"--self-test"])
        result = json.loads(log.read_text())
        if result.get("status")!="READER_CONTROLS_PASS" or len(result.get("controls",[]))!=7:
            raise ValueError("unchanged seven reader controls failed")
        self.counts["reader_controls"] = 1
        self.stage = "new_converter_identities_bicubic4"
        for runtime in RUNTIMES:
            for fixture in FIXTURES:
                for mode,siting,kind,fmt in (("i420-i420","center","I420","i420"),("yuyv-yuyv","left","native_YUYV","yuyv")):
                    name = f"identity-{runtime}-{fixture}-{mode}"
                    output = self.convert(name,runtime,fixture,4,mode,siting,siting)
                    row = self.read(name,output,self.pins[fixture+"/"+kind]["path"],fixture,fmt,identity=True)
                    row.update(runtime=runtime,mode=mode)
                    self.rows["identities"].append(row)
                    self.counts["identities"] += 1
        save(self.out/"eight-new-converter-identities.json",self.rows["identities"])
        self.stage,outputs = "filter_native_matrix",{}
        for algorithm in self.filters:
            for runtime in RUNTIMES:
                for fixture in FIXTURES:
                    for src in SITINGS:
                        for dst in SITINGS:
                            name = f"{algorithm['name']}-{runtime}-{fixture}-{src}-to-{dst}"
                            output = self.convert(name,runtime,fixture,algorithm["flags"],"i420-yuyv",src,dst)
                            outputs[algorithm["name"],runtime,fixture,src,dst] = output
                            row = self.read(name,self.pins[fixture+"/native_YUYV"]["path"],output,fixture,planar=True)
                            row.update(filter=algorithm["name"],flags=algorithm["flags"],runtime=runtime,src=src,dst=dst)
                            self.rows["native"].append(row)
                            self.counts["conversions"] += 1
        self.stage = "fixed_case_pre_post"
        for algorithm in self.filters:
            for fixture in FIXTURES:
                for src in SITINGS:
                    for dst in SITINGS:
                        name = f"pair-{algorithm['name']}-{fixture}-{src}-to-{dst}"
                        row = self.read(name,outputs[algorithm["name"],"post6507",fixture,src,dst],
                            outputs[algorithm["name"],"pre0a41",fixture,src,dst],fixture)
                        row.update(filter=algorithm["name"],flags=algorithm["flags"],src=src,dst=dst,direction="post6507 minus pre0a41")
                        self.rows["pre_post"].append(row)
                        self.counts["pre_post"] += 1
        self.stage = "existing_bicubic_hash_discriminator"
        for n,case in enumerate(self.manifest.get("bicubic_cross_hash_cases",[])):
            old,post = self.pins[f"bicubic-{n}/old711"],self.pins[f"bicubic-{n}/post6507"]
            self.rows["old711_post_bicubic_hashes"].append({"fixture":case["fixture"],"src":case["src"],"dst":case["dst"],
                "old711_sha256":old["sha256"],"post6507_sha256":post["sha256"],"same_bytes":old["sha256"]==post["sha256"]})
        count = len(self.filters)
        expected = {"reader_controls":1,"identities":8,"conversions":36*count,"pre_post":18*count,"commands":17+90*count}
        if self.counts!=expected or self.manifest.get("expected_counts")!=expected:
            raise ValueError("frozen complete case counts differ")
        self.stage = "final_pin_verification"
        for record in list(self.pins.values())+list(self.generated.values()):
            stable(record,content=True)
        save(self.out/"all-pins-unchanged.json",{"status":"ALL_FROZEN_AND_GENERATED_BYTES_UNCHANGED","pins":len(self.pins),"outputs":len(self.generated)})

    def aggregate(self):
        result = []
        for algorithm in getattr(self,"filters",[]):
            for runtime in RUNTIMES:
                for fixture in FIXTURES:
                    rows = [r for r in self.rows["native"] if r["filter"]==algorithm["name"] and r["runtime"]==runtime and r["fixture"]==fixture]
                    if rows:
                        result.append({"filter":algorithm["name"],"runtime":runtime,"fixture":fixture,"cases":len(rows),
                            "Y_exact_all":all(r["Y_exact"] for r in rows),"max_error_all9":{c:max(r["components"][c]["maximum_absolute_error"] for r in rows) for c in COMPONENTS}})
        return result

    def report(self):
        return {"schema":"crystalhd.issue529.filter-reference-report.v1","status":"MEASURED","counts":self.counts,
            "filters":self.filters,"aggregate":self.aggregate(),"results":self.rows,
            "revisions":{r:{"commit":REVISIONS[r][0],"tree":REVISIONS[r][1],"configure_flags":self.builds[r]["configure_flags"],
                "libraries":{k:{"filename":Path(self.pins[r+"/"+k]["path"]).name,"bytes":self.pins[r+"/"+k]["bytes"],"sha256":self.pins[r+"/"+k]["sha256"]} for k in ("scale","util")}} for r in RUNTIMES},
            "fixtures":{f:{"frames":self.frames[f],"geometry":[352,288],"hashes":{k:self.pins[f+"/"+k]["sha256"] for k in ("ES","I420","native_YUYV")}} for f in FIXTURES},
            "owned_sources":{label:{"bytes":self.pins[label]["bytes"],"sha256":self.pins[label]["sha256"]} for label in ("runner","reader","converter-source","converter","manifest")},
            "method":"same retainedI420; fixed full9siting/filter matrix; limited601/threads1; noRGB/resize; fullframe component metrics; no best-case selection",
            "identity_scope":"eight new-converter bicubic4 identities; qualified providers reused; each new filter AVOption verified percase",
            "reference_branch":"POINT16 makes the pre unscaled420-toYUYV wrapper eligible. A0.25chroma-sample displacement may still yield identical nearest samples; equal output is not ignored-siting proof.",
            "historical_flags":"UNKNOWN","decoder_stage":"NOT_RUN; no fresh ES decoder comparison","raw_order_preserved":True,
            "PTS":"ordinal/25 synthetic; packet tokens never sorted asPTS","quality_target_applied":False,
            "quality_pass_claim":False,"firmware_or_general_FFmpeg_effect_claim":False,"new_hardware":"NOT_RUN"}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest",required=True)
    parser.add_argument("--converter",required=True)
    parser.add_argument("--output",required=True)
    args = parser.parse_args()
    args.manifest,args.converter = str(Path(args.manifest).resolve()),str(Path(args.converter).resolve())
    out = Path(args.output).resolve()
    try:
        out.mkdir(mode=0o700)
    except FileExistsError:
        parser.error("output exists; preserve it; no overwrite/resume/retry")
    matrix,error = Matrix(args),None
    try:
        matrix.freeze()
        matrix.execute()
        save(out/"reference-report.json",matrix.report())
    except BaseException as caught:
        error = {"type":type(caught).__name__,"message":str(caught)}
    summary = {"schema":"crystalhd.issue529.filter-summary.v1","status":"FAIL_RETAIN_ALL_ARTIFACTS" if error else "FIXED_FILTER_MATRIX_MEASURED",
        "error":error,"stage":matrix.stage,"last_case":matrix.case,"counts":matrix.counts,"aggregate":matrix.aggregate(),
        "pre_post_identical_cases":sum(r["same_bytes"] for r in matrix.rows["pre_post"]),
        "old711_post_bicubic_identical_hashes":sum(r["same_bytes"] for r in matrix.rows["old711_post_bicubic_hashes"]),
        "elapsed_seconds":time.monotonic()-matrix.start,"quality_pass_claim":False,"historical_flags":"UNKNOWN"}
    save(out/"summary.json",summary)
    print(json.dumps(summary,sort_keys=True),flush=True)
    return int(error is not None)

if __name__ == "__main__":
    sys.exit(main())
