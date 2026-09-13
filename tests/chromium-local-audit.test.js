// SPDX-License-Identifier: LGPL-2.1-or-later
'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const {createFrameAuditor, validateBrowserAudit} = require('./chromium-local-audit');

function sample(identity, overrides = {}) {
  return {identity, frameTime: identity / 30, callbackTime: identity / 30,
    white: 255, black: 0, seeking: false, ...overrides};
}

function completed(hardware = false, transform = value => value) {
  const auditor = createFrameAuditor();
  let identity = 0;
  for (let step = 0; step < 1000 && identity < 360; ++step) {
    const seek = auditor.observe(transform(sample(identity)));
    identity = seek === undefined ? identity + 1 : seek * 30;
  }
  auditor.end(sample(359), 12);
  return {hardware, audit: auditor.audit, mediaErrors: [],
    decoders: [hardware ? 'VaapiVideoDecoder' : 'FFmpegVideoDecoder'],
    platformDecoders: [hardware ? 'true' : 'false']};
}

test('complete software playback validates exact pixels, four seeks, and final frame', () => {
  const result = completed();
  assert.deepEqual(result.audit.segments.map(segment => segment.target), [0, 6, 2, 8, 0]);
  assert.equal(result.audit.finalFrame.identity, 359);
  assert.doesNotThrow(() => validateBrowserAudit(result));
});

test('complete hardware playback also requires the platform decoder flag', () => {
  assert.doesNotThrow(() => validateBrowserAudit(completed(true)));
});

test('delayed callback metadata does not weaken pixel identity checks', () => {
  const result = completed(false, value => ({...value, callbackTime: value.frameTime - 1 / 30}));
  assert.ok(result.audit.adjacentReadbacks > 0);
  assert.equal(result.audit.mismatches.length, 0);
  assert.doesNotThrow(() => validateBrowserAudit(result));
});

test('a wrong picture fails even when it is only one frame away', () => {
  const result = completed(false, value => value.identity === 50
    ? {...value, identity: 51} : value);
  assert.ok(result.audit.mismatches.length > 0);
  assert.throws(() => validateBrowserAudit(result), /correct frame identities/);
});

test('blank or corrupt barcode reference pixels fail', () => {
  const result = completed(false, value => value.identity === 60
    ? {...value, white: 16, black: 16} : value);
  assert.throws(() => validateBrowserAudit(result), /correct frame identities/);
});

test('advancing callbacks with a stalled actual picture do not count as playback', () => {
  const auditor = createFrameAuditor();
  for (let count = 0; count < 100; ++count)
    assert.equal(auditor.observe(sample(0, {callbackTime: count / 30})), undefined);
  auditor.end(sample(359), 12);
  assert.equal(auditor.audit.frames, 1);
  assert.equal(auditor.audit.duplicateReadbacks, 99);
  assert.throws(() => validateBrowserAudit({...completed(), audit: auditor.audit}),
    /all four seeks/);
});

test('a timeout or stalled playback without ended fails', () => {
  const result = completed();
  result.audit.done = false;
  assert.throws(() => validateBrowserAudit(result), /did not complete/);
});

test('ended without the final picture is rejected by the page auditor', () => {
  const auditor = createFrameAuditor();
  auditor.end(sample(358), 12);
  assert.match(auditor.audit.errors[0], /final frame 359/);
});

test('final validation independently rejects a missing tail even with five good segments', () => {
  const result = completed();
  result.audit.finalFrame = sample(358);
  assert.throws(() => validateBrowserAudit(result), /actual final frame 359/);
});

test('fixture duration and final snapshot are mandatory', () => {
  const wrongDuration = completed();
  wrongDuration.audit.duration = 11;
  assert.throws(() => validateBrowserAudit(wrongDuration), /12-second fixture/);
  const missingSnapshot = completed();
  missingSnapshot.audit.finalFrame = null;
  assert.throws(() => validateBrowserAudit(missingSnapshot), /actual final frame/);
});

test('hardware-to-software fallback fails even if all displayed pixels are correct', () => {
  const result = completed(true);
  result.decoders.push('FFmpegVideoDecoder');
  result.platformDecoders.push('false');
  assert.throws(() => validateBrowserAudit(result), /exclusively use VaapiVideoDecoder/);
});

test('a hardware-looking decoder name cannot conceal a false or absent platform flag', () => {
  const result = completed(true);
  result.platformDecoders = ['false'];
  assert.throws(() => validateBrowserAudit(result), /Platform decoder status/);
  result.platformDecoders = [];
  assert.throws(() => validateBrowserAudit(result), /Platform decoder status/);
});

test('software-default probe refuses hardware decoder selection', () => {
  const result = completed();
  result.decoders = ['VaapiVideoDecoder'];
  result.platformDecoders = ['true'];
  assert.throws(() => validateBrowserAudit(result), /exclusively use FFmpegVideoDecoder/);
});

test('post-seek preroll is rejected even when pixels match their own timestamp', () => {
  const auditor = createFrameAuditor();
  let seek;
  for (let identity = 0; identity < 30 && seek === undefined; ++identity)
    seek = auditor.observe(sample(identity));
  assert.equal(seek, 6);
  auditor.observe(sample(179));
  assert.match(auditor.audit.errors[0], /Pre-target frame after seek/);
  assert.equal(auditor.audit.segments.length, 1);
});

test('late callback may settle just after target but cannot skip the target window', () => {
  const auditor = createFrameAuditor();
  let seek;
  for (let identity = 0; identity < 30 && seek === undefined; ++identity)
    seek = auditor.observe(sample(identity));
  auditor.observe(sample(182));
  assert.equal(auditor.audit.segments.length, 2);
  assert.equal(auditor.audit.errors.length, 0);
  const result = completed();
  result.audit.segments[1].first = 6.2;
  assert.throws(() => validateBrowserAudit(result), /all four seeks/);
});

test('decoder errors fail an otherwise complete correct audit', () => {
  const result = completed(true);
  result.mediaErrors.push({errorType: 'decode', code: 1});
  assert.throws(() => validateBrowserAudit(result), /did not complete/);
});
