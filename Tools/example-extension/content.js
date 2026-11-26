(function () {
  if (!document.body) return;
  var existing = document.getElementById("ladybird-ext-inspector-badge");
  if (existing) existing.remove();
  var links = document.querySelectorAll("a").length;
  var headings = document.querySelectorAll("h1,h2,h3").length;
  ladybirdExt.storage.local.get({ labelPrefix: "Inspector" }).then(function (cfg) {
    var badge = document.createElement("button");
    badge.id = "ladybird-ext-inspector-badge";
    badge.type = "button";
    badge.textContent = cfg.labelPrefix + ": " + headings + "h / " + links + "a";
    badge.title = "Click to dismiss (Ladybird extension example)";
    badge.addEventListener("click", function () {
      badge.remove();
    });
    document.body.appendChild(badge);
  });
})();
