/* The configurator's edit flow: what a row or a form is in (Unsaved, Saving, Saved, Applied on the
 * radio, Failed, Changed on the radio), drafts that survive another row's save and a reload of the
 * list, Save all and Discard, a delete that says what goes and can be undone (U04).
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * A row is a <tr> with inputs; its action cell holds a .save button and a <span class="rst"> that
 * says its state. The page builds rows with data-key (the entry's identity on the radio: its id,
 * or the values that name it) and "" for a row not yet on the radio.
 *
 *   EditFlow.snapshot(row)            the values now are what the radio has
 *   EditFlow.watch(row, save)         an edit marks the row Unsaved (Enter in a text field saves)
 *   EditFlow.dirty(row)               edited since the snapshot?
 *   EditFlow.state(row, st, text)     "" | unsaved | saving | saved | applied | failed | changed
 *   EditFlow.rebuild(body, build)     build() fills the table again from the radio's list; the rows
 *                                     being edited keep their drafts (a row the radio changed
 *                                     meanwhile says so: Reload takes the radio's, Save keeps yours)
 *   EditFlow.tools(card, body)        "N unsaved · Save all · Discard" in the card's head
 *   EditFlow.confirmDelete(what, lost)
 *   EditFlow.undo($status, text, fn)  "Deleted X. Undo" for twenty seconds
 *   EditFlow.effect(el, text)         "Takes effect: ..." beside a card's title
 *   EditFlow.form(container, $status, $save)  a form: Unsaved in its status and Save enabled once a
 *                                     field changed; .reset() after a load or a save
 */
