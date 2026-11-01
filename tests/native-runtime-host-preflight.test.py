#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Inert sysfs/stat fixtures; no devices are created, opened or loaded."""
from contextlib import redirect_stdout
import importlib.util
import io
import os
from pathlib import Path
import shutil
import stat
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('native_host_preflight',
    ROOT / 'scripts/native-runtime-host-preflight.py')
HOST = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HOST)


class HostPreflightTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.sysfs, self.dev = self.root / 'sys', self.root / 'dev'
        self.dev.mkdir()
        self.module = self.sysfs / 'module/crystalhd'
        self.module.mkdir(parents=True)
        (self.module / 'version').write_text('3.10.13\n')
        (self.module / 'srcversion').write_text('F76E163690F147B91495DDD\n')
        self.driver = self.sysfs / 'bus/pci/drivers/crystalhd'
        self.driver.mkdir(parents=True)
        (self.driver / 'module').symlink_to(self.module)
        self.device = self.sysfs / 'devices/pci0000:00/0000:02:00.0'
        self.device.mkdir(parents=True)
        (self.device / 'subsystem').symlink_to(self.sysfs / 'bus/pci')
        (self.device / 'driver').symlink_to(self.driver)
        self.video = self.device / 'video4linux/video7'
        self.video.mkdir(parents=True)
        (self.video / 'dev').write_text('81:7\n')
        (self.video / 'device').symlink_to(self.device)
        self.directory = self.sysfs / 'class/video4linux'
        self.directory.mkdir(parents=True)
        (self.directory / 'video7').symlink_to(self.video)
        self.node = self.dev / 'video7'
        self.node.write_bytes(b'inert placeholder, never a real device')

    def character_stat(self, path):
        observed = os.lstat(path)
        if Path(path) == self.node and stat.S_ISREG(observed.st_mode):
            return SimpleNamespace(st_mode=stat.S_IFCHR | 0o600, st_rdev=os.makedev(81, 7))
        return observed

    def observe(self, **options):
        values = {'sysfs_root': self.sysfs, 'dev_root': self.dev,
                  'node_stat': self.character_stat}
        values.update(options)
        return HOST.observe(**values)

    def assert_unqualified(self, report):
        self.assertEqual(report['public_distribution_status'], 'PUBLIC_NOT_READY')
        self.assertFalse(report['module_strings_are_ABI_authentication'])
        self.assertTrue(all(value == 'NOT_RUN' for value in report['runtime_acceptance'].values()))

    def assert_blocked(self, report):
        self.assertEqual(report['status'], 'BLOCKED_NATIVE_RUNTIME_PREREQUISITES', report)
        self.assertTrue(report['blocking_reasons'])
        self.assert_unqualified(report)

    def test_positive_is_present_unverified_not_runtime_acceptance(self):
        report = self.observe()
        self.assertEqual(report['status'], 'PREREQUISITES_PRESENT_UNVERIFIED', report)
        self.assertEqual(report['candidate_video_nodes'], [str(self.node)])
        self.assertEqual(report['module_identity_pin_state'], 'UNPINNED')
        self.assertEqual(report['loaded_module']['version'], '3.10.13')
        self.assert_unqualified(report)

    def test_expected_identity_mismatch_retains_actual_observations_and_blocks(self):
        report = self.observe(expected_module_version='3.10.14',
                              expected_module_srcversion='E3D1535AB4C470BC21D6E32')
        self.assert_blocked(report)
        self.assertEqual(report['loaded_module']['version'], '3.10.13')
        self.assertEqual(report['loaded_module']['srcversion'], 'F76E163690F147B91495DDD')
        self.assertEqual(report['expected_module']['version'], '3.10.14')
        self.assertEqual(len(report['candidate_video_nodes']), 1)

    def test_matching_expected_strings_do_not_authenticate_ABI(self):
        report = self.observe(expected_module_version='3.10.13',
                              expected_module_srcversion='F76E163690F147B91495DDD')
        self.assertEqual(report['status'], 'PREREQUISITES_PRESENT_UNVERIFIED', report)
        self.assert_unqualified(report)

    def test_missing_module_blocks(self):
        shutil.rmtree(self.module)
        report = self.observe()
        self.assert_blocked(report)
        self.assertFalse(report['loaded_module']['present'])

    def test_missing_node_blocks(self):
        self.node.unlink()
        self.assert_blocked(self.observe())

    def test_regular_file_cannot_substitute_for_character_node(self):
        report = self.observe(node_stat=os.lstat)
        self.assert_blocked(report)
        self.assertIn('not a character device', report['video_entries'][0]['blocking_reasons'][0])

    def test_device_symlink_is_rejected_even_when_target_exists(self):
        target = self.dev / 'other'
        self.node.rename(target)
        self.node.symlink_to(target)
        report = self.observe()
        self.assert_blocked(report)
        self.assertIn('symlink', report['video_entries'][0]['blocking_reasons'][0])

    def test_wrong_character_rdev_is_rejected(self):
        report = self.observe(node_stat=lambda path: SimpleNamespace(
            st_mode=stat.S_IFCHR | 0o600, st_rdev=os.makedev(81, 8)))
        self.assert_blocked(report)
        self.assertIn('major:minor differ', report['video_entries'][0]['blocking_reasons'][0])

    def test_wrong_PCI_driver_and_wrong_driver_module_are_rejected(self):
        other = self.sysfs / 'bus/pci/drivers/other'
        other.mkdir()
        (other / 'module').symlink_to(self.module)
        (self.device / 'driver').unlink()
        (self.device / 'driver').symlink_to(other)
        self.assert_blocked(self.observe())
        (self.device / 'driver').unlink()
        (self.device / 'driver').symlink_to(self.driver)
        wrong_module = self.sysfs / 'module/camera'
        wrong_module.mkdir()
        (self.driver / 'module').unlink()
        (self.driver / 'module').symlink_to(wrong_module)
        report = self.observe()
        self.assert_blocked(report)
        self.assertIn('module is not crystalhd', report['video_entries'][0]['blocking_reasons'][0])

    def test_USB_camera_does_not_supply_CrystalHD_prerequisite(self):
        usb = self.sysfs / 'devices/pci0000:00/0000:00:1d.0/usb1/1-1:1.0'
        usb.mkdir(parents=True)
        (self.sysfs / 'bus/usb').mkdir()
        (usb / 'subsystem').symlink_to(self.sysfs / 'bus/usb')
        (self.video / 'device').unlink()
        (self.video / 'device').symlink_to(usb)
        report = self.observe()
        self.assert_blocked(report)
        self.assertEqual(report['candidate_video_nodes'], [])

    def test_missing_or_malformed_sysfs_identity_is_blocked(self):
        (self.module / 'version').unlink()
        self.assert_blocked(self.observe())
        (self.module / 'version').write_text('3.10.13\n')
        (self.video / 'dev').write_text('81:not-a-number\n')
        self.assert_blocked(self.observe())

    def test_racing_node_is_unavailable_not_runtime_verdict(self):
        def disappeared(path):
            raise FileNotFoundError('simulated node disappearance')
        report = self.observe(node_stat=disappeared)
        self.assert_blocked(report)
        self.assertIn('disappearance', report['video_entries'][0]['blocking_reasons'][0])

    def test_observer_opens_only_regular_sysfs_attributes(self):
        original_open, opened = os.open, []
        def sysfs_only(path, *args, **kwargs):
            path = Path(path)
            self.assertIn(self.sysfs, path.parents)
            self.assertNotIn(self.dev, path.parents)
            opened.append(path)
            return original_open(path, *args, **kwargs)
        with mock.patch.object(HOST.os, 'open', side_effect=sysfs_only):
            report = self.observe()
        self.assertEqual(report['status'], 'PREREQUISITES_PRESENT_UNVERIFIED', report)
        self.assertEqual({p.name for p in opened}, {'version', 'srcversion', 'dev'})

    def test_CLI_exit_codes_never_call_present_observation_PASS(self):
        for status, expected in [('BLOCKED_NATIVE_RUNTIME_PREREQUISITES', 2),
                                 ('PREREQUISITES_PRESENT_UNVERIFIED', 0)]:
            with mock.patch.object(HOST, 'observe', return_value={'status': status}), redirect_stdout(io.StringIO()):
                self.assertEqual(HOST.main([]), expected)


if __name__ == '__main__':
    unittest.main()
