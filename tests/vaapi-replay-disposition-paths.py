#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""CPU-only actual OWN path controls; every DIL/device call uses existing mocks.

Each case has its own PID and retained native/disposition files. Successful
temporary controls are removed; failures and explicit output roots are kept.
This scaffold cannot qualify stock playback or a hardware lease.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--library-directory', type=Path, required=True)
    parser.add_argument('--endpicture-body-binary', type=Path)
    parser.add_argument('--output-root', type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    library = args.library_directory.resolve(strict=True)
    directory = Path(__file__).resolve().parent
    reader = load('replay_path_reader', directory / 'vaapi-replay-disposition-reader.py')
    stock = load('replay_path_stock', directory / 'vaapi-centered-payload-stock.py')
    payload = load('replay_path_native', directory / 'vaapi-centered-payload-reader.py')
    root = (args.output_root.resolve() if args.output_root else
            Path(tempfile.mkdtemp(prefix='crystalhd-replay-path-')))
    if args.output_root:
        root.mkdir(parents=True)
    results = {}
    try:
        for case in ('positive', 'retired-backing', 'wrong-parity', 'release-failure'):
            output = root / case
            output.mkdir()
            env = dict(os.environ, CRYSTALHD_CONFIG_DISABLE='1', CRYSTALHD_VAAPI_TRACE='1',
                CRYSTALHD_TEST_MAINTAINED_COST='',
                CRYSTALHD_TEST_REPLAY_DISPOSITION='v1',
                CRYSTALHD_TEST_REPLAY_DISPOSITION_PREFIX=str(output / 'replay-disposition'),
                LD_LIBRARY_PATH=str(library))
            with (output / 'stdout.log').open('xb') as stdout, (output / 'native.log').open('xb') as stderr:
                child = subprocess.Popen([str(binary), '--case', case], env=env, stdout=stdout, stderr=stderr)
                try:
                    code = child.wait(timeout=45)
                except subprocess.TimeoutExpired:
                    # This executable is explicitly CPU-only with all device
                    # calls mocked; it is never the media retirement path.
                    child.kill()
                    child.wait()
                    raise
            require(code == 0, case + ': production-path assertion failed; see ' + str(output))
            path = output / ('replay-disposition-' + str(child.pid) + '.jsonl')
            validated = reader.read_report(path, expected_pid=child.pid)
            audit = reader.audit_report(path, expected_pid=child.pid)
            require(audit.get('status') == 'PASS_SCOPED_REPLAY_DISPOSITION_OBSERVATION',
                    case + ': complete active disposition ledger required')
            events = validated['events']
            require(any(e['event'] == 'input_commit' and e['completed_before'] is True for e in events),
                    case + ': actual completed-history input must be exercised')
            raw = (output / 'native.log').read_bytes()
            native = stock.parse_rows(raw, payload)
            windows = {'1': dict(begin=0, end=len(raw), begin_ns=0, end_ns=(1 << 64)-1)}
            # Exactly two originals are admitted by this declared software
            # scaffold. The real frozen controller always supplies900.
            census = reader.census_case(validated, native, windows, child.pid, expected_originals=2)
            positive = case in ('positive', 'retired-backing')
            legacy_comparison = None
            if positive:
                require(census.get('status') == 'PASS_SCOPED_REPLAY_DISPOSITION_CENSUS' and
                        census['plays']['1']['ledger_status'] == 'PASS',
                        case + ': exact production replay/history census rejected: ' + json.dumps(census))
                require(any(e['event'] == 'disposition' and e['observe'] == 'duplicate' and
                            e['pair_valid'] is True for e in events), case + ': actual validated duplicate pair required')
                if case == 'retired-backing':
                    require(any(e['event'] == 'output' and e['token'] == 1 and
                                e['live_decode_identity'] == e['live_owner'] == 0 for e in events),
                            'retired backing must retain raw zero live identity')
                    try:
                        stock.operation_census(native, windows['1'], child.pid, 900)
                    except ValueError as error:
                        require('incomplete binding/sequence identity' in str(error),
                                'legacy comparison must reproduce the original identity rejection')
                        legacy_error = str(error)
                    else:
                        raise RuntimeError('untagged legacy census unexpectedly accepts retired raw identity')
                    same900 = reader.census_case(validated, native, windows, child.pid, expected_originals=900)
                    play900 = same900['plays']['1']
                    require(play900['ledger_status'] == 'PASS' and play900['status'] == 'FAIL' and
                            play900['source_population_qualified'] is False,
                            'replay repair must keep the identical900-original criterion unqualified')
                    legacy_comparison = dict(expected_originals=900, legacy_error=legacy_error,
                        corrected_ledger_status=play900['ledger_status'],
                        corrected_source_status=play900['status'],
                        observed_originals=play900['observed_submit_bindings'])
            else:
                require(census.get('status') != 'PASS_SCOPED_REPLAY_DISPOSITION_CENSUS' and
                        census['plays']['1']['ledger_status'] == 'FAIL',
                        case + ': expected product rejection cannot qualify a ledger')
                if case == 'wrong-parity':
                    require(any(e['event'] == 'disposition' and e['result'] != 0 and
                                e['pair_valid'] is False for e in events), 'wrong parity rejection not observed')
                else:
                    require(any(e['event'] == 'release' and e['result'] != 0 for e in events),
                            'failed original release not observed')
            result = dict(cpu_scaffold_status='PASS', owner_pid=child.pid, expected_negative=not positive,
                          audit=audit, census=census, legacy_versus_new_900=legacy_comparison)
            (output / 'analysis.json').write_text(json.dumps(result, indent=2)+'\n')
            results[case] = dict(owner_pid=child.pid, expected_negative=not positive,
                                 ledger_status=census['plays']['1']['ledger_status'])
        if args.endpicture_body_binary:
            output = root / 'endpicture-body'
            output.mkdir()
            env = dict(os.environ, CRYSTALHD_CONFIG_DISABLE='1', CRYSTALHD_VAAPI_TRACE='1',
                CRYSTALHD_TEST_MAINTAINED_COST='v2',
                CRYSTALHD_TEST_MAINTAINED_COST_PREFIX=str(output / 'maintained-cost'),
                CRYSTALHD_TEST_REPLAY_DISPOSITION='', LD_LIBRARY_PATH=str(library))
            with (output / 'stdout.log').open('xb') as stdout, (output / 'native.log').open('xb') as stderr:
                child = subprocess.Popen([str(args.endpicture_body_binary.resolve(strict=True))],
                                         env=env, stdout=stdout, stderr=stderr)
                try:
                    code = child.wait(timeout=45)
                except subprocess.TimeoutExpired:
                    # This binary includes the same CPU-only DIL/device mocks.
                    child.kill()
                    child.wait()
                    raise
            require(code == 0, 'EndPicture body assertions failed; see ' + str(output))
            cost = load('endpicture_cost_reader', directory / 'vaapi-maintained-cost-reader.py')
            path = output / ('maintained-cost-' + str(child.pid) + '.jsonl')
            validated = cost.read_report(path, expected_pid=child.pid)
            require(validated['header']['schema'] == cost.ENDPICTURE_SCHEMA and
                    len(validated['events']) == 4 and
                    all(e['stage'] == 'end_picture_body' for e in validated['events']) and
                    [e['detail'] for e in validated['events']] == [0, 1, 0, 2],
                    'EndPicture body publication lost actual four-path coverage')
            results['endpicture-body'] = dict(owner_pid=child.pid, cpu_scaffold_status='PASS',
                                             actual_body_scopes=4)
        (root / 'summary.json').write_text(json.dumps(results, indent=2)+'\n')
        print('OWN replay production-path controls: 2 positive / 2 expected rejection ledgers PASS')
        if args.endpicture_body_binary:
            print('EndPicture body: four actual early-return scopes and complete publication PASS')
    except BaseException:
        print('Replay path failure artifacts retained at ' + str(root), flush=True)
        raise
    else:
        if not args.output_root:
            shutil.rmtree(root)


if __name__ == '__main__':
    main()
