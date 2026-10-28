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
