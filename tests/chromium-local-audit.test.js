// SPDX-License-Identifier: LGPL-2.1-or-later
'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const {createFrameAuditor, createPlaybackControlAuditor,
  validatePlaybackControls, validateBrowserAudit} = require('./chromium-local-audit');

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

// No wall-clock sleeps or browser are used here. The same pure controller is
// serialized into the real fixture page. A selected rate and actual media
// progression are deliberately independent so a metadata-only pass is tested.
function runControls({transform = value => value, actualRate = rate => rate,
  ignorePause = false, onSample = () => {}} = {}) {
  const controller = createPlaybackControlAuditor();
  let paused = false, rate = 1, media = 0;
  let lastSample;
  const actions = [];
  for (let step = 0; step < 400 && !controller.audit.done &&
      !controller.audit.errors.length; ++step) {
    const phase = controller.audit.phases.length;
    if (step && (!paused || ignorePause)) media += actualRate(rate) * 0.1;
    const identity = Math.floor((media + 0.000001) * 30);
    const observed = transform(sample(identity, {wallTime: step * 100,
      currentTime: media, paused, playbackRate: rate, ended: false}), {phase, step});
    lastSample = observed;
    const action = controller.observe(observed);
    onSample(observed, controller);
    if (action) {
      actions.push(action);
      rate = action.rate;
      paused = action.type === 'pause';
    }
  }
  return {audit: controller.audit, actions, lastSample};
}

test('controls verify pause/resume, all requested actual speeds, and restoration to 1x', () => {
  const {audit, actions} = runControls();
  assert.deepEqual(actions, [
    {type: 'pause', rate: 1}, {type: 'play', rate: 1},
    {type: 'play', rate: 0.5}, {type: 'play', rate: 1.5},
    {type: 'play', rate: 2}, {type: 'play', rate: 1},
  ]);
  assert.doesNotThrow(() => validatePlaybackControls(audit));
  assert.equal(audit.phases[1].firstIdentity, audit.phases[1].lastIdentity);
  assert.equal(audit.phases[1].advances, 0);
});

test('control mode still requires the existing exact four seeks and real final frame', () => {
  const auditor = createFrameAuditor();
  const control = runControls({onSample: value => {
    // Controls run before the four seeks; their samples do not trigger seeks.
    assert.equal(auditor.observe(value, false), undefined);
  }});
  let identity = control.lastSample.identity + 1;
  for (let step = 0; step < 1000 && identity < 360; ++step) {
    const seek = auditor.observe(sample(identity));
    identity = seek === undefined ? identity + 1 : seek * 30;
  }
  auditor.end(sample(359), 12);
  auditor.audit.controls = control.audit;
  const result = {...completed(), controls: true, audit: auditor.audit};
  assert.doesNotThrow(() => validateBrowserAudit(result));
  result.audit.controls.done = false;
  assert.throws(() => validateBrowserAudit(result), /playback-rate audit did not complete/);
});

test('a paused property cannot hide continuing actual playback', () => {
  const {audit} = runControls({ignorePause: true});
  assert.match(audit.errors[0], /continued after pause|Paused playback changed/);
  assert.throws(() => validatePlaybackControls(audit), /did not complete/);
});

test('pause must be confirmed and remain stable, not merely requested', () => {
  const ignored = runControls({transform: (value, {phase}) => phase === 1
    ? {...value, paused: false} : value});
  assert.match(ignored.audit.errors[0], /Requested pause\/rate state/);
  let pausedSamples = 0;
  const drifting = runControls({transform: (value, {phase}) => {
    if (phase === 1 && ++pausedSamples >= 5)
      return {...value, currentTime: value.currentTime + 1 / 30,
        frameTime: value.frameTime + 1 / 30, identity: value.identity + 1};
    return value;
  }});
  assert.match(drifting.audit.errors[0], /Paused playback changed/);
});

test('rate metadata alone cannot pass if actual playback remains at 1x', () => {
  const {audit} = runControls({actualRate: () => 1});
  assert.match(audit.errors[0], /did not match 0.5x/);
});

test('every requested speed and the final 1x restoration are independently mandatory', () => {
  for (const rate of [0.5, 1.5, 2]) {
    const {audit} = runControls({actualRate: selected => selected === rate ? 1 : selected});
    assert.match(audit.errors[0], new RegExp(`did not match ${rate}x`));
  }
  const ignoredRestore = runControls({transform: (value, {phase}) => phase === 6
    ? {...value, playbackRate: 2} : value});
  assert.match(ignoredRestore.audit.errors[0], /Requested pause\/rate state.*restored-1x/);
});

test('resumed metadata cannot conceal a frozen actual picture', () => {
  let frozen;
  const {audit} = runControls({transform: (value, {phase}) => {
    if (phase !== 2) return value;
    frozen ||= value;
    return {...value, frameTime: frozen.frameTime, identity: frozen.identity};
  }});
  assert.match(audit.errors[0], /Actual picture\/time progression.*resumed-1x/);
});

test('advancing actual pictures cannot conceal a frozen media clock', () => {
  let frozen;
  const {audit} = runControls({transform: (value, {phase}) => {
    if (phase !== 2) return value;
    frozen ||= value;
    return {...value, currentTime: frozen.currentTime};
  }});
  assert.match(audit.errors[0], /Actual picture\/time progression.*resumed-1x/);
});

test('controls reject a wrong picture even when time and selected rate agree', () => {
  const {audit} = runControls({transform: (value, {phase}) => phase === 3
    ? {...value, identity: value.identity + 1} : value});
  assert.match(audit.errors[0], /Invalid actual-frame sample.*half-speed/);
});

test('control observations cannot skip a phase through sparse or regressing clock samples', () => {
  const controller = createPlaybackControlAuditor();
  controller.observe(sample(0, {wallTime: 0, currentTime: 0,
    paused: false, playbackRate: 1}));
  controller.observe(sample(30, {wallTime: 1000, currentTime: 1,
    paused: false, playbackRate: 1}));
  assert.match(controller.audit.errors[0], /observation continuity/);
  const regressing = runControls({transform: (value, {step}) => step === 3
    ? {...value, wallTime: 0} : value});
  assert.match(regressing.audit.errors[0], /clock regressed/);
});

test('control timeout, unexpected seek, and early EOS never count as completion', () => {
  const controller = createPlaybackControlAuditor();
  controller.observe(sample(0, {wallTime: 0, currentTime: 0,
    paused: true, playbackRate: 1}));
  controller.observe(sample(0, {wallTime: 6000, currentTime: 0,
    paused: true, playbackRate: 1}));
  assert.match(controller.audit.errors[0], /within five seconds/);
  for (const changed of [{seeking: true}, {ended: true}]) {
    const {audit} = runControls({transform: (value, {phase}) => phase === 3
      ? {...value, ...changed} : value});
    assert.match(audit.errors[0], /Unexpected seek or EOS/);
  }
});

test('final control validation rejects missing phases and forged progression summaries', () => {
  const good = runControls().audit;
  for (const mutate of [
    value => value.phases.pop(),
    value => { value.phases[1].lastIdentity += 1; },
    value => { value.phases[3].frameSeconds = 0; },
    value => { value.phases[4].timeSeconds = 0; },
    value => { value.phases[5].advances = 0; },
    value => { value.phases[6].wallSeconds = 0; },
  ]) {
    const bad = structuredClone(good);
    mutate(bad);
    assert.throws(() => validatePlaybackControls(bad));
  }
});
