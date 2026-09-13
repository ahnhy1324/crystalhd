// SPDX-License-Identifier: LGPL-2.1-or-later
'use strict';

const assert = require('node:assert/strict');
const {spawn, spawnSync} = require('node:child_process');
const http = require('node:http');
const path = require('node:path');
const test = require('node:test');
const {WebSocketServer} = require('ws');
const {PlaybackHealth, hardwareContinuity, safeDiagnostic,
       readPlayerDiagnostics, DEFAULT_SECONDS} = require('./chromium-youtube');

async function runProbe(mode, seconds) {
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
      socket.send(JSON.stringify({id: command.id, result}));
    });
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  try {
    const child = spawn(process.execPath, [
      path.join(__dirname, 'chromium-youtube.js'),
      '--port', String(server.address().port), '--seconds', String(seconds), url,
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
