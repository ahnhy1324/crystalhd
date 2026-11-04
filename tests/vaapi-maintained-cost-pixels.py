#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run existing owned pixel controls and require active, complete timing hooks."""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location(
        'active_pixel_cost_reader', Path(__file__).with_name('vaapi-maintained-cost-reader.py'))
    reader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reader)
    with tempfile.TemporaryDirectory(prefix='crystalhd-cost-pixels-') as directory:
        prefix = Path(directory) / 'cost'
        env = dict(os.environ, CRYSTALHD_CONFIG_DISABLE='1',
                   CRYSTALHD_TEST_MAINTAINED_COST='v1',
                   CRYSTALHD_TEST_MAINTAINED_COST_PREFIX=str(prefix))
        process = subprocess.Popen([str(args.binary.resolve(strict=True))], env=env)
        status = process.wait()
        if status:
            raise RuntimeError('cost pixel controls failed: ' + str(status))
        result = reader.audit_report(Path(str(prefix) + '-' + str(process.pid) + '.jsonl'),
                                     expected_pid=process.pid)
        if result.get('status') != 'PASS_SCOPED_MAINTAINED_COST_OBSERVATION' or result.get('events', 0) == 0:
            raise RuntimeError('active pixel timing ledger failed: ' + str(result))
    print('Active pixel timing ledger: PASS')


if __name__ == '__main__':
    main()
