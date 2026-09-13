// SPDX-License-Identifier: LGPL-2.1-or-later
'use strict';

// This function is serialized into the fixture page. Keep it self-contained
// so the browser and deterministic tests exercise exactly the same decisions.
function createFrameAuditor() {
  const fps = 30;
  const duration = 12;
  const lastIdentity = 359;
  const targets = [6, 2, 8, 0];
  const audit = {frames: 0, segments: [], mismatches: [], adjacentReadbacks: 0,
    duplicateReadbacks: 0, errors: [], done: false, seek: null,
    duration: null, finalFrame: null};
  let segment = {target: 0, frames: 0, first: null, last: null};
  audit.segments.push(segment);
  let nextSeek = 0;

  function checkPixels(sample) {
    const expected = Math.round(sample.frameTime * fps);
    const valid = Number.isFinite(sample.frameTime) &&
      Number.isInteger(sample.identity) && expected >= 0 && expected <= lastIdentity &&
      sample.identity === expected && Number.isFinite(sample.white) &&
      Number.isFinite(sample.black) && sample.white >= 200 && sample.black <= 50;
    if (!valid && audit.mismatches.length < 30)
      audit.mismatches.push({...sample, expected});
    return valid;
  }

  function observe(sample, allowSeek = true) {
    if (!checkPixels(sample)) return undefined;
    if (Number.isFinite(sample.callbackTime) &&
        Math.round(sample.callbackTime * fps) !== sample.identity)
      ++audit.adjacentReadbacks;
    if (sample.seeking) return undefined;
    const time = sample.frameTime;
    if (audit.seek !== null) {
      if (time < audit.seek - 0.5 / fps) {
        audit.errors.push(`Pre-target frame after seek: ${time} < ${audit.seek}`);
        return undefined;
      }
      if (time > audit.seek + 0.15) {
        audit.errors.push(`Seek target was not observed: ${time} > ${audit.seek}`);
        return undefined;
      }
      segment = {target: audit.seek, frames: 0, first: null, last: null};
      audit.segments.push(segment);
      audit.seek = null;
    }
    if (segment.last !== null && time < segment.last - 0.001) {
      audit.errors.push(`Unrequested regression ${segment.last} -> ${time}`);
      return undefined;
    }
    if (segment.last !== null && Math.abs(time - segment.last) < 0.001) {
      ++audit.duplicateReadbacks;
      return undefined;
    }
    if (segment.first === null) segment.first = time;
    segment.last = time;
    ++segment.frames;
    ++audit.frames;
    if (allowSeek && nextSeek < targets.length && segment.frames >= 20 &&
        segment.last - segment.first >= 0.7) {
      audit.seek = targets[nextSeek++];
      return audit.seek;
    }
    return undefined;
  }

  function end(sample, mediaDuration) {
    audit.duration = mediaDuration;
    audit.finalFrame = sample;
    if (!sample || !checkPixels(sample) || sample.identity !== lastIdentity ||
        Math.abs(sample.frameTime - lastIdentity / fps) > 0.5 / fps)
      audit.errors.push('Playback ended without actual final frame 359');
    if (!Number.isFinite(mediaDuration) || Math.abs(mediaDuration - duration) > 0.5 / fps)
      audit.errors.push('Fixture duration is not 12 seconds');
    audit.done = true;
  }
  return {audit, observe, end};
}

