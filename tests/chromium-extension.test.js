// SPDX-License-Identifier: LGPL-2.1-or-later
'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const extensionDirectory = path.join(__dirname, '../browser/h264-only');
const source = fs.readFileSync(path.join(extensionDirectory, 'prefer-h264.js'), 'utf8');

function harness() {
  const calls = {source: [], element: [], capabilities: []};
  const nativeInfo = {supported: true, smooth: false, powerEfficient: false};
  const nativePromise = Promise.resolve(nativeInfo);
  const sandbox = {
    MediaSource: {
      isTypeSupported(...args) {
        calls.source.push({receiver: this, args});
        return true;
      },
    },
    HTMLMediaElement: class {
      canPlayType(...args) {
        calls.element.push({receiver: this, args});
        return 'probably';
      }
    },
    navigator: {
      mediaCapabilities: {
        decodingInfo(...args) {
          calls.capabilities.push({receiver: this, args});
          return nativePromise;
        },
      },
    },
  };
  return {sandbox, calls, nativePromise, nativeInfo,
    run: () => vm.runInNewContext(source, sandbox, {filename: 'prefer-h264.js'})};
}

const allowedTypes = [
  'video/mp4; codecs="avc1.42e01e"',
  'video/mp4; codecs="avc1.4d401f"; framerate=29.97',
  'video/mp4; codecs="avc1.640028, mp4a.40.2"; framerate="30"',
  'VIDEO/MP4; CODECS="AVC1.640028"; FRAMERATE=24',
  'audio/mp4; codecs="mp4a.40.2"',
  'audio/webm; codecs="opus"',
  'application/octet-stream',
];

for (const contentType of allowedTypes) {
  test(`forwards native support queries unchanged: ${contentType}`, () => {
    const {sandbox, calls, run} = harness();
    run();
    const receiver = {purpose: 'preserve the native receiver'};
    const extraArgument = {purpose: 'preserve every argument'};
    assert.equal(sandbox.MediaSource.isTypeSupported.call(receiver,
      contentType, extraArgument), true);
    assert.equal(sandbox.HTMLMediaElement.prototype.canPlayType.call(receiver,
      contentType, extraArgument), 'probably');
    for (const callList of [calls.source, calls.element]) {
      assert.equal(callList.length, 1);
      assert.equal(callList[0].receiver, receiver);
      assert.deepEqual(callList[0].args, [contentType, extraArgument]);
    }
  });
}

const rejectedTypes = [
  'video/webm',
  'VIDEO/WEBM; codecs="opus"',
  'video/mp4; codecs="vp8"',
  'video/mp4; codecs="vp08.00.10.08"',
  'video/mp4; codecs="vp9"',
  'video/mp4; codecs="vp09.00.10.08"',
  'video/mp4; codecs="AV01.0.04M.08"',
  'video/mp4; codecs="avc1.640028, vp09.00.10.08"',
  'video/mp4; codecs="avc1.640028"; framerate=30.01',
  'video/mp4; codecs="avc1.640028"; framerate="60"',
];

for (const contentType of rejectedTypes) {
  test(`rejects unsupported video without consulting native APIs: ${contentType}`, async () => {
    const {sandbox, calls, run} = harness();
    run();
    assert.equal(sandbox.MediaSource.isTypeSupported(contentType), false);
    const element = new sandbox.HTMLMediaElement();
    assert.equal(element.canPlayType(contentType), '');
    const result = await sandbox.navigator.mediaCapabilities.decodingInfo({
      type: 'media-source', video: {contentType, framerate: 30},
    });
    // The returned object belongs to the isolated browser realm.
    assert.deepEqual({...result}, {
      supported: false, smooth: false, powerEfficient: false,
    });
    assert.deepEqual(calls, {source: [], element: [], capabilities: []});
  });
}

test('H.264 capability queries preserve resolution, configuration, receiver, and native promise', async () => {
  const {sandbox, calls, nativePromise, nativeInfo, run} = harness();
  run();
  const receiver = {purpose: 'native capability receiver'};
  const extraArgument = {purpose: 'native capability argument'};
  for (const [width, height] of [[854, 480], [1280, 720], [1920, 1080]]) {
    for (const framerate of [24, 25, 29.97, 30]) {
      const video = Object.freeze({contentType: 'video/mp4; codecs="avc1.640028"',
        width, height, bitrate: 8000000, framerate});
      const configuration = Object.freeze({type: 'media-source', video});
      const result = sandbox.navigator.mediaCapabilities.decodingInfo.call(
        receiver, configuration, extraArgument);
      assert.equal(result, nativePromise);
      assert.equal(await result, nativeInfo);
      const call = calls.capabilities.at(-1);
      assert.equal(call.receiver, receiver);
      assert.equal(call.args[0], configuration);
      assert.deepEqual(call.args, [configuration, extraArgument]);
    }
  }
  assert.equal(calls.capabilities.length, 12);
});

test('capability configuration rejects frame rates above 30 independently of MIME parameters', async () => {
  const {sandbox, calls, run} = harness();
  run();
  for (const framerate of [30.001, 50, 59.94, 60, '60']) {
    const result = await sandbox.navigator.mediaCapabilities.decodingInfo({
      type: 'media-source',
      video: {contentType: 'video/mp4; codecs="avc1.640028"', framerate},
    });
    assert.deepEqual({...result}, {
      supported: false, smooth: false, powerEfficient: false,
    });
  }
  assert.equal(calls.capabilities.length, 0);
});

