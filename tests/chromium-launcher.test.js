// SPDX-License-Identifier: LGPL-2.1-or-later
'use strict';

const assert = require('node:assert/strict');
const {spawnSync} = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

function launch(overrides = {}, browserVersion = 'Chromium 999.0',
    configText = null) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'crystalhd-launcher-'));
  try {
    const browser = path.join(directory, 'mock-browser');
    const config = path.join(directory, 'chromium.conf');
    fs.writeFileSync(browser, `#!/usr/bin/env node
if (process.argv[2] === '--version') {
  console.log(${JSON.stringify(browserVersion)});
} else {
  console.log(JSON.stringify({args: process.argv.slice(2),
    driver: process.env.LIBVA_DRIVER_NAME || null,
    driverPath: process.env.LIBVA_DRIVERS_PATH || null}));
}
`, {mode: 0o700});
    if (configText !== null)
      fs.writeFileSync(config, configText, {mode: 0o600});
    const env = {...process.env};
    for (const key of Object.keys(env)) {
      if (key.startsWith('CRYSTALHD_CHROMIUM_') || key.startsWith('LIBVA_'))
        delete env[key];
    }
    Object.assign(env, {
      CRYSTALHD_CHROMIUM: browser,
      CRYSTALHD_CHROMIUM_CONFIG: configText === null ?
        path.join(directory, 'absent.conf') : config,
      ...overrides,
    });
    return spawnSync('sh', [path.join(__dirname, '../scripts/crystalhd-chromium'),
      'https://example.invalid/video'], {env, encoding: 'utf8'});
  } finally {
    fs.rmSync(directory, {recursive: true, force: true});
  }
}

for (const layout of [
  {name: 'split directories', bindir: '/opt/crystal/bin', datadir: '/opt/crystal-data/share'},
  {name: 'spaces and Unicode', bindir: '/opt/설정 browser/bin', datadir: '/opt/CrystalHD data/공유'},
  {name: 'default directories', bindir: '/usr/bin', datadir: '/usr/share'},
]) {
  test(`installed browser launcher uses selected data: ${layout.name}`, () => {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'crystalhd-browser-install-'));
    const root = path.resolve(__dirname, '..');
    try {
      let stage = path.join(directory, 'stage');
      const make = (target) => spawnSync('make', [target,
        `DESTDIR=${stage}`, `BINDIR=${layout.bindir}`, `DATADIR=${layout.datadir}`],
      {cwd: root, encoding: 'utf8'});
      const installed = make('install-browser');
      assert.equal(installed.status, 0, installed.stdout + installed.stderr);
      const launcherRelative = path.join(layout.bindir.slice(1), 'crystalhd-chromium');
      const contents = fs.readFileSync(path.join(stage, launcherRelative), 'utf8');
      assert.ok(!contents.includes(stage), 'DESTDIR must not become a runtime location');
      const relocated = path.join(directory, 'relocated stage');
      fs.renameSync(stage, relocated);
      stage = relocated;
      // A rendered installation must not mistake an adjacent old checkout
      // for its explicitly selected data directory.
      const stale = path.join(stage, path.dirname(layout.bindir.slice(1)), 'browser/h264-only');
      fs.mkdirSync(stale, {recursive: true});
      fs.writeFileSync(path.join(stale, 'manifest.json'), JSON.stringify({name: 'stale checkout'}));
      const browser = path.join(directory, 'fake-browser');
      fs.writeFileSync(browser, `#!${process.execPath}
const fs = require('node:fs');
if (process.argv[2] === '--version') console.log('Chromium 999.0');
else {
  const args = process.argv.slice(2);
  const extension = args.find(arg => arg.startsWith('--load-extension=')).slice(17);
  console.log(JSON.stringify({args, extension: fs.realpathSync(extension),
    manifest: JSON.parse(fs.readFileSync(extension + '/manifest.json', 'utf8'))}));
}
`, {mode: 0o700});
      const env = {...process.env};
      for (const key of Object.keys(env))
        if (key.startsWith('CRYSTALHD_') || key.startsWith('LIBVA_')) delete env[key];
      Object.assign(env, {CRYSTALHD_CHROMIUM: browser, CRYSTALHD_CHROMIUM_CONFIG_DISABLE: '1'});
      const uri = 'https://example.invalid/video?value=one two&literal=%26';
      const result = spawnSync(path.join(stage, launcherRelative), [uri], {env, encoding: 'utf8'});
      assert.equal(result.status, 0, result.stderr);
      const invocation = JSON.parse(result.stdout);
      const extension = path.join(stage, layout.datadir.slice(1), 'crystalhd/h264-only');
      assert.equal(invocation.extension, fs.realpathSync(extension));
      assert.deepEqual(invocation.manifest,
        JSON.parse(fs.readFileSync(path.join(root, 'browser/h264-only/manifest.json'), 'utf8')));
      assert.equal(fs.readFileSync(path.join(extension, 'prefer-h264.js'), 'utf8'),
        fs.readFileSync(path.join(root, 'browser/h264-only/prefer-h264.js'), 'utf8'));
      assert.equal(invocation.args.at(-1), uri);
      assert.ok(invocation.args.includes('--disable-accelerated-video-decode'));
      assert.ok(!invocation.args.includes('--disable-gpu-sandbox'));
      const sentinel = path.join(stage, layout.datadir.slice(1), 'crystalhd/other-user-file');
      fs.writeFileSync(sentinel, 'retained');
      for (let attempt = 0; attempt < 2; ++attempt) {
        const removed = make('uninstall-browser');
        assert.equal(removed.status, 0, removed.stdout + removed.stderr);
      }
      assert.ok(!fs.existsSync(path.join(stage, launcherRelative)));
      assert.ok(!fs.existsSync(path.join(extension, 'manifest.json')));
      assert.ok(!fs.existsSync(path.join(extension, 'prefer-h264.js')));
      assert.ok(!fs.existsSync(path.join(stage, layout.datadir.slice(1),
        'applications/crystalhd-chromium.desktop')));
      assert.equal(fs.readFileSync(sentinel, 'utf8'), 'retained');
    } finally {
      fs.rmSync(directory, {recursive: true, force: true});
    }
  });
}