// Serialized into the page alongside createFrameAuditor. Wall time is supplied
// by performance.now(), not by media metadata; deterministic tests supply it
// explicitly. Paused video has no frame callbacks, so the page also samples it
// with a timer. Every observation still owns and checks its actual VideoFrame.
function createPlaybackControlAuditor() {
  const plan = [
    {name: 'initial-1x', rate: 1, milliseconds: 1000},
    {name: 'paused', rate: 1, milliseconds: 1000, paused: true},
    {name: 'resumed-1x', rate: 1, milliseconds: 1000},
    {name: 'half-speed', rate: 0.5, milliseconds: 1500},
    {name: 'one-and-half-speed', rate: 1.5, milliseconds: 1000},
    {name: 'double-speed', rate: 2, milliseconds: 1000},
    {name: 'restored-1x', rate: 1, milliseconds: 1000},
  ];
  const audit = {done: false, phases: [], errors: []};
  let index = 0, requestedAt = null, requestSample = null;
  let anchor = null, previous = null, lastWall = null;
  let samples = 0, advances = 0;
  function fail(message) {
    if (!audit.errors.length) audit.errors.push(message);
  }
  function observe(sample) {
    if (audit.done || audit.errors.length) return undefined;
    const phase = plan[index];
    if (!sample || !Number.isFinite(sample.wallTime) ||
        !Number.isFinite(sample.currentTime) || !Number.isFinite(sample.frameTime) ||
        !Number.isInteger(sample.identity) || sample.identity < 0 || sample.identity > 359 ||
        sample.identity !== Math.round(sample.frameTime * 30) ||
        !Number.isFinite(sample.white) || sample.white < 200 ||
        !Number.isFinite(sample.black) || sample.black > 50 ||
        typeof sample.paused !== 'boolean' || !Number.isFinite(sample.playbackRate)) {
      fail(`Invalid actual-frame sample during ${phase.name}`);
      return undefined;
    }
    if (lastWall !== null && sample.wallTime < lastWall) {
      fail('Control observation clock regressed');
      return undefined;
    }
    lastWall = sample.wallTime;
    if (requestedAt === null) requestedAt = sample.wallTime;
    if (sample.wallTime - requestedAt > 5000) {
      fail(`Playback control ${phase.name} did not complete within five seconds`);
      return undefined;
    }
    if (sample.seeking || sample.ended) {
      fail(`Unexpected seek or EOS during ${phase.name}`);
      return undefined;
    }
    const matches = sample.paused === Boolean(phase.paused) &&
      Math.abs(sample.playbackRate - phase.rate) < 0.001;
    if (!matches) {
      if (anchor || sample.wallTime - requestedAt > 1000)
        fail(`Requested pause/rate state was not maintained during ${phase.name}`);
      return undefined;
    }
    // Allow a compositor readback already queued when pause() was requested
    // to settle, but do not allow a continuing stream to hide in this grace.
    if (phase.paused && requestSample &&
        (Math.abs(sample.frameTime - requestSample.frameTime) > 0.15 ||
         Math.abs(sample.currentTime - requestSample.currentTime) > 0.15)) {
      fail('Playback continued after pause was requested');
      return undefined;
    }
    if (phase.paused && sample.wallTime - requestedAt < 200) return undefined;
    if (!anchor) {
      anchor = previous = {...sample};
      samples = 1;
      advances = 0;
      return undefined;
    }
    if (sample.wallTime === previous.wallTime) return undefined;
    if (sample.wallTime - previous.wallTime > 750) {
      fail(`Insufficient observation continuity during ${phase.name}`);
      return undefined;
    }
    if (sample.currentTime < previous.currentTime - 0.001 ||
        sample.frameTime < previous.frameTime - 0.001) {
      fail(`Unrequested media regression during ${phase.name}`);
      return undefined;
    }
    ++samples;
    if (sample.frameTime > previous.frameTime + 0.001 &&
        sample.currentTime > previous.currentTime + 0.001) ++advances;
    previous = {...sample};
    const wallSeconds = (sample.wallTime - anchor.wallTime) / 1000;
    const frameSeconds = sample.frameTime - anchor.frameTime;
    const timeSeconds = sample.currentTime - anchor.currentTime;
    if (phase.paused && (sample.identity !== anchor.identity ||
        Math.abs(frameSeconds) > 1 / 60 || Math.abs(timeSeconds) > 1 / 60)) {
      fail('Paused playback changed its actual picture or media time');
      return undefined;
    }
    if (wallSeconds * 1000 + 0.001 < phase.milliseconds) return undefined;
    const expected = wallSeconds * phase.rate;
    const tolerance = Math.max(0.1, expected * 0.2);
    if (!phase.paused && (advances < 4 ||
        Math.abs(frameSeconds - expected) > tolerance ||
        Math.abs(timeSeconds - expected) > tolerance)) {
      fail(`Actual picture/time progression did not match ${phase.rate}x during ${phase.name}`);
      return undefined;
    }
    audit.phases.push({name: phase.name, rate: phase.rate,
      paused: Boolean(phase.paused), wallSeconds, frameSeconds, timeSeconds,
      samples, advances, firstIdentity: anchor.identity, lastIdentity: sample.identity});
    ++index;
    if (index === plan.length) {
      audit.done = true;
      return undefined;
    }
    requestedAt = sample.wallTime;
    requestSample = {...sample};
    anchor = previous = null;
    const next = plan[index];
    return {type: next.paused ? 'pause' : 'play', rate: next.rate};
  }
  return {audit, observe};
}

