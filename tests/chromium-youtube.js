#!/usr/bin/env node
// SPDX-License-Identifier: LGPL-2.1-or-later

'use strict';

const http = require('http');
let activeSocket = null;

let WebSocket;
try {
  WebSocket = require('ws');
} catch (_) {
  console.error('chromium-youtube: Node.js module "ws" is required (Ubuntu package: node-ws)');
  process.exit(2);
}

let port = 9223;
let forceH264 = false;
let expectHardware = false;
const DEFAULT_SECONDS = 90;
let seconds = DEFAULT_SECONDS;
const seekSchedule = [];
const seekTimes = [];
let url = 'https://www.youtube.com/watch?v=aqz-KE-bpKQ';
const cliArguments = require.main === module ? process.argv.slice(2) : [];
for (let index = 0; index < cliArguments.length; ++index) {
  const argument = cliArguments[index];
  if (argument === '--force-h264') {
    forceH264 = true;
  } else if (argument === '--expect-hardware') {
    expectHardware = true;
  } else if (argument === '--expect-seek-gate') {
    console.error('chromium-youtube: --expect-seek-gate is obsolete; masks do not verify playback or pixel identity');
    process.exit(2);
  } else if (argument === '--port' && cliArguments[index + 1]) {
    port = Number(cliArguments[++index]);
  } else if (argument === '--seconds' && cliArguments[index + 1]) {
    seconds = Number(cliArguments[++index]);
  } else if (argument === '--seek-at' && cliArguments[index + 1]) {
    seekTimes.push(Number(cliArguments[++index]));
  } else if (argument === '--seek-to' && cliArguments[index + 1]) {
    seekSchedule.push({target: Number(cliArguments[++index])});
  } else if (/^https:\/\//.test(argument)) {
    url = argument;
  } else {
    console.error('chromium-youtube: unknown argument');
    process.exit(2);
  }
}
for (let index = 0; index < seekSchedule.length; ++index)
  seekSchedule[index].at = seekTimes[index];
if (!Number.isInteger(port) || port < 1 || port > 65535 ||
    !Number.isInteger(seconds) || seconds < 1 || seconds > 300 ||
    seekTimes.length !== seekSchedule.length ||
    seekSchedule.some((seek, index) =>
      !Number.isInteger(seek.at) || seek.at < 1 || seek.at >= seconds ||
      !Number.isFinite(seek.target) || seek.target < 0 ||
      (index !== 0 && seek.at <= seekSchedule[index - 1].at))) {
  console.error('chromium-youtube: invalid port, duration or paired seek schedule');
  process.exit(2);
}