test('password store preserves the browser default unless explicitly selected', () => {
  for (const overrides of [{}, {CRYSTALHD_CHROMIUM_PASSWORD_STORE: ''}]) {
    const result = launch(overrides);
    assert.equal(result.status, 0, result.stderr);
    const invocation = JSON.parse(result.stdout);
    assert.ok(!invocation.args.some((argument) =>
      argument.startsWith('--password-store=')));
  }
  const selected = launch({CRYSTALHD_CHROMIUM_PASSWORD_STORE: 'basic'});
  assert.equal(selected.status, 0, selected.stderr);
  assert.ok(JSON.parse(selected.stdout).args.includes('--password-store=basic'));
});

test('legacy readable shell config overrides an inherited launcher value', () => {
  const result = launch({
    CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE: '1',
    CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX: '1',
    CRYSTALHD_DRM_DEVICE: '/dev/dri/inherited',
  }, 'Chromium 999.0', 'CRYSTALHD_DRM_DEVICE=/dev/dri/from-config\n');
  assert.equal(result.status, 0, result.stderr);
  const invocation = JSON.parse(result.stdout);
  assert.ok(invocation.args.includes(
    '--hardware-video-device-path=/dev/dri/from-config'));
  assert.ok(!invocation.args.includes(
    '--hardware-video-device-path=/dev/dri/inherited'));
});

test('managed launch boundary bypasses the legacy shell config', () => {
  const result = launch({
    CRYSTALHD_CHROMIUM_CONFIG_DISABLE: '1',
    CRYSTALHD_CHROMIUM_PASSWORD_STORE: '',
  }, 'Chromium 999.0', 'CRYSTALHD_CHROMIUM_PASSWORD_STORE=basic\n');
  assert.equal(result.status, 0, result.stderr);
  const invocation = JSON.parse(result.stdout);
  assert.ok(!invocation.args.some((argument) =>
    argument.startsWith('--password-store=')));
});

for (const acknowledgement of ['', '1']) {
  test(`software default keeps GPU sandbox (legacy acknowledgement=${acknowledgement || 'unset'})`, () => {
    const result = launch({CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX: acknowledgement});
    assert.equal(result.status, 0, result.stderr);
    const invocation = JSON.parse(result.stdout);
    assert.ok(invocation.args.includes('--disable-accelerated-video-decode'));
    assert.ok(!invocation.args.includes('--disable-gpu-sandbox'));
    assert.ok(!invocation.args.includes('--no-sandbox'));
    assert.ok(!invocation.args.some((argument) => argument.startsWith('--hardware-video-device-path=')));
    assert.equal(invocation.driver, null);
    assert.equal(invocation.driverPath, null);
  });
}

test('experimental hardware requires explicit sandbox acknowledgement', () => {
  const result = launch({CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE: '1'});
  assert.equal(result.status, 1);
  assert.match(result.stderr, /CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX=1/);
  assert.equal(result.stdout, '');
});

test('acknowledged hardware selects CrystalHD and only disables GPU sandbox', () => {
  const result = launch({CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE: '1',
    CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX: '1'});
  assert.equal(result.status, 0, result.stderr);
  const invocation = JSON.parse(result.stdout);
  assert.ok(invocation.args.includes('--disable-gpu-sandbox'));
  assert.ok(!invocation.args.includes('--no-sandbox'));
  assert.ok(!invocation.args.includes('--disable-accelerated-video-decode'));
  assert.equal(invocation.driver, 'crystalhd');
});

test('Google Chrome warning does not claim software playback uses CrystalHD', () => {
  const result = launch({}, 'Google Chrome 999.0');
  assert.equal(result.status, 0, result.stderr);
  assert.match(result.stderr, /Google Chrome may ignore --load-extension/);
  assert.match(result.stderr, /external\/policy extensions still apply/);
  assert.match(result.stderr, /including in fresh profiles/);
  assert.match(result.stderr, /YouTube may select VP9 or AV1/);
  assert.doesNotMatch(result.stderr, /pages still use CrystalHD/);
  const invocation = JSON.parse(result.stdout);
  assert.ok(invocation.args.includes('--disable-accelerated-video-decode'));
  assert.ok(!invocation.args.includes('--disable-gpu-sandbox'));
});
