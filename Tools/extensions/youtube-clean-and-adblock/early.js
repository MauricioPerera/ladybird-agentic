(function () {
  if (window.__ladybirdYtAdPrunerInstalled) return;
  window.__ladybirdYtAdPrunerInstalled = true;

  var stats = (window.__ladybirdYtStats = window.__ladybirdYtStats || {
    prunedFetch: 0,
    prunedXhr: 0,
    prunedInitial: 0,
    prunedFields: 0,
    enforcementsBlocked: 0,
    enforcementsDismissed: 0,
    adsSkipped: 0,
    buttonsClicked: 0
  });

  var AD_KEYS = ["adPlacements", "playerAds", "adSlots", "adBreakHeartbeatParams", "adBreakParams"];
  var MAX_PRUNE_DEPTH = 8;

  function isEnforcementAction(item) {
    if (!item || typeof item !== "object") return false;
    var openPopup = item.openPopupAction;
    if (!openPopup || typeof openPopup !== "object") return false;
    var popup = openPopup.popup;
    if (!popup || typeof popup !== "object") return false;
    if (
      Object.prototype.hasOwnProperty.call(popup, "enforcementMessageViewModel") ||
      Object.prototype.hasOwnProperty.call(popup, "bkaEnforcementMessageViewModel")
    ) {
      return true;
    }
    if (openPopup.popupType === "DIALOG") {
      try {
        var serialized = JSON.stringify(popup);
        if (
          serialized.indexOf("enforcementMessageViewModel") !== -1 ||
          serialized.indexOf("bkaEnforcementMessageViewModel") !== -1
        ) {
          return true;
        }
      } catch (e) {}
    }
    return false;
  }

  function pruneActionArray(arr) {
    if (!Array.isArray(arr)) return 0;
    var removed = 0;
    for (var i = arr.length - 1; i >= 0; i--) {
      if (isEnforcementAction(arr[i])) {
        arr.splice(i, 1);
        removed++;
        stats.enforcementsBlocked++;
      }
    }
    return removed;
  }

  function pruneAdFields(obj, depth) {
    if (!obj || typeof obj !== "object") return 0;
    var currentDepth = typeof depth === "number" ? depth : 0;
    if (currentDepth > MAX_PRUNE_DEPTH) return 0;

    var removed = 0;

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
      typeof obj.auxiliaryUi.messageRenderers === "object"
    ) {
      var renderers = obj.auxiliaryUi.messageRenderers;
      if (Object.prototype.hasOwnProperty.call(renderers, "bkaEnforcementMessageViewModel")) {
        delete renderers.bkaEnforcementMessageViewModel;
        removed++;
        stats.enforcementsBlocked++;
      }
      if (Object.prototype.hasOwnProperty.call(renderers, "enforcementMessageViewModel")) {
        delete renderers.enforcementMessageViewModel;
        removed++;
        stats.enforcementsBlocked++;
      }
      if (Object.prototype.hasOwnProperty.call(renderers, "mealbarPromoRenderer")) {
        try {
          var promoJson = JSON.stringify(renderers.mealbarPromoRenderer);
          if (promoJson.indexOf("enforcement") !== -1 || promoJson.indexOf("ad blocker") !== -1 || promoJson.indexOf("bloqueador") !== -1) {
            delete renderers.mealbarPromoRenderer;
            removed++;
            stats.enforcementsBlocked++;
          }
        } catch (e) {}
      }
    }

    if (Array.isArray(obj.onResponseReceivedActions)) {
      removed += pruneActionArray(obj.onResponseReceivedActions);
    }
    if (Array.isArray(obj.actions)) {
      removed += pruneActionArray(obj.actions);
    }

    if (obj.playerResponse && typeof obj.playerResponse === "object") {
      removed += pruneAdFields(obj.playerResponse, currentDepth + 1);
    }
    if (obj.response && typeof obj.response === "object") {
      removed += pruneAdFields(obj.response, currentDepth + 1);
    }

    if (currentDepth === 0 && removed > 0) {
      stats.prunedFields += removed;
    }
    return removed;
  }

  window.__ladybirdPruneYtAdFields = pruneAdFields;

  function isAdBreakUrl(rawUrl) {
    if (!rawUrl || typeof rawUrl !== "string") return false;
    return rawUrl.indexOf("/youtubei/v1/player/ad_break") !== -1;
  }

  function isPlayerApiUrl(rawUrl) {
    if (!rawUrl || typeof rawUrl !== "string") return false;
    return (
      rawUrl.indexOf("/youtubei/v1/player") !== -1 ||
      rawUrl.indexOf("/youtubei/v1/next") !== -1 ||
      rawUrl.indexOf("/get_midroll_info") !== -1
    );
  }

  function hasTargetKeys(obj) {
    if (!obj || typeof obj !== "object") return false;
    if (
      Object.prototype.hasOwnProperty.call(obj, "adPlacements") ||
      Object.prototype.hasOwnProperty.call(obj, "playerAds") ||
      Object.prototype.hasOwnProperty.call(obj, "adSlots") ||
      Object.prototype.hasOwnProperty.call(obj, "adBreakParams") ||
      Object.prototype.hasOwnProperty.call(obj, "adBreakHeartbeatParams")
    ) {
      return true;
    }
    if (
      obj.auxiliaryUi &&
      typeof obj.auxiliaryUi === "object" &&
      obj.auxiliaryUi.messageRenderers &&
      typeof obj.auxiliaryUi.messageRenderers === "object"
    ) {
      var r = obj.auxiliaryUi.messageRenderers;
      if (
        Object.prototype.hasOwnProperty.call(r, "enforcementMessageViewModel") ||
        Object.prototype.hasOwnProperty.call(r, "bkaEnforcementMessageViewModel")
      ) {
        return true;
      }
    }
    if (Array.isArray(obj.onResponseReceivedActions) || Array.isArray(obj.actions)) {
      return true;
    }
    if (obj.playerResponse && typeof obj.playerResponse === "object") {
      return hasTargetKeys(obj.playerResponse);
    }
    if (obj.response && typeof obj.response === "object") {
      return hasTargetKeys(obj.response);
    }
    return false;
  }

  function hookInitialGlobal(propName) {
    try {
      var desc = Object.getOwnPropertyDescriptor(window, propName);
      if (!desc || desc.configurable) {
        var currentVal = window[propName];
        if (currentVal && typeof currentVal === "object") {
          if (pruneAdFields(currentVal, 0) > 0) stats.prunedInitial++;
        }
        Object.defineProperty(window, propName, {
          configurable: true,
          enumerable: true,
          get: function () {
            return currentVal;
          },
          set: function (val) {
            if (val && typeof val === "object") {
              if (pruneAdFields(val, 0) > 0) stats.prunedInitial++;
            }
            currentVal = val;
          }
        });
      }
    } catch (e) {}
  }

  hookInitialGlobal("ytInitialPlayerResponse");
  hookInitialGlobal("ytInitialData");

  try {
    if (typeof Response !== "undefined" && Response.prototype && typeof Response.prototype.json === "function") {
      var origResponseJson = Response.prototype.json;
      Response.prototype.json = function () {
        return origResponseJson.call(this).then(function (parsed) {
          try {
            if (hasTargetKeys(parsed)) {
              if (pruneAdFields(parsed, 0) > 0) stats.prunedFetch++;
            }
          } catch (e) {}
          return parsed;
        });
      };
    }
  } catch (e) {}

  try {
    if (typeof window.fetch === "function") {
      var origFetch = window.fetch;
      window.fetch = function (input, init) {
        var url = typeof input === "string" ? input : (input && input.url) || "";
        return origFetch.apply(this, arguments).then(function (resp) {
          if (isAdBreakUrl(url) && resp && resp.ok) {
            stats.prunedFetch++;
            return new Response("{}", {
              status: resp.status,
              statusText: resp.statusText,
              headers: resp.headers
            });
          }
          if (!isPlayerApiUrl(url) || !resp || !resp.ok || typeof resp.clone !== "function") {
            return resp;
          }
          return resp
            .clone()
            .text()
            .then(function (text) {
              try {
                var parsed = JSON.parse(text);
                if (pruneAdFields(parsed, 0) > 0) {
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
              if (this.readyState === 4 && typeof raw === "string") {
                if (isAdBreakUrl(this.__lbUrl)) {
                  stats.prunedXhr++;
                  return "{}";
                }
                if (isPlayerApiUrl(this.__lbUrl)) {
                  var parsed = JSON.parse(raw);
                  if (pruneAdFields(parsed, 0) > 0) {
                    stats.prunedXhr++;
                    return JSON.stringify(parsed);
                  }
                }
              }
            } catch (e) {}
            return raw;
          }
        });
      }

      var respDesc = Object.getOwnPropertyDescriptor(XMLHttpRequest.prototype, "response");
      if (respDesc && respDesc.get && respDesc.configurable) {
        Object.defineProperty(XMLHttpRequest.prototype, "response", {
          configurable: true,
          enumerable: true,
          get: function () {
            var raw = respDesc.get.call(this);
            try {
              if (this.readyState === 4 && this.responseType === "json" && raw && typeof raw === "object") {
                if (isAdBreakUrl(this.__lbUrl)) {
                  stats.prunedXhr++;
                  return {};
                }
                if (isPlayerApiUrl(this.__lbUrl) || hasTargetKeys(raw)) {
                  if (pruneAdFields(raw, 0) > 0) {
                    stats.prunedXhr++;
                  }
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
