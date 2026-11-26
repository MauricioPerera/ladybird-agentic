(function () {
  function parseComparable(raw) {
    var s = (raw || "").trim();
    var lower = s.toLowerCase();
    if (!s || lower === "-" || lower === "\u2013" || lower === "\u2014" || lower === "n/a" || lower === "none") {
      return { kind: "empty", val: "" };
    }
    var numStr = s.replace(/[$€£%\s,]/g, "");
    if (numStr && /^-?\d+(\.\d+)?$/.test(numStr)) {
      return { kind: "num", val: parseFloat(numStr) };
    }
    if (/^\d{4}-\d{2}-\d{2}/.test(s)) {
      var ts = Date.parse(s);
      if (!isNaN(ts)) return { kind: "num", val: ts };
    }
    return { kind: "str", val: lower };
  }

  function compareCells(ca, cb, dir) {
    if (ca.kind === "empty" && cb.kind === "empty") return 0;
    if (ca.kind === "empty") return 1;
    if (cb.kind === "empty") return -1;
    if (ca.kind === "num" && cb.kind === "num") {
      return (ca.val - cb.val) * dir;
    }
    if (ca.kind === "num") return -1;
    if (cb.kind === "num") return 1;
    return String(ca.val).localeCompare(String(cb.val)) * dir;
  }

  function getHeaderText(th) {
    var clone = th.cloneNode(true);
    var ind = clone.querySelector(".ladybird-tbl-sort-ind");
    if (ind) ind.remove();
    return (clone.textContent || "").trim().replace(/\s+/g, " ");
  }

  function matchesNumericQuery(cellText, op, target) {
    var parsed = parseComparable(cellText);
    if (parsed.kind !== "num") return false;
    if (op === "<=") return parsed.val <= target;
    if (op === ">=") return parsed.val >= target;
    if (op === "<") return parsed.val < target;
    if (op === ">") return parsed.val > target;
    if (op === "=") return parsed.val === target;
    return false;
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
    rows.forEach(function (r, idx) {
      if (r.dataset.ladybirdOrigIdx === undefined) {
        r.dataset.ladybirdOrigIdx = String(idx);
      }
    });

    var toolbar = document.createElement("div");
    toolbar.className = "ladybird-tbl-toolbar";

    var filterGroup = document.createElement("div");
    filterGroup.className = "ladybird-tbl-group";

    var colSelect = document.createElement("select");
    colSelect.className = "ladybird-tbl-select";
    var allOpt = document.createElement("option");
    allOpt.value = "-1";
    allOpt.textContent = "All columns";
    colSelect.appendChild(allOpt);

    var headers = Array.from(headerRow.cells);
    headers.forEach(function (th, colIdx) {
      var opt = document.createElement("option");
      opt.value = String(colIdx);
      opt.textContent = getHeaderText(th) || ("Column " + (colIdx + 1));
      colSelect.appendChild(opt);
    });

    var filterInput = document.createElement("input");
    filterInput.type = "search";
    filterInput.className = "ladybird-tbl-filter";
    filterInput.placeholder = "Filter (text or <=0.5, >1)...";

    filterGroup.appendChild(colSelect);
    filterGroup.appendChild(filterInput);

    var rightGroup = document.createElement("div");
    rightGroup.className = "ladybird-tbl-group";

    var counter = document.createElement("span");
    counter.className = "ladybird-tbl-meta";
    counter.textContent = rows.length + " / " + rows.length + " rows";

    var resetBtn = document.createElement("button");
    resetBtn.type = "button";
    resetBtn.className = "ladybird-tbl-btn ladybird-tbl-btn-secondary";
    resetBtn.textContent = "Reset";

    var csvBtn = document.createElement("button");
    csvBtn.type = "button";
    csvBtn.className = "ladybird-tbl-btn";
    csvBtn.textContent = "Export CSV";

    rightGroup.appendChild(counter);
    rightGroup.appendChild(resetBtn);
    rightGroup.appendChild(csvBtn);
    toolbar.appendChild(filterGroup);
    toolbar.appendChild(rightGroup);

    var anchor = (table.parentElement && table.parentElement.tagName === "DIV" && table.parentElement.children.length === 1 && table.parentElement.parentNode)
      ? table.parentElement
      : table;
    anchor.parentNode.insertBefore(toolbar, anchor);

    var getBodyRows = function () {
      return tbody ? Array.from(tbody.rows) : Array.from(table.querySelectorAll("tr")).slice(1);
    };

    var applyFilter = function () {
      var rawQuery = filterInput.value.trim();
      var selectedCol = parseInt(colSelect.value, 10);
      var numMatch = rawQuery.match(/^(<=|>=|<|>|=)\s*\$?(-?\d+(?:\.\d+)?)$/);
      var q = rawQuery.toLowerCase();
      var currentRows = getBodyRows();
      var visible = 0;

      for (var i = 0; i < currentRows.length; i++) {
        var r = currentRows[i];
        var hit = true;
        if (rawQuery) {
          if (numMatch) {
            var op = numMatch[1];
            var target = parseFloat(numMatch[2]);
            if (selectedCol >= 0) {
              var cell = r.cells[selectedCol];
              hit = cell ? matchesNumericQuery(cell.textContent, op, target) : false;
            } else {
              hit = Array.from(r.cells).some(function (c) {
                return matchesNumericQuery(c.textContent, op, target);
              });
            }
          } else if (selectedCol >= 0) {
            var targetCell = r.cells[selectedCol];
            hit = targetCell ? (targetCell.textContent || "").toLowerCase().indexOf(q) !== -1 : false;
          } else {
            hit = (r.textContent || "").toLowerCase().indexOf(q) !== -1;
          }
        }
        r.style.display = hit ? "" : "none";
        if (hit) visible++;
      }
      counter.textContent = visible + " / " + currentRows.length + " rows";
    };

    filterInput.addEventListener("input", applyFilter);
    colSelect.addEventListener("change", applyFilter);

    var activeCol = -1;
    var activeDir = 1;

    function updateSortIndicators() {
      headers.forEach(function (other, idx) {
        var s = other.querySelector(".ladybird-tbl-sort-ind");
        if (idx === activeCol) {
          other.classList.add("ladybird-tbl-sorted");
          if (s) s.textContent = activeDir === 1 ? "▲" : "▼";
        } else {
          other.classList.remove("ladybird-tbl-sorted");
          if (s) s.textContent = "⇅";
        }
      });
    }

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
        updateSortIndicators();

        var currentRows = getBodyRows();
        var parentContainer = tbody || table;
        currentRows.sort(function (a, b) {
          var ca = parseComparable(a.cells[colIdx] ? a.cells[colIdx].textContent : "");
          var cb = parseComparable(b.cells[colIdx] ? b.cells[colIdx].textContent : "");
          return compareCells(ca, cb, activeDir);
        });
        for (var i = 0; i < currentRows.length; i++) {
          parentContainer.appendChild(currentRows[i]);
        }
      });
    });

    resetBtn.addEventListener("click", function () {
      filterInput.value = "";
      colSelect.value = "-1";
      activeCol = -1;
      activeDir = 1;
      updateSortIndicators();

      var currentRows = getBodyRows();
      var parentContainer = tbody || table;
      currentRows.sort(function (a, b) {
        return Number(a.dataset.ladybirdOrigIdx || 0) - Number(b.dataset.ladybirdOrigIdx || 0);
      });
      for (var i = 0; i < currentRows.length; i++) {
        parentContainer.appendChild(currentRows[i]);
      }
      applyFilter();
    });

    csvBtn.addEventListener("click", function () {
      var out = [];
      var hdr = headers.map(function (th) {
        return '"' + getHeaderText(th).replace(/"/g, '""') + '"';
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
