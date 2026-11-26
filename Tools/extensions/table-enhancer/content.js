(function () {
  function parseComparable(raw) {
    var s = (raw || "").trim();
    var numStr = s.replace(/[$,%\s,]/g, "");
    if (numStr && /^-?\d+(\.\d+)?$/.test(numStr)) {
      return { kind: "num", val: parseFloat(numStr) };
    }
    if (/^\d{4}-\d{2}-\d{2}/.test(s)) {
      var ts = Date.parse(s);
      if (!isNaN(ts)) return { kind: "num", val: ts };
    }
    return { kind: "str", val: s.toLowerCase() };
  }

  function enhanceTable(table, minRows) {
    if (!table || table.dataset.ladybirdEnhanced === "1") return;
    if (table.closest(".ladybird-tbl-toolbar")) return;

    var tbody = table.tBodies && table.tBodies[0] ? table.tBodies[0] : null;
    var rows = tbody ? Array.from(tbody.rows) : Array.from(table.querySelectorAll("tr")).slice(1);
    if (rows.length < minRows) return;

    var headerRow = (table.tHead && table.tHead.rows[0]) ? table.tHead.rows[0] : table.querySelector("tr");
    if (!headerRow || headerRow.cells.length < 2) return;

    table.dataset.ladybirdEnhanced = "1";

    var toolbar = document.createElement("div");
    toolbar.className = "ladybird-tbl-toolbar";

    var filterInput = document.createElement("input");
    filterInput.type = "search";
    filterInput.className = "ladybird-tbl-filter";
    filterInput.placeholder = "Filter rows...";

    var rightGroup = document.createElement("div");
    rightGroup.style.display = "flex";
    rightGroup.style.alignItems = "center";
    rightGroup.style.gap = "8px";

    var counter = document.createElement("span");
    counter.className = "ladybird-tbl-meta";
    counter.textContent = rows.length + " / " + rows.length + " rows";

    var csvBtn = document.createElement("button");
    csvBtn.type = "button";
    csvBtn.className = "ladybird-tbl-btn";
    csvBtn.textContent = "Export CSV";

    rightGroup.appendChild(counter);
    rightGroup.appendChild(csvBtn);
    toolbar.appendChild(filterInput);
    toolbar.appendChild(rightGroup);

    table.parentNode.insertBefore(toolbar, table);

    var getBodyRows = function () {
      return tbody ? Array.from(tbody.rows) : Array.from(table.querySelectorAll("tr")).slice(1);
    };

    filterInput.addEventListener("input", function () {
      var q = filterInput.value.trim().toLowerCase();
      var currentRows = getBodyRows();
      var visible = 0;
      for (var i = 0; i < currentRows.length; i++) {
        var r = currentRows[i];
        var hit = !q || (r.textContent || "").toLowerCase().indexOf(q) !== -1;
        r.style.display = hit ? "" : "none";
        if (hit) visible++;
      }
      counter.textContent = visible + " / " + currentRows.length + " rows";
    });

    var headers = Array.from(headerRow.cells);
    var activeCol = -1;
    var activeDir = 1;

    headers.forEach(function (th, colIdx) {
      th.classList.add("ladybird-tbl-sortable");
      var ind = document.createElement("span");
      ind.className = "ladybird-tbl-sort-ind";
      ind.textContent = "⇅";
      th.appendChild(ind);

      th.addEventListener("click", function () {
        if (activeCol === colIdx) activeDir = -activeDir;
        else {
          activeCol = colIdx;
          activeDir = 1;
        }
        headers.forEach(function (other, idx) {
          var s = other.querySelector(".ladybird-tbl-sort-ind");
          if (s) s.textContent = idx === activeCol ? (activeDir === 1 ? "▲" : "▼") : "⇅";
        });

        var currentRows = getBodyRows();
        var parentContainer = tbody || table;
        currentRows.sort(function (a, b) {
          var ca = parseComparable(a.cells[colIdx] ? a.cells[colIdx].textContent : "");
          var cb = parseComparable(b.cells[colIdx] ? b.cells[colIdx].textContent : "");
          if (ca.kind === "num" && cb.kind === "num") {
            return (ca.val - cb.val) * activeDir;
          }
          return String(ca.val).localeCompare(String(cb.val)) * activeDir;
        });
        for (var i = 0; i < currentRows.length; i++) {
          parentContainer.appendChild(currentRows[i]);
        }
      });
    });

    csvBtn.addEventListener("click", function () {
      var out = [];
      var hdr = headers.map(function (th) {
        var clone = th.cloneNode(true);
        var ind = clone.querySelector(".ladybird-tbl-sort-ind");
        if (ind) ind.remove();
        return '"' + (clone.textContent || "").trim().replace(/"/g, '""') + '"';
      });
      out.push(hdr.join(","));
      getBodyRows().forEach(function (r) {
        if (r.style.display === "none") return;
        var cols = Array.from(r.cells).map(function (td) {
          return '"' + (td.textContent || "").trim().replace(/"/g, '""') + '"';
        });
        out.push(cols.join(","));
      });
      var blob = new Blob([out.join("\n")], { type: "text/csv;charset=utf-8;" });
      var a = document.createElement("a");
      a.href = URL.createObjectURL(blob);
      a.download = "table-export.csv";
      document.body.appendChild(a);
      a.click();
      a.remove();
    });
  }

  ladybirdExt.storage.local.get({ minRows: 2 }).then(function (cfg) {
    var minRows = Math.max(1, Number(cfg.minRows || 2));
    var scan = function () {
      var tables = document.querySelectorAll("table");
      for (var i = 0; i < tables.length; i++) enhanceTable(tables[i], minRows);
    };
    scan();
    if (document.body) {
      new MutationObserver(scan).observe(document.body, { childList: true, subtree: true });
    }
  });
})();