function validatePlaybackControls(controls) {
  const plan = [
    ['initial-1x', 1, 1], ['paused', 1, 1], ['resumed-1x', 1, 1],
    ['half-speed', 0.5, 1.5], ['one-and-half-speed', 1.5, 1],
    ['double-speed', 2, 1], ['restored-1x', 1, 1],
  ];
  if (!controls || !controls.done || !Array.isArray(controls.errors) ||
      controls.errors.length || !Array.isArray(controls.phases) ||
      controls.phases.length !== plan.length)
    throw new Error('Pause/resume and playback-rate audit did not complete');
  for (let index = 0; index < plan.length; ++index) {
    const [name, rate, minimumSeconds] = plan[index];
    const phase = controls.phases[index];
    const paused = name === 'paused';
    if (!phase || phase.name !== name || phase.rate !== rate || phase.paused !== paused ||
        !Number.isFinite(phase.wallSeconds) || phase.wallSeconds + 0.000001 < minimumSeconds ||
        phase.wallSeconds > 5 || !Number.isFinite(phase.frameSeconds) ||
        !Number.isFinite(phase.timeSeconds) || !Number.isInteger(phase.samples) ||
        phase.samples < 4 || !Number.isInteger(phase.advances) || phase.advances < 0 ||
        phase.advances >= phase.samples || !Number.isInteger(phase.firstIdentity) ||
        !Number.isInteger(phase.lastIdentity) || phase.firstIdentity < 0 ||
        phase.lastIdentity > 359 || phase.lastIdentity < phase.firstIdentity)
      throw new Error(`Invalid playback-control evidence for ${name}`);
    if (paused) {
      if (phase.advances !== 0 || phase.firstIdentity !== phase.lastIdentity ||
          Math.abs(phase.frameSeconds) > 1 / 60 || Math.abs(phase.timeSeconds) > 1 / 60)
        throw new Error('Paused playback did not retain its actual picture and media time');
    } else {
      const expected = phase.wallSeconds * rate;
      const tolerance = Math.max(0.1, expected * 0.2);
      if (phase.advances < 4 || Math.abs(phase.frameSeconds - expected) > tolerance ||
          Math.abs(phase.timeSeconds - expected) > tolerance ||
          Math.abs((phase.lastIdentity - phase.firstIdentity) / 30 - phase.frameSeconds) > 1 / 30)
        throw new Error(`Actual picture/time progression did not match ${rate}x during ${name}`);
    }
  }
}

function validateBrowserAudit(result) {
  const {hardware, decoders, platformDecoders, mediaErrors, audit} = result;
  const targets = [0, 6, 2, 8, 0];
  if (!audit || !audit.done || audit.seek !== null ||
      !Array.isArray(audit.segments) || audit.segments.length !== targets.length ||
      audit.segments.some((segment, index) => segment.target !== targets[index] ||
        !Number.isInteger(segment.frames) || segment.frames < 20 ||
        !Number.isFinite(segment.first) || !Number.isFinite(segment.last) ||
        segment.last - segment.first < 0.7 ||
        segment.first < segment.target - 1 / 60 || segment.first > segment.target + 0.15) ||
      !Array.isArray(audit.mismatches) || audit.mismatches.length ||
      !Array.isArray(audit.errors) || audit.errors.length ||
      !Array.isArray(mediaErrors) || mediaErrors.length)
    throw new Error('Playback did not complete with correct frame identities and all four seeks');
  const final = audit.finalFrame;
  if (!Number.isFinite(audit.duration) || Math.abs(audit.duration - 12) > 1 / 60 ||
      !final || final.identity !== 359 || !Number.isFinite(final.frameTime) ||
      Math.abs(final.frameTime - 359 / 30) > 1 / 60 ||
      !Number.isFinite(final.white) || final.white < 200 ||
      !Number.isFinite(final.black) || final.black > 50)
    throw new Error('Playback did not reach actual final frame 359 of the 12-second fixture');
  const expectedDecoder = hardware ? 'VaapiVideoDecoder' : 'FFmpegVideoDecoder';
  if (!Array.isArray(decoders) || !decoders.length ||
      decoders.some(decoder => decoder !== expectedDecoder))
    throw new Error(`Playback did not exclusively use ${expectedDecoder}`);
  const expectedPlatform = hardware ? 'true' : 'false';
  if (!Array.isArray(platformDecoders) || !platformDecoders.length ||
      platformDecoders.some(value => String(value) !== expectedPlatform))
    throw new Error(`Platform decoder status was not consistently ${expectedPlatform}`);
  if (result.controls) validatePlaybackControls(audit.controls);
}

module.exports = {createFrameAuditor, createPlaybackControlAuditor,
  validatePlaybackControls, validateBrowserAudit};
