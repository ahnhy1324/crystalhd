// SPDX-License-Identifier: LGPL-2.1-or-later
'use strict';

const assert = require('node:assert/strict');
const {spawn} = require('node:child_process');
const http = require('node:http');
const path = require('node:path');
const test = require('node:test');
const {WebSocketServer} = require('ws');

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
          currentTime: mode === 'stalled' ? 2 : sample,
          paused: false, ended: false, readyState: 4,
          width: 640, height: 360, totalFrames: sample * 30, droppedFrames: 0,
          youtubeCodecs: 'avc1.64001e', youtubeResolution: '640x360@30',
          presentedFrames: sample * 30, presentedRegressions: [],
          seekSettled: false, seekViolations: [], seekGateAvailable: false,
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
  assert.match(result.stderr, /did not sustain H.264 playback/);
});

test('stalled software playback fails', {timeout: 10000}, async () => {
  const result = await runProbe('stalled', 2);
  assert.equal(result.code, 1, result.stdout);
  assert.match(result.stderr, /did not sustain H.264 playback/);
});
