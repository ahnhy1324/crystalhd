#!/usr/bin/env node
// SPDX-License-Identifier: LGPL-2.1-or-later
'use strict';

// Real browser test, not part of unattended make check. Use the fixture from
// generate-browser-sample.sh; a different video must fail the pixel audit.
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');
const {spawn} = require('node:child_process');
const WebSocket = require('ws');
const {createFrameAuditor, validateBrowserAudit} = require('./chromium-local-audit');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const args = process.argv.slice(2);
const hardware = args[1] === '--expect-hardware';
if (args.length < 1 || args.length > 2 || (args[1] && !hardware)) {
  console.error('usage: chromium-local-playback.js BARCODE.mp4 [--expect-hardware]');
  process.exit(2);
}
if (hardware && process.env.CRYSTALHD_CHROMIUM_DISABLE_GPU_SANDBOX !== '1') {
  console.error('Experimental hardware requires explicit GPU sandbox opt-out acknowledgement.');
  process.exit(2);
}
const videoPath = path.resolve(args[0]);
const videoSize = fs.statSync(videoPath).size;

function pageAudit(createAuditor) {
  const video = document.querySelector('video');
  const canvas = document.createElement('canvas');
  canvas.width = 288;
  canvas.height = 32;
  const ctx = canvas.getContext('2d', {willReadFrequently: true});
  const auditor = createAuditor();
  const audit = window.audit = auditor.audit;
  video.addEventListener('error', () => audit.errors.push(String(video.error?.message)));
  function snapshot(callbackTime) {
    // Retain pixels and their own timestamp until the entire CPU readback is
    // complete. Never substitute requestVideoFrameCallback's older timestamp.
    const frame = new VideoFrame(video);
    try {
      ctx.drawImage(frame, 0, 0);
      const pixels = ctx.getImageData(0, 16, 288, 1).data;
      const luma = x => (pixels[x * 4] + pixels[x * 4 + 1] + pixels[x * 4 + 2]) / 3;
      let identity = 0;
      for (let bit = 0; bit < 9; ++bit)
        if (luma(16 + 24 * bit) > 128) identity |= 1 << bit;
      return {frameTime: frame.timestamp / 1000000, callbackTime, identity,
        white: luma(248), black: luma(272), seeking: video.seeking};
    } finally { frame.close(); }
  }
  video.addEventListener('ended', () => {
    try { auditor.end(snapshot(null), video.duration); }
    catch (error) { audit.errors.push(String(error)); }
  });
  const observe = (_, metadata) => {
    try {
      if (!video.seeking) {
        const target = auditor.observe(snapshot(metadata.mediaTime));
        if (target !== undefined) video.currentTime = target;
      }
    } catch (error) { audit.errors.push(String(error)); }
    video.requestVideoFrameCallback(observe);
  };
  if (!video.requestVideoFrameCallback || typeof VideoFrame === 'undefined') {
    audit.errors.push('requestVideoFrameCallback or VideoFrame is unavailable');
    return;
  }
  video.requestVideoFrameCallback(observe);
  video.play().catch(error => audit.errors.push(String(error)));
}

