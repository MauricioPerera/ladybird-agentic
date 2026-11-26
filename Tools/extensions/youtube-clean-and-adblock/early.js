(function () {
  if (window.__ladybirdYtAdPrunerInstalled) return;
  window.__ladybirdYtAdPrunerInstalled = true;

  var stats = (window.__ladybirdYtStats = window.__ladybirdYtStats || {
    prunedFetch: 0,
    prunedXhr: 0,
    prunedInitial: 0,
    prunedFields: 0,
    adsSkipped: 0,
    buttonsClicked: 0
  });

  var AD_KEYS = ["adPlacements", "playerAds", "adSlots", "adBreakHeartbeatParams"];

  function pruneAdFields(obj) {
    var removed = 0;
    if (!obj || typeof obj !== "object") return 0;

    for (var i = 0; i < AD_KEYS.length; i++) {
      var k = AD_KEYS[i];
      if (Object.prototype.hasOwnProperty.call(obj, k)) {
        delete obj[k];
        removed++;
      }
    }

    if (
      obj.auxiliaryUi &&
      typeof obj.auxiliaryUi === "object" &&
      obj.auxiliaryUi.messageRenderers &&
      typeof obj.auxiliaryUi.messageRenderers === "object" &&
      Object.prototype.hasOwnProperty.call(obj.auxiliaryUi.messageRenderers, "bkaEnforcementMessageViewModel")
    ) {
      delete obj.auxiliaryUi.messageRenderers.bkaEnforcementMessageViewModel;
      removed++;
    }

    if (obj.playerResponse && typeof obj.playerResponse === "object") {
      removed += pruneAdFields(obj.playerResponse);
    }

    if (removed > 0) stats.prunedFields += removed;
    return removed;
  }

  window.__ladybirdPruneYtAdFields = pruneAdFields;

  function isPlayerApiUrl(rawUrl) {
    if (!rawUrl || typeof rawUrl !== "string") return false;
    return (
      rawUrl.indexOf("/youtubei/v1/player") !== -1 ||
      rawUrl.indexOf("/youtubei/v1/next") !== -1 ||
      rawUrl.indexOf("/get_midroll_info") !== -1
    );
  }

  try {
    var initialDesc = Object.getOwnPropertyDescriptor(window, "ytInitialPlayerResponse");
    if (!initialDesc || initialDesc.configurable) {
      var currentInitial = window.ytInitialPlayerResponse;
      if (currentInitial && typeof currentInitial === "object") {
        if (pruneAdFields(currentInitial) > 0) stats.prunedInitial++;
      }
      Object.defineProperty(window, "ytInitialPlayerResponse", {
        configurable: true,
        enumerable: true,
        get: function () {
          return currentInitial;
        },
        set: function (val) {
          if (val && typeof val === "object") {
            if (pruneAdFields(val) > 0) stats.prunedInitial++;
          }
          currentInitial = val;
        }
      });
    }
  } catch (e) {}

  try {
    if (typeof window.fetch === "function") {
      var origFetch = window.fetch;
      window.fetch = function (input, init) {
        var url = typeof input === "string" ? input : (input && input.url) || "";
        return origFetch.apply(this, arguments).then(function (resp) {
          if (!isPlayerApiUrl(url) || !resp || !resp.ok || typeof resp.clone !== "function") {
            return resp;
          }
          return resp
            .clone()
            .text()
            .then(function (text) {
              try {
                var parsed = JSON.parse(text);
                if (pruneAdFields(parsed) > 0) {
                  stats.prunedFetch++;
                  return new Response(JSON.stringify(parsed), {
                    status: resp.status,
                    statusText: resp.statusText,
                    headers: resp.headers
                  });
                }
              } catch (err) {}
              return resp;
            })
            .catch(function () {
              return resp;
            });
        });
      };
    }
  } catch (e) {}

  try {
    if (typeof XMLHttpRequest !== "undefined") {
      var origOpen = XMLHttpRequest.prototype.open;
      XMLHttpRequest.prototype.open = function (method, url) {
        try {
          this.__lbUrl = typeof url === "string" ? url : String(url || "");
        } catch (e) {}
        return origOpen.apply(this, arguments);
      };

      var rtDesc = Object.getOwnPropertyDescriptor(XMLHttpRequest.prototype, "responseText");
      if (rtDesc && rtDesc.get && rtDesc.configurable) {
        Object.defineProperty(XMLHttpRequest.prototype, "responseText", {
          configurable: true,
          enumerable: true,
          get: function () {
            var raw = rtDesc.get.call(this);
            try {
              if (this.readyState === 4 && typeof raw === "string" && isPlayerApiUrl(this.__lbUrl)) {
                var parsed = JSON.parse(raw);
                if (pruneAdFields(parsed) > 0) {
                  stats.prunedXhr++;
                  return JSON.stringify(parsed);
                }
              }
            } catch (e) {}
            return raw;
          }
        });
      }
    }
  } catch (e) {}
})();
