// SPDX-License-Identifier: LGPL-2.1-or-later

(() => {
  "use strict";

  const rejectsCrystalHd = (contentType) => {
    const type = String(contentType || "").toLowerCase();
    const frameRate = /(?:^|[;,])\s*framerate\s*=\s*["']?([0-9.]+)/
      .exec(type);
    return type.startsWith("video/webm") ||
      /(?:codecs\s*=\s*["'][^"']*)?(?:vp0?8|vp0?9|av01)/.test(type) ||
      (frameRate && Number(frameRate[1]) > 30);
  };

  if (globalThis.MediaSource?.isTypeSupported) {
    const original = MediaSource.isTypeSupported.bind(MediaSource);
    Object.defineProperty(MediaSource, "isTypeSupported", {
      configurable: true,
      value: (contentType) => rejectsCrystalHd(contentType) ? false : original(contentType)
    });
  }

  if (globalThis.HTMLMediaElement?.prototype?.canPlayType) {
    const original = HTMLMediaElement.prototype.canPlayType;
    Object.defineProperty(HTMLMediaElement.prototype, "canPlayType", {
      configurable: true,
      value(contentType) {
        return rejectsCrystalHd(contentType) ? "" : original.call(this, contentType);
      }
    });
  }

  const capabilities = navigator.mediaCapabilities;
  if (capabilities?.decodingInfo) {
    const original = capabilities.decodingInfo.bind(capabilities);
    Object.defineProperty(capabilities, "decodingInfo", {
      configurable: true,
      value: (configuration) => {
        const frameRate = Number(configuration?.video?.framerate || 0);
        if (rejectsCrystalHd(configuration?.video?.contentType) ||
            frameRate > 30) {
          return Promise.resolve({supported: false, smooth: false, powerEfficient: false});
        }
        return original(configuration);
      }
    });
  }

  // YouTube sometimes places its cued thumbnail over a decoded frame while an
  // MSE seek or representation switch is settling. That is a separate DOM
  // layer, not a decoder frame, and can resurrect a visibly old image after the
  // video gate has opened. Never use that poster as a playback fallback.
  const posterStyle = document.createElement("style");
  posterStyle.id = "crystalhd-no-retained-poster";
  posterStyle.textContent = `
    .ytp-cued-thumbnail-overlay,
    .ytp-cued-thumbnail-overlay-image {
      display: none !important;
    }
  `;
  document.documentElement.append(posterStyle);

  // The GM965/CrystalHD presentation path on the target ThinkPad cannot keep
  // Chromium's ARGB import queue fed reliably at 720p. The normal YouTube
  // player is 854 pixels wide here, so cap its adaptive range at native 480p
  // instead of letting it overload at 720p and react only after visible stalls.
  const capYouTubeQuality = () => {
    const player = document.getElementById("movie_player");
    if (typeof player?.setPlaybackQualityRange !== "function")
      return;
    player.setPlaybackQualityRange("tiny", "large");
    const quality = player.getPlaybackQuality?.();
    if (quality === "auto" || quality === "hd720" || quality === "hd1080" ||
        quality === "hd1440" || quality === "hd2160" || quality === "highres") {
      player.setPlaybackQuality?.("large");
    }
  };
  document.addEventListener("yt-navigate-finish", capYouTubeQuality, true);
  setInterval(capYouTubeQuality, 1000);

  // Chromium can keep painting a previously presented DMA-BUF while a seek is
  // waiting for CrystalHD's reordered output. Hide every retained pixel at the
  // synchronous `seeking` event boundary, then reveal the video only after a
  // buffered run of callbacks belongs to the requested side of the timeline
  // and the compositor has had time to retire its pre-seek surface.
  const frameStates = new WeakMap();
  const seekGate = {
    active: false,
    from: null,
    target: null,
    lastPresented: null,
    goodFrames: 0,
    completed: 0,
    maskVisible: false,
    releasePending: false
  };
  Object.defineProperty(globalThis, "__crystalHdSeekGate", {
    configurable: true,
    value: seekGate
  });

  let gatedVideo = null;
  let gateToken = 0;
  let mask = null;
  let maskAnimation = 0;
  let revealTimer = 0;

  const playerFor = (video) =>
    video?.closest(".html5-video-player") || video;

  const cancelReveal = () => {
    if (revealTimer) {
      clearTimeout(revealTimer);
      revealTimer = 0;
    }
    seekGate.releasePending = false;
  };

  const removeMask = () => {
    if (maskAnimation) {
      cancelAnimationFrame(maskAnimation);
      maskAnimation = 0;
    }
    if (mask) {
      mask.remove();
      mask = null;
    }
  };

  // YouTube's cued-thumbnail/poster is a sibling of <video>, so an overlay
  // inside the video stacking context can still end up underneath the stale
  // image. Keep the seek mask at the document's top stacking level and size it
  // to the complete player on every animation frame. In YouTube fullscreen the
  // player itself is promoted to the browser's top layer, so put the mask
  // inside that element instead.
  const positionMask = (token) => {
    if (token !== gateToken || !mask || !gatedVideo)
      return;

    const player = playerFor(gatedVideo);
    const fullscreen = document.fullscreenElement;
    const fullscreenHost = fullscreen && fullscreen !== gatedVideo &&
      fullscreen.contains(gatedVideo) ? fullscreen : null;
    const wantedParent = fullscreenHost || document.documentElement;
    if (mask.parentElement !== wantedParent)
      wantedParent.append(mask);

    if (fullscreenHost) {
      mask.style.position = "absolute";
      mask.style.inset = "0";
      mask.style.removeProperty("left");
      mask.style.removeProperty("top");
      mask.style.removeProperty("width");
      mask.style.removeProperty("height");
    } else {
      const rectangle = player.getBoundingClientRect();
      mask.style.position = "fixed";
      mask.style.removeProperty("inset");
      mask.style.left = `${rectangle.left}px`;
      mask.style.top = `${rectangle.top}px`;
      mask.style.width = `${rectangle.width}px`;
      mask.style.height = `${rectangle.height}px`;
    }
    maskAnimation = requestAnimationFrame(() => positionMask(token));
  };

  const reveal = (token) => {
    if (token !== gateToken || !gatedVideo)
      return;
    revealTimer = 0;
    const state = frameStates.get(gatedVideo);
    if (state) {
      if (state.opacityValue)
        gatedVideo.style.setProperty("opacity", state.opacityValue,
          state.opacityPriority);
      else
        gatedVideo.style.removeProperty("opacity");
    }
    removeMask();
    gatedVideo = null;
    seekGate.active = false;
    seekGate.maskVisible = false;
    seekGate.releasePending = false;
    seekGate.completed += 1;
  };

  const cover = (video, state) => {
    const continuingSameSeek = gatedVideo === video;
    cancelReveal();
    ++gateToken;
    if (gatedVideo && gatedVideo !== video) {
      const previous = frameStates.get(gatedVideo);
      if (previous?.opacityValue)
        gatedVideo.style.setProperty("opacity", previous.opacityValue,
          previous.opacityPriority);
      else
        gatedVideo.style.removeProperty("opacity");
    }
    removeMask();
    gatedVideo = video;
    if (!continuingSameSeek) {
      state.opacityValue = video.style.getPropertyValue("opacity");
      state.opacityPriority = video.style.getPropertyPriority("opacity");
    }
    video.style.setProperty("opacity", "0", "important");

    mask = document.createElement("div");
    mask.id = "crystalhd-seek-mask";
    mask.setAttribute("aria-hidden", "true");
    mask.style.cssText = [
      "display:block!important",
      "margin:0!important",
      "padding:0!important",
      "border:0!important",
      "background:#000!important",
      "opacity:1!important",
      "visibility:visible!important",
      "pointer-events:none!important",
      "transform:none!important",
      "z-index:2147483647!important"
    ].join(";");
    document.documentElement.append(mask);
    seekGate.active = true;
    seekGate.goodFrames = 0;
    seekGate.maskVisible = true;
    positionMask(gateToken);
  };

  const track = (video) => {
    if (!(video instanceof HTMLVideoElement))
      return null;
    if (frameStates.has(video))
      return frameStates.get(video);
    const state = {
      lastMediaTime: null,
      opacityValue: "",
      opacityPriority: ""
    };
    frameStates.set(video, state);
    if (typeof video.requestVideoFrameCallback !== "function")
      return state;

    const observe = (_, metadata) => {
      state.lastMediaTime = metadata.mediaTime;
      if (seekGate.active && gatedVideo === video) {
        seekGate.lastPresented = metadata.mediaTime;
        const forward = seekGate.target >= seekGate.from;
        const cutoff = (seekGate.target + seekGate.from) / 2;
        const onRequestedTimeline = forward
          ? metadata.mediaTime >= cutoff
          : metadata.mediaTime <= cutoff;
        const followsPlayback =
          Math.abs(metadata.mediaTime - video.currentTime) <= 1;
        if (!video.seeking && video.readyState >= HTMLMediaElement.HAVE_FUTURE_DATA &&
            onRequestedTimeline && followsPlayback) {
          seekGate.goodFrames += 1;
        } else {
          seekGate.goodFrames = 0;
          cancelReveal();
        }
        if (seekGate.goodFrames >= 4 && !revealTimer) {
          const completedToken = gateToken;
          seekGate.releasePending = true;
          revealTimer = setTimeout(() => {
            // Keep the release latched while the final two animation frames
            // pass; otherwise another video callback could schedule a second
            // timer in this short interval.
            revealTimer = -1;
            requestAnimationFrame(() => requestAnimationFrame(() =>
              reveal(completedToken)));
          }, 750);
        }
      }
      video.requestVideoFrameCallback(observe);
    };
    video.requestVideoFrameCallback(observe);
    return state;
  };

  document.addEventListener("seeking", (event) => {
    const video = event.target;
    const state = track(video);
    if (!state)
      return;
    const from = Number.isFinite(state.lastMediaTime)
      ? state.lastMediaTime
      : video.currentTime;
    seekGate.from = from;
    seekGate.target = video.currentTime;
    seekGate.lastPresented = state.lastMediaTime;
    cover(video, state);
  }, true);

  const discoverVideos = () => {
    for (const video of document.querySelectorAll("video"))
      track(video);
  };
  new MutationObserver(discoverVideos).observe(document, {
    childList: true,
    subtree: true
  });
  queueMicrotask(discoverVideos);
})();
