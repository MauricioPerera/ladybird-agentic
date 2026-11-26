(function () {
  var selectorsToRemove = [
    "ytd-reel-shelf-renderer",
    "ytd-rich-shelf-renderer[is-shorts]",
    "ytd-ad-slot-renderer",
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

    var observer = new MutationObserver(function () {
      cleanYouTubeDOM(document);
    });
    if (document.documentElement) {
      observer.observe(document.documentElement, { childList: true, subtree: true });
    }

    window.addEventListener("yt-navigate-finish", function () {
      handleShortsNavigation(!!cfg.redirectShortsToWatch);
      cleanYouTubeDOM(document);
    });
    window.addEventListener("popstate", function () {
      handleShortsNavigation(!!cfg.redirectShortsToWatch);
      cleanYouTubeDOM(document);
    });
  });
})();