window.EditFlow = (function() {
  "use strict";
  const TEXT = {unsaved: "Unsaved", saving: "Saving...", saved: "Saved", applied: "Applied on the radio", failed: "Failed", changed: "Changed on the radio"};

  function fields(row) { return row.find("input, select, textarea").not("[type=button], [type=submit]"); }
  function values(row) {
    return fields(row).map(function() { return this.type === "checkbox" || this.type === "radio" ? !!this.checked : this.value; }).get();
  }
  function setValues(row, vals) {
    fields(row).each(function(i) { if (i >= vals.length) return; if (this.type === "checkbox" || this.type === "radio") this.checked = !!vals[i]; else this.value = vals[i]; });
  }
  function snapshot(row) { row.data("orig", JSON.stringify(values(row))); }
  function dirty(row) { return JSON.stringify(values(row)) !== row.data("orig"); }
  function rst(row) {
    let el = row.find(".rst");
    if (!el.length) el = $("<span class='rst'>").appendTo(row.children(".action-cell").last());
    return el;
  }
  function state(row, st, text) {
    const el = rst(row).attr("data-st", st).empty();
    if (st) el.append(document.createTextNode(text ? (st === "failed" ? "Failed: " + text : text) : TEXT[st]));
    row.toggleClass("unsaved", st === "unsaved" || st === "changed");
    row.find(".save").prop("disabled", !(st === "unsaved" || st === "changed"));
    const body = row.closest("tbody");
    if (body.length) tools(null, body);
    return el;
  }
  function check(row) {
    const st = row.find(".rst").attr("data-st");
    if (st === "saving") return;
    if (st === "changed" && dirty(row)) return;
    if (dirty(row)) state(row, "unsaved"); else if (st === "unsaved" || st === "changed") state(row, "");
  }
  function watch(row, save) {
    snapshot(row);
    fields(row).on("input change", function() { check(row); });
    fields(row).filter("input[type=text], input:not([type])").on("keydown", function(e) { if (e.key === "Enter" && dirty(row)) { e.preventDefault(); save(); } });
    row.find(".save").prop("disabled", true);
    rst(row);
  }
  /* The list again from the radio, the drafts kept */
  function rebuild(body, build) {
    const drafts = {}, news = [];
    body.children("tr").each(function() {
      const row = $(this), key = row.data("key");
      if (key === "" || key === undefined) { if (dirty(row) || row.find(".rst").attr("data-st") === "failed") news.push(row.detach()); return; }
      if (dirty(row)) drafts[key] = {vals: values(row), orig: row.data("orig"), st: row.find(".rst").attr("data-st"), text: row.find(".rst").text()};
    });
    build();
    const rows = {};
    body.children("tr").each(function() {
      const row = $(this), key = row.data("key"), d = drafts[key];
      rows[key] = row;
      if (!d) return;
      const fresh = row.data("orig");
      setValues(row, d.vals);
      if (fresh === d.orig) state(row, d.st === "failed" ? "failed" : "unsaved", d.st === "failed" ? d.text.replace(/^Failed: /, "") : "");
      else {
        row.data("orig", fresh);
        state(row, "changed", "Changed on the radio meanwhile: Save keeps yours, ").append($("<button type='button' class='ghost sm'>Reload</button>").on("click", function() { setValues(row, JSON.parse(fresh)); state(row, ""); }));
      }
    });
    news.forEach(function(row) { body.append(row); });
    tools(null, body);
    return rows;
  }
  /* "N unsaved · Save all · Discard", in the head of the card around the table */
  function tools(card, body) {
    card = card || body.closest(".dp-card");
    if (!card.length) return;
    let bar = card.find(".dp-tools");
    const rows = body.children("tr").filter(function() { const st = $(this).find(".rst").attr("data-st"); return st === "unsaved" || st === "changed"; });
    if (!rows.length) { bar.remove(); return; }
    if (!bar.length) bar = $("<div class='dp-tools'>").insertAfter(card.children(".dp-head"));
    bar.empty().append($("<span>").text(rows.length + " unsaved"),
      $("<button type='button' class='primary sm'>Save all</button>").on("click", function() { rows.each(function() { $(this).find(".save").trigger("click"); }); }),
      $("<button type='button' class='ghost sm'>Discard</button>").on("click", function() {
        rows.each(function() {
          const row = $(this);
          if (row.data("key") === "" || row.data("key") === undefined) row.remove(); else { setValues(row, JSON.parse(row.data("orig"))); state(row, ""); }
        });
        tools(card, body);
      }));
  }
  function confirmDelete(what, lost) {
    return confirm("Delete " + what + "?" + (lost ? "\n\n" + lost : "") + "\n\nUndo is offered for twenty seconds afterwards.");
  }
  function undo($status, text, fn) {
    $status.removeClass("error").html('<span class="dot"></span>').append(document.createTextNode(text + " "),
      $("<button type='button' class='ghost sm'>Undo</button>").on("click", function() { $(this).prop("disabled", true); fn(); }));
    setTimeout(function() { $status.find("button").remove(); }, 20000);
  }
  function effect(el, text) { $(el).append($("<span class='eff'>").text("Takes effect: " + text)); }
  function form(container, $status, $save) {
    const c = $(container);
    const api = {
      reset: function() { c.data("orig", JSON.stringify(values(c))); api.check(); },
      check: function() {
        const d = c.data("orig") !== undefined && dirty(c);
        if ($save) $save.prop("disabled", !d);
        if (d) $status.addClass("form-unsaved").removeClass("error").html('<span class="dot"></span>').append(document.createTextNode("Unsaved"));
        else if ($status.hasClass("form-unsaved")) $status.removeClass("form-unsaved").empty();
      }
    };
    fields(c).on("input change", api.check);
    return api;
  }
  window.addEventListener("beforeunload", function(e) {
    if (document.querySelector(".rst[data-st=unsaved], .rst[data-st=changed], .form-unsaved")) { e.preventDefault(); e.returnValue = ""; }
  });
  return {snapshot: snapshot, watch: watch, dirty: dirty, state: state, check: check, values: values, setValues: setValues,
          rebuild: rebuild, tools: tools, confirmDelete: confirmDelete, undo: undo, effect: effect, form: form};
})();