test('audio-only, absent, and null arguments are forwarded rather than fabricated as supported', () => {
  const {sandbox, calls, nativePromise, run} = harness();
  run();
  for (const args of [[], [undefined], [null], ['']]) {
    sandbox.MediaSource.isTypeSupported(...args);
    sandbox.HTMLMediaElement.prototype.canPlayType(...args);
    assert.deepEqual(calls.source.at(-1).args, args);
    assert.deepEqual(calls.element.at(-1).args, args);
    assert.equal(sandbox.navigator.mediaCapabilities.decodingInfo(...args), nativePromise);
    assert.deepEqual(calls.capabilities.at(-1).args, args);
  }
  const configuration = {type: 'media-source', audio: {
    contentType: 'audio/mp4; codecs="mp4a.40.2"', channels: '2', samplerate: 48000,
  }};
  assert.equal(sandbox.navigator.mediaCapabilities.decodingInfo(configuration), nativePromise);
  assert.equal(calls.capabilities.at(-1).args[0], configuration);
});

test('native unsupported results, synchronous exceptions, and promise rejections are preserved', async () => {
  const state = harness();
  const failure = new TypeError('native validation failure');
  state.sandbox.MediaSource.isTypeSupported = () => false;
  state.sandbox.HTMLMediaElement.prototype.canPlayType = () => '';
  state.sandbox.navigator.mediaCapabilities.decodingInfo = () => Promise.reject(failure);
  state.run();
  assert.equal(state.sandbox.MediaSource.isTypeSupported(allowedTypes[0]), false);
  assert.equal(state.sandbox.HTMLMediaElement.prototype.canPlayType(allowedTypes[0]), '');
  await assert.rejects(state.sandbox.navigator.mediaCapabilities.decodingInfo({
    video: {contentType: allowedTypes[0], framerate: 30},
  }), (error) => error === failure);

  const throwing = harness();
  throwing.sandbox.MediaSource.isTypeSupported = () => { throw failure; };
  throwing.sandbox.HTMLMediaElement.prototype.canPlayType = () => { throw failure; };
  throwing.sandbox.navigator.mediaCapabilities.decodingInfo = () => { throw failure; };
  throwing.run();
  assert.throws(() => throwing.sandbox.MediaSource.isTypeSupported(allowedTypes[0]),
    (error) => error === failure);
  assert.throws(() => throwing.sandbox.HTMLMediaElement.prototype.canPlayType(allowedTypes[0]),
    (error) => error === failure);
  assert.throws(() => throwing.sandbox.navigator.mediaCapabilities.decodingInfo({
    video: {contentType: allowedTypes[0], framerate: 30},
  }), (error) => error === failure);
});

test('missing or non-callable optional media APIs do not prevent initialization', () => {
  for (const sandbox of [
    {},
    {navigator: {}},
    {MediaSource: null, HTMLMediaElement: null, navigator: null},
    {MediaSource: {}, HTMLMediaElement: {}, navigator: {mediaCapabilities: {}}},
    {MediaSource: {isTypeSupported: true}, HTMLMediaElement: {prototype: {canPlayType: 1}},
      navigator: {mediaCapabilities: {decodingInfo: 'unavailable'}}},
  ]) {
    assert.doesNotThrow(() => vm.runInNewContext(source, sandbox));
    assert.equal(Object.hasOwn(sandbox, '__crystalHdSeekGate'), false);
  }
});

test('codec filtering never accesses player/DOM state or schedules presentation work', () => {
  const {sandbox, run} = harness();
  const forbidden = (name) => () => assert.fail(`unexpected presentation access: ${name}`);
  // Reading document/window/video state could reach quality setters, opacity,
  // poster styles, or seek listeners. No such access belongs in a codec filter,
  // including during document_start before a document element exists.
  for (const name of ['document', 'window', 'HTMLVideoElement']) {
    Object.defineProperty(sandbox, name, {get: forbidden(name)});
  }
  for (const name of ['setTimeout', 'clearTimeout', 'setInterval', 'clearInterval',
    'requestAnimationFrame', 'cancelAnimationFrame', 'queueMicrotask',
    'MutationObserver', 'addEventListener']) {
    sandbox[name] = forbidden(name);
  }
  run();
  sandbox.MediaSource.isTypeSupported(allowedTypes[0]);
  sandbox.HTMLMediaElement.prototype.canPlayType(allowedTypes[0]);
  sandbox.navigator.mediaCapabilities.decodingInfo({
    video: {contentType: allowedTypes[0], width: 1920, height: 1080, framerate: 30},
  });
  assert.equal(Object.hasOwn(sandbox, '__crystalHdSeekGate'), false);
});

test('manifest publishes codec-only update on the existing YouTube scope', () => {
  const manifest = JSON.parse(fs.readFileSync(path.join(extensionDirectory, 'manifest.json')));
  assert.equal(manifest.manifest_version, 3);
  assert.equal(manifest.version, '1.6.0');
  assert.doesNotMatch(manifest.description, /gate|mask/i);
  assert.deepEqual(manifest.content_scripts, [{
    matches: ['*://*.youtube.com/*'], js: ['prefer-h264.js'],
    run_at: 'document_start', world: 'MAIN',
  }]);
  assert.equal(manifest.permissions, undefined);
});
