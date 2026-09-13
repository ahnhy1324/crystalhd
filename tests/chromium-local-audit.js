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

  function observe(sample) {
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
    if (nextSeek < targets.length && segment.frames >= 20 &&
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
}

module.exports = {createFrameAuditor, validateBrowserAudit};
