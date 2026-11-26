(function () {
  var stats = (window.__ladybirdYtStats = window.__ladybirdYtStats || {
    prunedFetch: 0,
    prunedXhr: 0,
    prunedInitial: 0,
    prunedFields: 0,
    adsSkipped: 0,
    buttonsClicked: 0
  });

  if (typeof window.__ladybirdPruneYtAdFields === "function" && window.ytInitialPlayerResponse) {
    if (window.__ladybirdPruneYtAdFields(window.ytInitialPlayerResponse) > 0) {
      stats.prunedInitial++;
    }
  }

  var selectorsToRemove = [
    "ytd-reel-shelf-renderer",
    "ytd-rich-shelf-renderer[is-shorts]",
    "ytd-ad-slot-renderer",
    "ytd-in-feed-ad-layout-renderer",
    "ytd-companion-slot-renderer",
    "ytd-action-companion-ad-renderer",
    "ytd-promoted-sparkles-web-renderer",
    "ytd-display-ad-renderer"
  ];

  function cleanYouTubeDOM(root) {
    if (!root || !root.querySelectorAll) return;
    for (var i = 0; i < selectorsToRemove.length; i++) {
      var nodes = root.querySelectorAll(selectorsToRemove[i]);
      for (var j = 0; j < nodes.length; j++) {
        nodes[j].remove();
      }
    }

    var shortsLinks = root.querySelectorAll('a[href^="/shorts/"], a[title="Shorts"]');
    for (var k = 0; k < shortsLinks.length; k++) {
      var container = shortsLinks[k].closest("ytd-rich-item-renderer, ytd-video-renderer, ytd-grid-video-renderer, ytd-guide-entry-renderer, ytd-mini-guide-entry-renderer");
      if (container) container.remove();
    }
  }

  var skipButtonSelectors = [
    ".ytp-skip-ad-button",
    ".ytp-ad-skip-button",
    ".ytp-ad-skip-button-modern",
    "button.ytp-ad-skip-button-slot",
    ".ytp-ad-overlay-close-button"
  ];

  var lastSkipClickAt = 0;

  function handlePlayerAds() {
    var now = Date.now();
    if (now - lastSkipClickAt >= 250) {
      for (var i = 0; i < skipButtonSelectors.length; i++) {
        var buttons = document.querySelectorAll(skipButtonSelectors[i]);
        for (var j = 0; j < buttons.length; j++) {
          var btn = buttons[j];
          if (btn && !btn.disabled && typeof btn.click === "function") {
            btn.click();
            stats.buttonsClicked++;
            lastSkipClickAt = now;
          }
        }
      }
    }

    var player = document.querySelector(".html5-video-player");
    if (!player) return;
    var video = player.querySelector("video.html5-main-video") || player.querySelector("video");
    if (!video) return;

    var isAd =
      player.classList.contains("ad-showing") ||
      player.classList.contains("ad-interrupting");

    if (isAd) {
      if (player.dataset.lbAdSaved !== "1") {
        player.dataset.lbAdSaved = "1";
        player.dataset.lbPrevMuted = video.muted ? "1" : "0";
        player.dataset.lbPrevRate = String(video.playbackRate || 1);
      }
      video.muted = true;
      if (isFinite(video.duration) && video.duration > 0) {
        if (video.currentTime < video.duration - 0.1) {
          video.currentTime = Math.max(0, video.duration - 0.05);
          stats.adsSkipped++;
        }
        video.playbackRate = 4;
      } else {
        video.playbackRate = 16;
      }
      if (video.paused && typeof video.play === "function") {
        try {
          var p = video.play();
          if (p && p.catch) p.catch(function () {});
        } catch (e) {}
      }
    } else if (player.dataset.lbAdSaved === "1") {
      var prevMuted = player.dataset.lbPrevMuted === "1";
      var prevRate = parseFloat(player.dataset.lbPrevRate || "1");
      if (!isFinite(prevRate) || prevRate <= 0 || prevRate > 4) {
        prevRate = 1;
      }
      video.muted = prevMuted;
      video.playbackRate = prevRate;
      delete player.dataset.lbAdSaved;
      delete player.dataset.lbPrevMuted;
      delete player.dataset.lbPrevRate;
    }
  }

  function handleShortsNavigation(redirectEnabled) {
    if (!redirectEnabled) return;
    var match = /^\/shorts\/([^/?#]+)/.exec(window.location.pathname || "");
    if (match && match[1]) {
      window.location.replace("/watch?v=" + encodeURIComponent(match[1]));
    }
  }

  ladybirdExt.storage.local.get({ redirectShortsToWatch: true }).then(function (cfg) {
    handleShortsNavigation(!!cfg.redirectShortsToWatch);
    cleanYouTubeDOM(document);
    handlePlayerAds();

    if (!window.__ladybirdYtWatcherInstalled) {
      window.__ladybirdYtWatcherInstalled = true;

      var observer = new MutationObserver(function () {
        cleanYouTubeDOM(document);
        handlePlayerAds();
      });
      if (document.documentElement) {
        observer.observe(document.documentElement, { childList: true, subtree: true });
      }

      window.addEventListener("yt-navigate-finish", function () {
        handleShortsNavigation(!!cfg.redirectShortsToWatch);
        cleanYouTubeDOM(document);
        handlePlayerAds();
      });
      window.addEventListener("popstate", function () {
        handleShortsNavigation(!!cfg.redirectShortsToWatch);
        cleanYouTubeDOM(document);
        handlePlayerAds();
      });

      setInterval(function () {
        if ((window.location.pathname || "").indexOf("/watch") === 0) {
          handlePlayerAds();
        }
      }, 300);
    }
  });
})();
