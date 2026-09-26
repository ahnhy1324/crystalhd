// SPDX-License-Identifier: LGPL-2.1-or-later
'use strict';

const assert = require('node:assert/strict');
const {spawn, spawnSync} = require('node:child_process');
const http = require('node:http');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');
const {WebSocketServer} = require('ws');
const {PlaybackHealth, hardwareContinuity, safeDiagnostic,
       readPlayerDiagnostics, observeSeekFrame, DEFAULT_SECONDS} = require('./chromium-youtube');

async function runProbe(mode, seconds, extraArguments = []) {
  const url = 'https://example.invalid/crystalhd-probe-test';
  const server = http.createServer((request, response) => {
    response.setHeader('Content-Type', 'application/json');
    response.end(JSON.stringify([{
      type: 'page',
      webSocketDebuggerUrl: `ws://127.0.0.1:${server.address().port}/devtools/page/test`,
    }]));
  });
  const sockets = new WebSocketServer({server});
  sockets.on('connection', (socket) => {
    let sample = 0;
    let mediaTime = 0;
    let pendingSeek = false;
    let frameCallbacks = [];
    const seekedListeners = [];
    const video = {
      readyState: 4, duration: 600, paused: false, ended: false, seeking: false,
      videoWidth: 640, videoHeight: 360, error: null,
      get currentTime() { return mediaTime; },
      set currentTime(value) {
        if (mode !== 'ignored-seek') {
          mediaTime = value;
          pendingSeek = true;
          this.seeking = true;
        }
      },
      play: () => Promise.resolve(),
      getVideoPlaybackQuality: () => ({totalVideoFrames: sample * 30, droppedVideoFrames: 0}),
      requestVideoFrameCallback: (callback) => frameCallbacks.push(callback),
      addEventListener: (name, callback) => {
        assert.equal(name, 'seeked');
        seekedListeners.push(callback);
      },
    };
    const page = vm.createContext({
      document: {
        querySelector: () => video,
        querySelectorAll: () => [],
        getElementById: () => ({getStatsForNerds: () => ({
          codecs: 'avc1.64001e', resolution: '640x360@30',
        })}),
      },
      performance: {now: () => sample * 1000},
    });
    socket.on('message', (data) => {
      const command = JSON.parse(data);
      let result = {};
      if (command.method === 'Media.enable') {
        socket.send(JSON.stringify({
          method: 'Media.playerPropertiesChanged',
          params: {playerId: 'test', properties: [
            {name: 'kFrameUrl', value: url},
            {name: 'kVideoTracks', value: '[{"codec":"h264"}]'},
            {name: 'kVideoDecoderName', value: 'FFmpegVideoDecoder'},
            {name: 'kIsPlatformVideoDecoder', value: 'false'},
          ]},
        }));
      }
      if (command.method === 'Runtime.evaluate') {
        if (mode === 'ignored-seek' || mode === 'working-seek') {
          // Execute the real serialized page code, not a fabricated settled
          // result. A failed seek setter lets ordinary playback continue.
          if (!command.params.expression.includes('audit.seek = {')) {
            ++sample;
            if (pendingSeek) {
              pendingSeek = false;
              video.seeking = false;
              for (const callback of seekedListeners.splice(0))
                callback();
            } else {
              ++mediaTime;
            }
            const callbacks = frameCallbacks;
            frameCallbacks = [];
            for (const callback of callbacks)
              callback(sample * 1000, {mediaTime});
          }
          result = {result: {value: vm.runInContext(command.params.expression, page)}};
        } else {
        ++sample;
        result = {result: {value: mode === 'absent' ? null : {
          videoPresent: true, videoId: 1, appError: false, mediaError: null,
          currentTime: mode === 'stalled' ? 2 : sample,
          paused: false, ended: false, readyState: 4, seeking: false, duration: 600,
          width: 640, height: 360, totalFrames: sample * 30, droppedFrames: 0,
          youtubeCodecs: 'avc1.64001e', youtubeResolution: '640x360@30',
          presentedFrames: sample * 30, presentedMediaTime: sample,
          presentedRegressions: [], seekSettled: false, seekViolations: [],
        }}};
        }
      }
      socket.send(JSON.stringify({id: command.id, result}));
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  try {
    const child = spawn(process.execPath, [
      path.join(__dirname, 'chromium-youtube.js'),
      '--port', String(server.address().port), '--seconds', String(seconds), ...extraArguments, url,
    ], {stdio: ['ignore', 'pipe', 'pipe']});
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (data) => { stdout += data; });
    child.stderr.on('data', (data) => { stderr += data; });
    const code = await new Promise((resolve, reject) => {
      child.once('error', reject);
      child.once('close', resolve);
    });
    return {code, stdout, stderr};
  } finally {
    for (const socket of sockets.clients)
      socket.terminate();
    await new Promise((resolve) => sockets.close(resolve));
    await new Promise((resolve) => server.close(resolve));
  }
}

test('software playback must advance and present frames', {timeout: 15000}, async () => {
  const result = await runProbe('playing', 7);
  assert.equal(result.code, 0, result.stderr);
  assert.match(result.stdout, /FFmpegVideoDecoder \(platform=false\)/);
});

test('ignored forward seek cannot pass by naturally crossing its midpoint', {timeout: 15000}, async () => {
  const result = await runProbe('ignored-seek', 8, ['--seek-at', '2', '--seek-to', '6']);
  assert.equal(result.code, 1, result.stdout);
  assert.match(result.stderr, /seek/i);
});

test('completed forward seek passes the real injected event/frame callback path', {timeout: 15000}, async () => {
  const result = await runProbe('working-seek', 8, ['--seek-at', '2', '--seek-to', '30']);
  assert.equal(result.code, 0, result.stderr);
  assert.match(result.stdout, /Seek: 1\.00s -> 30\.00s/);
});

function seekState(from = 20, target = 120) {
  return {target, cutoff: (from + target) / 2, forward: target >= from,
    settled: false, seeked: false, failure: null, deadline: 5000, violations: []};
}

test('seek settlement needs a seeked event and an actual near-target timestamp', () => {
  const seek = seekState();
  observeSeekFrame(seek, 120, 1000);
  assert.equal(seek.settled, false, 'timestamp alone cannot validate an ignored seek');
  seek.seeked = true;
  observeSeekFrame(seek, 75, 1100);
  assert.equal(seek.settled, false, 'crossing midpoint is not target arrival');
  observeSeekFrame(seek, 120.033, 1200);
  assert.equal(seek.settled, true);
  observeSeekFrame(seek, 21, 1300);
  assert.deepEqual(seek.violations, [{mediaTime: 21, cutoff: 70}]);
});

test('backward seek requires the requested target rather than merely the new side', () => {
  const seek = seekState(120, 20);
  seek.seeked = true;
  observeSeekFrame(seek, 65, 1000);
  assert.equal(seek.settled, false);
  observeSeekFrame(seek, 20, 1100);
  assert.equal(seek.settled, true);
});

test('expired seek cannot settle late or be hidden by ordinary playback', () => {
  const seek = seekState();
  seek.seeked = true;
  observeSeekFrame(seek, null, 5001);
  assert.match(seek.failure, /deadline/);
  observeSeekFrame(seek, 120, 5002);
  assert.equal(seek.settled, false);
  assert.throws(() => established().observe(picture(9, {seekFailure: seek.failure}), 9000),
                /requested seek.*deadline/);
});

test('missing video fails even without --expect-hardware', {timeout: 10000}, async () => {
  const result = await runProbe('absent', 1);
  assert.equal(result.code, 1, result.stdout);
  assert.match(result.stderr, /did not sustain playback/);
});

test('stalled software playback fails', {timeout: 10000}, async () => {
  const result = await runProbe('stalled', 2);
  assert.equal(result.code, 1, result.stdout);
  assert.match(result.stderr, /did not sustain playback/);
});

function picture(time, extra = {}) {
  return {videoPresent: true, videoId: 1, readyState: 4,
    currentTime: time, duration: 600, paused: false, ended: false, seeking: false,
    width: 1920, height: 1080, totalFrames: time * 30,
    presentedFrames: time * 30, presentedMediaTime: time, ...extra};
}

function established() {
  const health = new PlaybackHealth();
  for (let second = 1; second <= 8; ++second)
    health.observe(picture(second), second * 1000);
  assert.equal(health.finish(8000), false);
  return health;
}

test('default observation covers the reported 45-second failure window', () => {
  assert.equal(DEFAULT_SECONDS, 90);
});

test('playing then disappearing fails despite earlier sufficient progress', () => {
  const health = established();
  assert.throws(() => health.observe({videoPresent: false, readyState: 0}, 45000),
                /disappeared/);
});

test('visible application error and media-element error both fail late', () => {
  assert.throws(() => established().observe(picture(45, {appError: true}), 45000),
                /visible YouTube player error/);
  assert.throws(() => established().observe(picture(45, {mediaError: 3}), 45000),
                /video element error \(code 3\)/);
});

test('application status fails without DOM or MediaError and preserves first error', () => {
  const health = established();
  assert.throws(() => health.observe(picture(45, {
    appError: false, mediaError: null, playerErrorCode: 'ump.spsrejectfailure',
    playerDebugStatus: 'HTML5_SPS_UMP_STATUS_REJECTED',
  }), 45000), /ump\.spsrejectfailure; HTML5_SPS_UMP_STATUS_REJECTED/);
  assert.throws(() => health.observe(picture(46, {appError: true}), 46000),
                /ump\.spsrejectfailure/);
  assert.throws(() => health.finish(46000), /ump\.spsrejectfailure/);
});

test('player diagnostics export only whitelisted status, not private payloads', () => {
  const result = readPlayerDiagnostics({
    getVideoData: () => ({errorCode: 'ump.spsrejectfailure', privateToken: 'PRIVATE'}),
    getVideoStats: () => ({cpn: 'PRIVATE', visitor: 'PRIVATE', debug_error: JSON.stringify({
      errorCode: 'ump.spsrejectfailure', args: ['HTML5_SPS_UMP_STATUS_REJECTED'],
      url: 'https://example.invalid/videoplayback?signature=PRIVATE',
    })}),
  });
  assert.deepEqual(result, {playerErrorCode: 'ump.spsrejectfailure',
    playerDebugStatus: 'HTML5_SPS_UMP_STATUS_REJECTED'});
  assert.doesNotMatch(JSON.stringify(result), /PRIVATE|https|cpn|visitor/);
  assert.deepEqual(readPlayerDiagnostics({getVideoData: () => ({errorCode: 'PRIVATE'})}),
                   {playerErrorCode: 'unclassified-player-error', playerDebugStatus: null});
  assert.deepEqual(readPlayerDiagnostics(null),
                   {playerErrorCode: null, playerDebugStatus: null});
});

test('late stall fails despite earlier sufficient progress', () => {
  const health = established();
  for (let second = 9; second <= 13; ++second)
    health.observe(picture(8), second * 1000);
  assert.throws(() => health.observe(picture(8), 14000), /media time stopped/);
});

test('time alone cannot conceal frozen frame callbacks', () => {
  const health = established();
  for (let second = 9; second <= 13; ++second)
    health.observe(picture(second, {presentedFrames: 240, presentedMediaTime: 8}),
                   second * 1000);
  assert.throws(() => health.observe(picture(14,
      {presentedFrames: 240, presentedMediaTime: 8}), 14000), /frame callbacks stopped/);
});

test('reported frame counts without advancing rVFC timestamps cannot pass', () => {
  const health = established();
  for (let second = 9; second <= 13; ++second)
    health.observe(picture(second, {presentedMediaTime: 8}), second * 1000);
  assert.throws(() => health.observe(picture(14, {presentedMediaTime: 8}), 14000),
                /frame callbacks stopped/);
});

test('final sample must still be healthy, not merely recently successful', () => {
  const health = established();
  health.observe(picture(9, {paused: true}), 9000);
  assert.throws(() => health.finish(9000), /final sample/);
});

test('final video replacement cannot inherit old frame/time freshness', () => {
  const health = established();
  health.observe(picture(9, {videoId: 2}), 9000);
  assert.throws(() => health.finish(9000), /recent frame and time/);
});

test('replacement first seen before callbacks needs its own observed advancement', () => {
  const health = established();
  health.observe(picture(9, {videoId: 2, presentedFrames: 0,
    presentedMediaTime: null, readyState: 1}), 9000);
  health.observe(picture(10, {videoId: 2}), 10000);
  assert.equal(health.recent(10000), false);
  health.observe(picture(11, {videoId: 2}), 11000);
  assert.equal(health.finish(11000), false);
});

test('forward and backward seeks allow settling but require new advancement', () => {
  const health = established();
  health.beginSeek(8500);
  health.observe(picture(30), 9000);
  assert.throws(() => {
    const incomplete = established();
    incomplete.beginSeek(8500);
    incomplete.observe(picture(30), 9000);
    incomplete.finish(9000);
  }, /recent frame and time/);
  health.observe(picture(31), 10000);
  health.observe(picture(32), 11000);
  assert.equal(health.finish(11000), false);
  health.beginSeek(11500);
  health.observe(picture(2), 12000);
  health.observe(picture(3), 13000);
  assert.equal(health.finish(13000), false);
});

test('natural EOS requires finite duration and observed final rVFC interval', () => {
  const health = established();
  health.observe(picture(9, {duration: 9, paused: true, ended: true,
    presentedMediaTime: 8.97}), 9000);
  assert.equal(health.finish(90000), true);
  assert.throws(() => established().observe(picture(9, {duration: 9,
    paused: true, ended: true, presentedMediaTime: 2}), 9000), /unverified end/);
  const seekEnd = established();
  seekEnd.beginSeek(8500);
  assert.throws(() => seekEnd.observe(picture(600, {paused: true, ended: true}), 9000),
                /unverified end/);
});

test('a verified EOS cannot validate a replaced or cleared video later', () => {
  const health = established();
  health.observe(picture(9, {duration: 9, paused: true, ended: true,
    presentedMediaTime: 8.97}), 9000);
  health.observe(picture(10, {videoId: 2, duration: 10, paused: true, ended: true}), 10000);
  assert.throws(() => health.finish(10000), /completed video identity/);
});

test('hardware expectation rejects software fallback but has no 854-pixel ceiling', () => {
  const player = {properties: {kVideoTracks: '{"codec":"h264","width":1920}',
    kIsPlatformVideoDecoder: 'true'}, decoderHistory: ['VaapiVideoDecoder']};
  assert.equal(hardwareContinuity([player], 'VaapiVideoDecoder', 'true', 30), true);
  player.decoderHistory.push('FFmpegVideoDecoder', 'VaapiVideoDecoder');
  assert.equal(hardwareContinuity([player], 'VaapiVideoDecoder', 'true', 30), false);
  assert.equal(hardwareContinuity([player], 'FFmpegVideoDecoder', 'false', 30), false);
});

test('obsolete seek-mask option is rejected before connecting', () => {
  const result = spawnSync(process.execPath,
      [path.join(__dirname, 'chromium-youtube.js'), '--expect-seek-gate'],
      {encoding: 'utf8', timeout: 3000});
  assert.equal(result.status, 2);
  assert.match(result.stderr, /obsolete; masks do not verify playback or pixel identity/);
});

test('diagnostics redact URL query data and local profile paths', () => {
  const value = safeDiagnostic('https://example.invalid/watch?signature=SECRET /tmp/profile/Default');
  assert.doesNotMatch(value, /SECRET|\/tmp\/profile|https:/);
});