function safeDiagnostic(value) {
  return String(value)
      .replace(/(?:https?|file):\/\/[^\s"'<>]+/gi, '[redacted-url]')
      .replace(/\/(?:home|Users|tmp)\/[^\s"'<>]+/g, '[redacted-path]')
      .replace(/[\w.+-]+@[\w.-]+\.[A-Za-z]{2,}/g, '[redacted-email]')
      .slice(0, 512);
}

function playerErrorCode(value) {
  if (!value)
    return null;
  return value === 'ump.spsrejectfailure' ? value : 'unclassified-player-error';
}

// Serialized into the page as well as exercised directly by the mock tests.
function readPlayerDiagnostics(player) {
  let playerErrorCode = null;
  let playerDebugStatus = null;
  try {
    const data = player?.getVideoData?.();
    if (data?.errorCode)
      playerErrorCode = data.errorCode === 'ump.spsrejectfailure'
          ? 'ump.spsrejectfailure' : 'unclassified-player-error';
    const debug = player?.getVideoStats?.().debug_error;
    if (typeof debug === 'string' && debug.length <= 65536) {
      const parsed = JSON.parse(debug);
      if (!playerErrorCode && parsed?.errorCode)
        playerErrorCode = parsed.errorCode === 'ump.spsrejectfailure'
            ? 'ump.spsrejectfailure' : 'unclassified-player-error';
      // Only this known status is exported. Never return videoStats, debug
      // payloads, URLs, CPNs, visitor data, or exception strings.
      if (JSON.stringify(parsed).includes('HTML5_SPS_UMP_STATUS_REJECTED'))
        playerDebugStatus = 'HTML5_SPS_UMP_STATUS_REJECTED';
    }
  } catch (_) { /* Undocumented methods may be absent or change. */ }
  return {playerErrorCode, playerDebugStatus};
}

// This checks live rVFC timestamps, NOT the identity of the displayed pixels.
// Keep the policy independent of CDP so late failures can be tested without a
// browser or real-time sleeps. `now` is monotonic milliseconds from the caller.
class PlaybackHealth {
  constructor() {
    this.previous = null;
    this.latest = null;
    this.videoId = null;
    this.started = false;
    this.progress = 0;
    this.lastTimeAdvance = null;
    this.lastFrameAdvance = null;
    this.lastSeek = -Infinity;
    this.seekGraceUntil = -Infinity;
    this.naturalEnd = false;
    this.endState = null;
    this.failure = null;
  }

  fail(message) {
    this.failure ||= message;
    throw new Error(this.failure);
  }

  beginSeek(now) {
    this.previous = null;
    this.lastSeek = now;
    this.seekGraceUntil = now + 5000;
    this.naturalEnd = false;
    this.endState = null;
  }

  static usable(snapshot) {
    return snapshot && snapshot.videoPresent === true &&
        Number.isInteger(snapshot.videoId) && snapshot.videoId > 0 &&
        snapshot.readyState >= 2 && snapshot.width > 0 && snapshot.height > 0 &&
        Number.isFinite(snapshot.currentTime) &&
        Number.isFinite(snapshot.presentedMediaTime) &&
        Number.isFinite(snapshot.presentedFrames) && snapshot.presentedFrames > 0;
  }

  static playing(snapshot) {
    return PlaybackHealth.usable(snapshot) && !snapshot.paused &&
        !snapshot.ended && !snapshot.seeking;
  }

  recent(now) {
    return this.lastTimeAdvance !== null && this.lastFrameAdvance !== null &&
        now - this.lastTimeAdvance <= 5000 && now - this.lastFrameAdvance <= 5000 &&
        this.lastTimeAdvance >= this.lastSeek && this.lastFrameAdvance >= this.lastSeek;
  }

  observe(snapshot, now) {
    if (this.failure)
      this.fail(this.failure);
    this.latest = snapshot;
    if (snapshot?.playerErrorCode || snapshot?.playerDebugStatus) {
      const code = playerErrorCode(snapshot.playerErrorCode) || 'unclassified-player-error';
      const status = snapshot.playerDebugStatus === 'HTML5_SPS_UMP_STATUS_REJECTED'
          ? '; HTML5_SPS_UMP_STATUS_REJECTED' : '';
      this.fail(`YouTube application error: ${code}${status}`);
    }
    if (snapshot?.appError)
      this.fail('visible YouTube player error');
    if (snapshot?.mediaError)
      this.fail(`video element error (code ${Number(snapshot.mediaError) || 'unknown'})`);
    if (snapshot?.videoPresent && Number.isInteger(snapshot.videoId) && snapshot.videoId > 0) {
      if (this.videoId !== null && this.videoId !== snapshot.videoId) {
        // A replacement may initially have no rVFC sample at all. Earlier
        // video's freshness must never validate this new object's playback.
        this.lastTimeAdvance = -Infinity;
        this.lastFrameAdvance = -Infinity;
        this.seekGraceUntil = Math.max(this.seekGraceUntil, now + 5000);
      }
      this.videoId = snapshot.videoId;
    }
    const usable = PlaybackHealth.usable(snapshot);
    const previous = this.previous;
    if (usable && previous && PlaybackHealth.usable(previous.snapshot) &&
        snapshot.videoId === previous.snapshot.videoId && !snapshot.seeking) {
      const elapsed = (now - previous.now) / 1000;
      const timeDelta = snapshot.currentTime - previous.snapshot.currentTime;
      const frameDelta = snapshot.presentedMediaTime - previous.snapshot.presentedMediaTime;
      const timeAdvanced = timeDelta > 0 && timeDelta <= elapsed + 0.5;
      const framesAdvanced = snapshot.presentedFrames > previous.snapshot.presentedFrames &&
          frameDelta > 0 && frameDelta <= elapsed + 0.5;
      if (timeAdvanced)
        this.lastTimeAdvance = now;
      if (framesAdvanced)
        this.lastFrameAdvance = now;
      if (timeAdvanced && framesAdvanced)
        this.progress += Math.min(timeDelta, frameDelta);
    }
    if (!this.started && PlaybackHealth.playing(snapshot) && snapshot.currentTime > 0) {
      this.started = true;
      this.lastTimeAdvance = now;
      this.lastFrameAdvance = now;
    }
    if (snapshot?.ended && !this.naturalEnd) {
      // Natural EOS is accepted only after real observed progress and a
      // transition from playback to a finite duration's final rVFC interval.
      // Seeking directly to the end or an `ended` flag alone cannot pass.
      const duration = snapshot.duration;
      this.naturalEnd = Boolean(usable && previous &&
          PlaybackHealth.playing(previous.snapshot) &&
          snapshot.videoId === previous.snapshot.videoId &&
          Number.isFinite(duration) && duration > 0 &&
          Math.abs(snapshot.currentTime - duration) <= 0.1 &&
          snapshot.presentedMediaTime >= duration - 0.25 &&
          snapshot.presentedMediaTime <= duration && this.progress >= 5 &&
          this.recent(now));
      if (!this.naturalEnd)
        this.fail('unverified end of playback');
      this.endState = {videoId: snapshot.videoId, duration,
        frames: snapshot.presentedFrames, mediaTime: snapshot.presentedMediaTime};
    }
    if (this.started && !this.naturalEnd && now > this.seekGraceUntil) {
      if (!snapshot?.videoPresent)
        this.fail('video element disappeared after playback began');
      if (now - this.lastTimeAdvance > 5000)
        this.fail('playback stalled: media time stopped advancing');
      if (now - this.lastFrameAdvance > 5000)
        this.fail('playback stalled: frame callbacks stopped advancing');
    }
    this.previous = snapshot ? {snapshot, now} : null;
  }

  finish(now) {
    if (this.failure)
      this.fail(this.failure);
    const snapshot = this.latest;
    const verifiedEnd = this.naturalEnd && PlaybackHealth.usable(snapshot) &&
        snapshot.ended && snapshot.videoId === this.endState.videoId &&
        snapshot.duration === this.endState.duration &&
        snapshot.presentedFrames >= this.endState.frames &&
        snapshot.presentedMediaTime >= this.endState.mediaTime &&
        snapshot.presentedMediaTime <= snapshot.duration &&
        Math.abs(snapshot.currentTime - snapshot.duration) <= 0.1;
    if (this.naturalEnd && !verifiedEnd)
      this.fail('completed video identity or final timestamp changed');
    if (!this.started || this.progress < 5 ||
        (!verifiedEnd && (!PlaybackHealth.playing(snapshot) || !this.recent(now))))
      this.fail('YouTube did not sustain playback through the final sample with recent frame and time advancement');
    return verifiedEnd;
  }
}

function hardwareContinuity(videoPlayers, decoder, platform, maximumYoutubeFps) {
  const h264Players = videoPlayers.filter((candidate) =>
      /(?:avc1|h264)/i.test(candidate.properties.kVideoTracks || ''));
  return decoder === 'VaapiVideoDecoder' && platform === 'true' &&
      maximumYoutubeFps <= 30 && h264Players.length > 0 &&
      h264Players.every((candidate) => candidate.decoderHistory.length > 0 &&
          candidate.decoderHistory.every((name) => name === 'VaapiVideoDecoder') &&
          candidate.properties.kIsPlatformVideoDecoder === 'true');
}

function getJson(path) {
  return new Promise((resolve, reject) => {
    const request = http.get({host: '127.0.0.1', port, path}, (response) => {
      let body = '';
      response.setEncoding('utf8');
      response.on('data', (chunk) => { body += chunk; });
      response.on('end', () => {
        try {
          resolve(JSON.parse(body));
        } catch (error) {
          reject(error);
        }
      });
    });
    request.setTimeout(10000, () => request.destroy(new Error('DevTools HTTP request timed out')));
    request.on('error', reject);
  });
}

async function findPageTarget() {
  for (let attempt = 0; attempt < 100; ++attempt) {
    try {
      const targets = await getJson('/json/list');
      const page = targets.find((target) => target.type === 'page');
      if (page)
        return page;
    } catch (_) {
      // Chrome may still be creating its DevTools listener.
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`no Chrome page target on DevTools port ${port}`);
}

async function main() {
  const target = await findPageTarget();
  const socket = new WebSocket(target.webSocketDebuggerUrl,
                               {origin: 'http://localhost'});
  activeSocket = socket;
  await new Promise((resolve, reject) => {
    socket.once('open', resolve);
    socket.once('error', reject);
  });

  let nextId = 1;
  const replies = new Map();
  const players = new Map();
  let snapshot = null;
  let previousPlayableTime = null;
  let firstPlayingTime = null;
  let playbackAdvancement = 0;
  let previousSampleAt = null;
  let maximumTime = 0;
  const regressions = [];
  const presentedRegressions = new Map();
  const seekViolations = new Map();
  let maximumPresentedFrames = 0;
  let maximumYoutubeFps = 0;
  let maximumYoutubeWidth = 0;
  let nextSeek = 0;
  const settledSeeks = new Set();
  const health = new PlaybackHealth();

  function call(method, params = {}) {
    const id = nextId++;
    socket.send(JSON.stringify({id, method, params}));
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        replies.delete(id);
        reject(new Error(`DevTools ${method} timed out`));
      }, 15000);
      replies.set(id, {resolve, reject, timer});
    });
  }

  socket.on('message', (data) => {
    const message = JSON.parse(data.toString());
    if (message.id) {
      const reply = replies.get(message.id);
      if (!reply)
        return;
      replies.delete(message.id);
      clearTimeout(reply.timer);
      if (message.error)
        reply.reject(new Error(`DevTools command failed (code ${message.error.code})`));
      else
        reply.resolve(message.result);
      return;
    }

    const params = message.params || {};
    const playerId = params.playerId ||
                     (params.player && params.player.playerId);
    if (playerId && !players.has(playerId))
      players.set(playerId, {
        properties: {}, messages: [], errors: [], decoderHistory: [],
      });
    const player = players.get(playerId);
    if (message.method === 'Media.playerPropertiesChanged' && player) {
      for (const property of params.properties || []) {
        player.properties[property.name] = property.value;
        if (property.name === 'kVideoDecoderName' &&
            player.decoderHistory.at(-1) !== property.value) {
          player.decoderHistory.push(property.value);
        }
      }
    } else if (message.method === 'Media.playerMessagesLogged' && player) {
      player.messages.push(...(params.messages || []));
    } else if (message.method === 'Media.playerErrorsRaised' && player) {
      player.errors.push(...(params.errors || []));
    }
  });

  await call('Page.enable');
  await call('Runtime.enable');
  await call('Media.enable');
  if (forceH264) {
    await call('Page.addScriptToEvaluateOnNewDocument', {source: `
      (() => {
        const blocked = /(?:video\\/webm|vp0?9|av0?1)/i;
        if (self.MediaSource && MediaSource.isTypeSupported) {
          const original = MediaSource.isTypeSupported.bind(MediaSource);
          MediaSource.isTypeSupported = type =>
              blocked.test(type) ? false : original(type);
        }
      })();
    `});
  }
  await call('Page.navigate', {url});
  console.log(`Observation window: ${seconds}s; rVFC checks ordered timestamps, not pixel identity`);

  for (let elapsed = 0; elapsed < seconds; ++elapsed) {
    await new Promise((resolve) => setTimeout(resolve, 1000));
    if (nextSeek < seekSchedule.length &&
        elapsed + 1 >= seekSchedule[nextSeek].at) {
      const scheduledSeek = seekSchedule[nextSeek];
      const seekResult = await call('Runtime.evaluate', {
        returnByValue: true,
        expression: `(() => {
          const video = document.querySelector('video');
          const audit = video ? video.__crystalHdAudit : null;
          if (!video || !audit || video.readyState < 2 || video.currentTime <= 0)
            return {performed: false};
          const from = video.currentTime;
          if (audit.seek && !audit.seek.settled)
            return {performed: false};
          const target = ${JSON.stringify(scheduledSeek.target)};
          audit.seek = {
            id: ${nextSeek},
            from,
            target,
            cutoff: (from + target) / 2,
            forward: target >= from,
            settled: false,
            deadline: performance.now() + 5000,
            violations: [],
          };
          audit.lastMediaTime = null;
          audit.regressions = [];
          video.currentTime = target;
          video.play().catch(() => {});
          return {performed: true, from, target};
        })()`,
      });
      const seek = seekResult.result.value;
      if (seek && seek.performed) {
        ++nextSeek;
        previousPlayableTime = null;
        health.beginSeek(performance.now());
        console.log(`Seek: ${seek.from.toFixed(2)}s -> ${seek.target.toFixed(2)}s`);
      }
    }
    const result = await call('Runtime.evaluate', {
      returnByValue: true,
      awaitPromise: true,
      expression: `(() => {
        const video = document.querySelector('video');
        const player = document.getElementById('movie_player');
        if (video && !video.ended) {
          video.muted = true;
          video.play().catch(() => {});
        }
        const quality = video && video.getVideoPlaybackQuality ?
            video.getVideoPlaybackQuality() : null;
        let stats = null;
        try {
          stats = player && player.getStatsForNerds ? player.getStatsForNerds() : null;
        } catch (_) { /* This watch-page diagnostic is not a public API. */ }
        const {playerErrorCode, playerDebugStatus} =
            (${readPlayerDiagnostics.toString()})(player);
        const visible = element => {
          if (!element || !element.textContent.trim()) return false;
          if (typeof element.checkVisibility === 'function')
            return element.checkVisibility({checkOpacity: true, checkVisibilityCSS: true});
          const style = getComputedStyle(element);
          return element.getClientRects().length > 0 && style.display !== 'none' &&
              style.visibility !== 'hidden' && style.opacity !== '0';
        };
        const appError = [...document.querySelectorAll(
            '#movie_player .ytp-error-content-wrap, #movie_player .ytp-error, ' +
            'ytd-player-error-message-renderer')].some(visible);
        if (video && video.requestVideoFrameCallback && !video.__crystalHdAudit) {
          const audit = {lastMediaTime: null, frames: 0, regressions: [],
                         seek: null, videoId: (globalThis.__crystalHdProbeVideoId || 0) + 1};
          globalThis.__crystalHdProbeVideoId = audit.videoId;
          video.__crystalHdAudit = audit;
          const observe = (_, metadata) => {
            const seeking = audit.seek && !audit.seek.settled;
            if (!seeking && audit.lastMediaTime !== null &&
                metadata.mediaTime < audit.lastMediaTime - 0.001) {
              audit.regressions.push({from: audit.lastMediaTime,
                                      to: metadata.mediaTime});
            }
            audit.lastMediaTime = metadata.mediaTime;
            ++audit.frames;
            if (audit.seek) {
              const onNewSide = audit.seek.forward
                  ? metadata.mediaTime >= audit.seek.cutoff
                  : metadata.mediaTime <= audit.seek.cutoff;
              if (!audit.seek.settled && onNewSide)
                audit.seek.settled = true;
              else if (audit.seek.settled && !onNewSide &&
                       performance.now() <= audit.seek.deadline) {
                audit.seek.violations.push({mediaTime: metadata.mediaTime,
                                            cutoff: audit.seek.cutoff});
              }
            }
            video.requestVideoFrameCallback(observe);
          };
          video.requestVideoFrameCallback(observe);
        }
        const frameAudit = video ? video.__crystalHdAudit : null;
        return {
          videoPresent: Boolean(video),
          videoId: frameAudit ? frameAudit.videoId : null,
          appError,
          playerErrorCode,
          playerDebugStatus,
          mediaError: video && video.error ? video.error.code : null,
          currentTime: video ? video.currentTime : 0,
          duration: video && Number.isFinite(video.duration) ? video.duration : null,
          seeking: video ? video.seeking : false,
          paused: video ? video.paused : true,
          ended: video ? video.ended : false,
          readyState: video ? video.readyState : 0,
          width: video ? video.videoWidth : 0,
          height: video ? video.videoHeight : 0,
          totalFrames: quality ? quality.totalVideoFrames : 0,
          droppedFrames: quality ? quality.droppedVideoFrames : 0,
          youtubeCodecs: stats ? stats.codecs : '',
          youtubeResolution: stats ? stats.resolution : '',
          presentedFrames: frameAudit ? frameAudit.frames : 0,
          presentedMediaTime: frameAudit ? frameAudit.lastMediaTime : null,
          presentedRegressions: frameAudit ? frameAudit.regressions : [],
          seekSettled: frameAudit && frameAudit.seek
              ? frameAudit.seek.settled : false,
          seekId: frameAudit && frameAudit.seek
              ? frameAudit.seek.id : null,
          seekViolations: frameAudit && frameAudit.seek
              ? frameAudit.seek.violations : [],
        };
      })()`,
    });
    snapshot = result.result.value;
    health.observe(snapshot, performance.now());
    if (!snapshot)
      continue;
    if (!snapshot.paused && snapshot.currentTime > 0 && firstPlayingTime === null)
      firstPlayingTime = snapshot.currentTime;
    maximumTime = Math.max(maximumTime, snapshot.currentTime);
    maximumPresentedFrames = Math.max(maximumPresentedFrames,
                                      snapshot.presentedFrames);
    const youtubeFps = /@(\d+(?:\.\d+)?)/
        .exec(snapshot.youtubeResolution || '');
    const youtubeSize = /(\d+)x\d+/
        .exec(snapshot.youtubeResolution || '');
    if (youtubeFps)
      maximumYoutubeFps = Math.max(maximumYoutubeFps, Number(youtubeFps[1]));
    if (youtubeSize)
      maximumYoutubeWidth = Math.max(maximumYoutubeWidth,
                                     Number(youtubeSize[1]));
    for (const regression of snapshot.presentedRegressions) {
      presentedRegressions.set(`${regression.from}:${regression.to}`,
                               regression);
    }
    if (snapshot.seekSettled)
      settledSeeks.add(snapshot.seekId);
    for (const violation of snapshot.seekViolations) {
      seekViolations.set(`${violation.mediaTime}:${violation.cutoff}`,
                         violation);
    }
    // Ignore sub-frame jitter but fail any real backward seek. YouTube may
    // recreate the decoder internally; that must not move the media timeline.
    // During an adaptive quality switch YouTube briefly replaces the media
    // source. That element reports currentTime=0 with HAVE_METADATA or less;
    // it is not a playback seek and is not visible as rewound video.
    if (snapshot.readyState >= 2 && previousPlayableTime !== null &&
        snapshot.currentTime < previousPlayableTime - 0.5) {
      regressions.push({elapsed: elapsed + 1, from: previousPlayableTime,
                        to: snapshot.currentTime});
    }
    const sampledAt = Date.now();
    if (snapshot.readyState >= 2 && previousPlayableTime !== null &&
        previousSampleAt !== null) {
      const delta = snapshot.currentTime - previousPlayableTime;
      // A timeline jump is not evidence that frames continued playing.
      if (delta > 0 && delta <= (sampledAt - previousSampleAt) / 1000 + 0.5)
        playbackAdvancement += delta;
    }
    previousSampleAt = sampledAt;
    if (snapshot.readyState >= 2)
      previousPlayableTime = snapshot.currentTime;
    if ((elapsed + 1) % 5 === 0) {
      console.log(`Timeline ${elapsed + 1}s: media=${snapshot.currentTime.toFixed(2)}s, ` +
                  `ready=${snapshot.readyState}, ` +
                  `frames=${snapshot.totalFrames}, dropped=${snapshot.droppedFrames}, ` +
                  `presented=${snapshot.presentedFrames}`);
    }
  }

  const videoPlayers = [...players.values()].filter(
      (player) => player.properties.kVideoTracks);
  const player = videoPlayers.toReversed().find((candidate) =>
      candidate.properties.kFrameUrl === url) || videoPlayers[0];
  const properties = player ? player.properties : {};
  const decoder = properties.kVideoDecoderName || 'unknown';
  const platform = properties.kIsPlatformVideoDecoder || 'unknown';
  const tracks = properties.kVideoTracks || 'unknown';

  console.log(`YouTube codec: ${safeDiagnostic(snapshot ? snapshot.youtubeCodecs : 'unknown')}`);
  console.log(`YouTube resolution: ${safeDiagnostic(snapshot ? snapshot.youtubeResolution : 'unknown')} ` +
              `(maximum ${maximumYoutubeWidth || 'unknown'}px, ` +
              `${maximumYoutubeFps || 'unknown'} fps)`);
  console.log(`Chrome decoder: ${safeDiagnostic(decoder)} (platform=${safeDiagnostic(platform)})`);
  console.log(`Chrome tracks: ${safeDiagnostic(tracks)}`);
  if (snapshot) {
    console.log(`Playback: ${snapshot.currentTime.toFixed(2)}s, ` +
                `${snapshot.width}x${snapshot.height}, ` +
                `${snapshot.droppedFrames}/${snapshot.totalFrames} frames dropped`);
  }
  if (player) {
    console.log(`Decoder history: ${safeDiagnostic(player.decoderHistory.join(' -> ') || 'unknown')}`);
    for (const error of player.errors)
      console.error(`Media error: type=${safeDiagnostic(error.errorType)} code=${Number(error.code)}`);
  }
  for (const regression of regressions) {
    console.error(`Timeline regression at ${regression.elapsed}s: ` +
                  `${regression.from.toFixed(2)}s -> ${regression.to.toFixed(2)}s`);
  }
  for (const regression of presentedRegressions.values()) {
    console.error(`Presented-frame regression: ` +
                  `${regression.from.toFixed(3)}s -> ${regression.to.toFixed(3)}s`);
  }
  for (const violation of seekViolations.values()) {
    console.error(`Post-seek timestamp violation: media=${violation.mediaTime.toFixed(3)}s ` +
                  `cutoff=${violation.cutoff.toFixed(3)}s`);
  }

  socket.close();
  const naturalEnd = health.finish(performance.now());
  if (naturalEnd)
    console.log('Observed natural EOS: finite duration and near-tail rVFC timestamp verified (not pixel identity)');
  const h264 = /(?:avc1|h264)/i.test(
      `${snapshot ? snapshot.youtubeCodecs : ''} ${tracks}`);
  const mediaErrors = videoPlayers.flatMap((candidate) => candidate.errors);
  if (!h264 || !snapshot || firstPlayingTime === null || maximumTime < 5 ||
      playbackAdvancement < 5 || regressions.length || presentedRegressions.size ||
      maximumPresentedFrames === 0 || mediaErrors.length ||
      (seekSchedule.length !== 0 &&
       (nextSeek !== seekSchedule.length || settledSeeks.size !== seekSchedule.length ||
        seekViolations.size))) {
    throw new Error('YouTube did not sustain H.264 playback with complete, ordered seeks');
  }
  if (expectHardware) {
    if (!hardwareContinuity(videoPlayers, decoder, platform, maximumYoutubeFps)) {
      throw new Error('YouTube did not sustain H.264 playback through VA-API');
    }
  }
}

module.exports = {PlaybackHealth, hardwareContinuity, safeDiagnostic,
                  readPlayerDiagnostics, DEFAULT_SECONDS};

if (require.main === module) main().catch((error) => {
  if (activeSocket)
    activeSocket.terminate();
  console.error(`chromium-youtube: ${safeDiagnostic(error.message)}`);
  process.exitCode = 1;
});
