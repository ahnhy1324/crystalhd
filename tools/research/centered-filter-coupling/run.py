#!/usr/bin/env python3
"""One CPU-only combined study using the retained original-provider namespace."""
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

sys.dont_write_bytecode = True
RUN = Path(os.environ["CRYSTALHD_COUPLING_RUN_DIR"]).resolve()
SOURCE = Path(__file__).resolve().parents[3]
OLD = Path('/home/mache/.cache/crystalhd-bwdif-paced-run-20261107')
CAPTURE = Path('/home/mache/.cache/crystalhd-remaining-qualification-run-20261106/drop-origin-900-25-01-stock/payload')

def need(ok, why):
    if not ok:
        raise ValueError(why)

def main():
    need(len(sys.argv) == 2 and sys.argv[1] in ('ordinary900-01', 'ordinary900-02', 'timing900-01', 'held8-control-01'), 'one fixed fresh case required')
    held = sys.argv[1].startswith('held8')
    case = RUN/sys.argv[1]
    need(not case.exists(), 'preserve prior case')
    spec = importlib.util.spec_from_file_location('original_reference', SOURCE/'tests/vaapi-postfilter-reference.py')
    reference = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reference)
    manifest = json.loads((CAPTURE/'reference-input-graph-1.json').read_text())
    reference.validate_manifest(manifest)
    command = json.loads((OLD/'paced-0-01/command.json').read_text())
    command = command[:command.index('--pattern-inputs')]
    command = [str(RUN/'coupled') if v == str(OLD/'postfilter-paced-cost') else
               str(RUN) if v == str(OLD) else
               '/results/'+case.name if v == '/results/paced-0-01' else v for v in command]
    if held:
        command += ['--hold-image', '--bounded-control']
    if sys.argv[1] == 'timing900-01':
        command += ['--timing-observer-condition']
    need('--dev' in command and command[command.index('--dev')+1] == '/dev' and
         not any(v in ('/dev/dri','/dev/crystalhd') for v in command), 'private device-free namespace required')
    case.mkdir(mode=0o700)
    (case/'command.json').write_text(json.dumps(command,indent=2)+'\n')
    report = dict(status='RUNNING',source_revision='1427d2de0825706e20137f16e17dcb42366bac97',
                  decoder=False,physical_devices_exposed=False,case=case.name,
                  expected_inputs=8 if held else 900,forced_image_overlap_control=held)
    process = None
    pidfd = None
    identity = None
    started = time.monotonic()
    try:
        with (case/'raw.jsonl').open('xb') as out, (case/'stderr').open('xb') as err:
            process = subprocess.Popen(command,stdin=subprocess.DEVNULL,stdout=out,stderr=err,start_new_session=True)
            if process.poll() is None:
                pidfd = os.pidfd_open(process.pid)
                identity = reference.process_identity(process)
            while process.poll() is None:
                need(time.monotonic()-started < 100, 'combined CPU study100s deadline')
                need((case/'raw.jsonl').stat().st_size < 12*1024*1024 and (case/'stderr').stat().st_size < 1024*1024,
                     'bounded original and combined output')
                time.sleep(.1)
        report.update(exit=process.returncode,elapsed_s=time.monotonic()-started,owned_process=identity)
        need(process.returncode == 0, 'preserve failed combined study')
        rows = [json.loads(line) for line in (case/'raw.jsonl').read_text().splitlines()]
        positions = [i for i,row in enumerate(rows) if row.get('type') == 'coupled_header']
        need(len(positions) == 1, 'one combined header')
        index = positions[0]
        report['captured_prefix'] = reference.compare_reference(manifest,rows[:index],case)
        head, tail = rows[index], rows[-1]
        need(head['inputs'] == report['expected_inputs'] and head['outputs'] == 2*(head['inputs']-1) and
             head['same_canvas_overlap_writes'] == (7 if held else 0) and head['final_cow_live_bytes'] == 0 and
             tail.get('complete') is True and tail.get('graphs_live') == 0 and tail.get('images_live') == 0,
             'combined count/lifetime completion required')
        report.update(status='PASS_SCOPED_COMBINED_CURRENT_OWN_AND_ORIGINAL_FILTER',header=head,footer=tail,
                      process_retired=True,stderr_empty=(case/'stderr').stat().st_size == 0)
    except BaseException as error:
        report.update(status='FAIL',reason=repr(error))
        raise
    finally:
        if process is not None and process.poll() is None:
            if pidfd is not None and identity == reference.process_identity(process):
                signal.pidfd_send_signal(pidfd,signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    report['handoff_required'] = True
            else:
                report['handoff_required'] = True
        if pidfd is not None:
            os.close(pidfd)
        (case/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ('status','elapsed_s','header','stderr_empty')}))

if __name__ == '__main__':
    main()