const html = `<!doctype html><video muted width="640" height="360" src="/sample.mp4"></video>
<script>(${pageAudit.toString()})(${createFrameAuditor.toString()})</script>`;
const server = http.createServer((request, response) => {
  if (request.url !== '/sample.mp4') {
    response.writeHead(200, {'Content-Type': 'text/html'}).end(html);
    return;
  }
  let start = 0, end = videoSize - 1;
  if (request.headers.range) {
    const match = /^bytes=(\d+)-(\d*)$/.exec(request.headers.range);
    if (!match) { response.writeHead(416).end(); return; }
    start = Number(match[1]);
    if (match[2]) end = Math.min(Number(match[2]), end);
    if (start > end || start >= videoSize) { response.writeHead(416).end(); return; }
  }
  const headers = {'Content-Type': 'video/mp4', 'Accept-Ranges': 'bytes',
    'Content-Length': end - start + 1};
  if (request.headers.range) headers['Content-Range'] = `bytes ${start}-${end}/${videoSize}`;
  response.writeHead(request.headers.range ? 206 : 200, headers);
  const stream = fs.createReadStream(videoPath, {start, end});
  stream.on('error', error => response.destroy(error));
  response.on('close', () => stream.destroy());
  stream.pipe(response);
});
let browser, socket, profile;
const pending = new Map();
let nextId = 0;
function call(method, params = {}) {
  return new Promise((resolve, reject) => {
    const id = ++nextId;
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error(`${method} timed out`));
    }, 10000);
    pending.set(id, {resolve, reject, timer});
    socket.send(JSON.stringify({id, method, params}));
  });
}
async function main() {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  profile = fs.mkdtempSync(path.join(os.tmpdir(), 'crystalhd-browser-audit-'));
  const env = {...process.env, CRYSTALHD_CHROMIUM_CONFIG: '/dev/null',
    CRYSTALHD_CHROMIUM_PROFILE_DIR: profile,
    CRYSTALHD_CHROMIUM_EXPERIMENTAL_HW_DECODE: hardware ? '1' : '0'};
  browser = spawn(path.resolve(__dirname, '../scripts/crystalhd-chromium'), [
    '--remote-debugging-port=0', '--remote-allow-origins=http://localhost',
    '--disable-background-networking', '--disable-component-update',
    '--disable-sync', '--autoplay-policy=no-user-gesture-required', 'about:blank',
  ], {env, detached: true, stdio: ['ignore', 'ignore', 'inherit']});
  let launchError;
  browser.once('error', error => { launchError = error; });
  let port;
  for (let attempt = 0; attempt < 200; ++attempt) {
    if (launchError) throw launchError;
    if (browser.exitCode !== null) throw new Error(`Browser exited ${browser.exitCode}`);
    try {
      port = Number(fs.readFileSync(path.join(profile, 'DevToolsActivePort'), 'utf8').split('\n')[0]);
      if (port) break;
    } catch (_) { /* Startup is still in progress. */ }
    await delay(100);
  }
  if (!port) throw new Error('Browser DevTools startup timed out');
  const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`,
    {signal: AbortSignal.timeout(10000)})).json();
  const target = targets.find(target => target.type === 'page');
  if (!target) throw new Error('No browser page');
  socket = new WebSocket(target.webSocketDebuggerUrl,
    {origin: 'http://localhost', handshakeTimeout: 10000});
  await new Promise((resolve, reject) => {
    socket.once('open', resolve);
    socket.once('error', reject);
  });
  const decoders = [];
  const platformDecoders = [];
  const mediaErrors = [];
  socket.on('message', data => {
    const message = JSON.parse(data.toString());
    if (message.id && pending.has(message.id)) {
      const reply = pending.get(message.id);
      pending.delete(message.id);
      clearTimeout(reply.timer);
      if (message.error) reply.reject(new Error(JSON.stringify(message.error)));
      else reply.resolve(message.result);
    }
    if (message.method === 'Media.playerPropertiesChanged') {
      for (const property of message.params.properties || []) {
        if (property.name === 'kVideoDecoderName') decoders.push(property.value);
        if (property.name === 'kIsPlatformVideoDecoder') platformDecoders.push(property.value);
      }
    }
    if (message.method === 'Media.playerErrorsRaised')
      mediaErrors.push(...message.params.errors);
  });
  await call('Runtime.enable');
  await call('Media.enable');
  await call('Page.navigate', {url: `http://127.0.0.1:${server.address().port}/`});
  let audit;
  const deadline = Date.now() + 90000;
  for (let elapsed = 0; Date.now() < deadline; ++elapsed) {
    await delay(1000);
    const result = await call('Runtime.evaluate', {expression: 'window.audit', returnByValue: true});
    audit = result.result?.value;
    if (audit && (audit.done || audit.errors.length || audit.mismatches.length)) break;
    if ((elapsed + 1) % 10 === 0)
      console.log(`Browser audit ${elapsed + 1}s: ${audit?.frames || 0} frames, ${audit?.segments.length || 0} segments`);
  }
  const result = {hardware, decoders, platformDecoders, mediaErrors, audit};
  console.log(JSON.stringify(result, null, 2));
  validateBrowserAudit(result);
  console.log('Browser pixel identity and forward/backward seek audit passed');
}
let cleanupPromise;
function cleanup() {
  if (cleanupPromise) return cleanupPromise;
  cleanupPromise = cleanupOnce();
  return cleanupPromise;
}
async function cleanupOnce() {
  for (const reply of pending.values()) clearTimeout(reply.timer);
  if (socket) socket.terminate();
  if (browser?.pid) {
    try { process.kill(-browser.pid, 'SIGTERM'); } catch (_) {}
    await delay(1000);
    try { process.kill(-browser.pid, 'SIGKILL'); } catch (_) {}
  }
  server.closeAllConnections();
  server.close();
  if (profile) fs.rmSync(profile, {recursive: true, force: true});
}
// An outer timeout must not leave this test's detached browser running.
for (const signal of ['SIGINT', 'SIGTERM']) {
  process.once(signal, () => {
    process.exitCode = 1;
    cleanup().finally(() => process.exit(1));
  });
}
main().catch(error => {
  console.error(`chromium-local-playback: ${error.message}`);
  process.exitCode = 1;
}).finally(cleanup);
