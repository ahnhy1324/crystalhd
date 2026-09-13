// SPDX-License-Identifier: LGPL-2.1-or-later

(() => {
  "use strict";

  // Filter codec capability queries only. Resolution, playback, posters, and
  // seek presentation remain under the site's and the user's control.
  const rejectsCrystalHd = (contentType) => {
    const type = String(contentType || "").toLowerCase();
    const frameRate = /(?:^|[;,])\s*framerate\s*=\s*["']?([0-9.]+)/
      .exec(type);
    return type.startsWith("video/webm") ||
      /(?:codecs\s*=\s*["'][^"']*)?(?:vp0?8|vp0?9|av01)/.test(type) ||
      (frameRate && Number(frameRate[1]) > 30);
  };

  const mediaSource = globalThis.MediaSource;
  if (typeof mediaSource?.isTypeSupported === "function") {
    const original = mediaSource.isTypeSupported;
    Object.defineProperty(mediaSource, "isTypeSupported", {
      configurable: true,
      value(...args) {
        return rejectsCrystalHd(args[0]) ? false : original.apply(this, args);
      }
    });
  }

  const mediaPrototype = globalThis.HTMLMediaElement?.prototype;
  if (typeof mediaPrototype?.canPlayType === "function") {
    const original = mediaPrototype.canPlayType;
    Object.defineProperty(mediaPrototype, "canPlayType", {
      configurable: true,
      value(...args) {
        return rejectsCrystalHd(args[0]) ? "" : original.apply(this, args);
      }
    });
  }

  const capabilities = globalThis.navigator?.mediaCapabilities;
  if (typeof capabilities?.decodingInfo === "function") {
    const original = capabilities.decodingInfo;
    Object.defineProperty(capabilities, "decodingInfo", {
      configurable: true,
      value(...args) {
        const configuration = args[0];
        const frameRate = Number(configuration?.video?.framerate || 0);
        if (rejectsCrystalHd(configuration?.video?.contentType) ||
            frameRate > 30) {
          return Promise.resolve({supported: false, smooth: false, powerEfficient: false});
        }
        return original.apply(this, args);
      }
    });
  }
})();
