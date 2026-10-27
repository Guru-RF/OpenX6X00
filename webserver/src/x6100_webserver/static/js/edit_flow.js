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
 *   EditFlow.submit(row) -> tk        a save of the row goes out (Saving...): what it sends is kept
 *                                     with the row; 0 when one is on its way already
 *   EditFlow.saved(tk, render)        its answer: render() puts the radio's list on the page and
 *                                     returns the row as it is there now; Saved only if its fields
 *                                     still hold what was sent, else the newer edit stays Unsaved
 *   EditFlow.failed(tk, text)         its failure: Failed, the draft kept, Save tries again
 *   EditFlow.ajax(body, settings)     $.ajax for a change to the list in the table body: one request
 *                                     of a table on its way at a time, the next after its answer
 *   EditFlow.rebuild(body, build)     build() fills the table again from the radio's list; the rows
 *                                     being edited keep their drafts (a row the radio changed
 *                                     meanwhile says so: Reload takes the radio's, Save keeps yours)
 *                                     and a row being saved stays Saving until its own answer
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
  /* Unsaved, Changed and Failed rows hold what the radio does not have: Save (again), Save all, Discard and
   * the warning on leaving the page are theirs. A failed save was not retried without another edit (audit C12) */
  function held(st) { return st === "unsaved" || st === "changed" || st === "failed"; }
  function state(row, st, text) {
    const el = rst(row).attr("data-st", st).empty();
    if (st) el.append(document.createTextNode(text ? (st === "failed" ? "Failed: " + text : text) : TEXT[st]));
    row.toggleClass("unsaved", held(st));
    row.find(".save").prop("disabled", !held(st));
    const body = row.closest("tbody");
    if (body.length) tools(null, body);
    return el;
  }
  function check(row) {
    const st = row.find(".rst").attr("data-st");
    if (st === "saving") return;    /* the answer decides, against what was sent (saved) */
    if (st === "changed" && dirty(row)) return;
    if (dirty(row)) state(row, "unsaved"); else if (held(st)) state(row, "");
  }
  /* A save on its way: the values it sends stay with the row, through a rebuild for another row's answer,
   * until its own answer. One at a time: Save, Enter or Save all again meanwhile sends nothing */
  let tickets = 0;
  function submit(row) {
    if (row.find(".rst").attr("data-st") === "saving") return 0;
    const tk = ++tickets;
    row.attr("data-tk", tk).data("sent", JSON.stringify(values(row)));
    state(row, "saving");
    return tk;
  }
  /* What was typed while the save was on its way is the newer draft: it stays, Unsaved, and the row says
   * Saved only when its fields hold what the radio confirmed (audit C12). The row is found by its ticket
   * before render(), whose row may have a new key (an entry added or renamed) */
  function saved(tk, render) {
    const was = $("tr[data-tk='" + tk + "']"), typed = was.length ? JSON.stringify(values(was)) : null, sent = was.data("sent");
    was.removeAttr("data-tk").removeData("sent");
    const row = render();
    if (!row || !row.length) return;
    if (typed === null || typed === sent) { setValues(row, JSON.parse(row.data("orig"))); state(row, "saved"); }
    else { setValues(row, JSON.parse(typed)); state(row, dirty(row) ? "unsaved" : ""); }
  }
  function failed(tk, text) {
    const row = $("tr[data-tk='" + tk + "']").removeAttr("data-tk").removeData("sent");
    if (row.length) state(row, "failed", text);
  }
  /* The changes to one table go out one at a time, each after the answer to the one before: the web server
   * answers in threads, so of two on their way at once (Save all) the later answer could carry the older
   * list, and a row would show what the radio no longer has, or a new entry vanish (audit C12). A row
   * waiting its turn says Saving..., and sends what it held when Save was pressed */
  const queues = new WeakMap();
  function ajax(body, settings) {
    const el = $(body)[0];
    let q = queues.get(el);
    if (!q) { q = []; queues.set(el, q); }
    q.push(settings);
    if (q.length === 1) send(q);
  }
  function send(q) {
    const s = q[0];
    let over = false;
    const next = function() { if (over) return; over = true; q.shift(); if (q.length) send(q); };
    $.ajax(Object.assign({}, s, {
      success: function() { try { if (s.success) s.success.apply(this, arguments); } finally { next(); } },
      error: function() { try { if (s.error) s.error.apply(this, arguments); } finally { next(); } }}));
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
      const row = $(this), key = row.data("key"), st = row.find(".rst").attr("data-st");
      if (key === "" || key === undefined) { if (dirty(row) || st === "failed" || st === "saving") news.push(row.detach()); return; }
      if (dirty(row) || st === "saving") drafts[key] = {vals: values(row), orig: row.data("orig"), st: st, text: row.find(".rst").text(), tk: row.attr("data-tk"), sent: row.data("sent")};
    });
    build();
    const rows = {};
    body.children("tr").each(function() {
      const row = $(this), key = row.data("key"), d = drafts[key];
      rows[key] = row;
      if (!d) return;
      const fresh = row.data("orig");
      setValues(row, d.vals);
      if (d.tk) { row.attr("data-tk", d.tk).data("sent", d.sent); state(row, "saving"); }   /* its own answer is still on its way */
      else if (!dirty(row)) state(row, "");                                                   /* the radio has it now */
      else if (fresh === d.orig) state(row, d.st === "failed" ? "failed" : "unsaved", d.st === "failed" ? d.text.replace(/^Failed: /, "") : "");
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
    const rows = body.children("tr").filter(function() { return held($(this).find(".rst").attr("data-st")); });
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
  /* A row that holds a draft, failed or not, or whose save has no answer yet */
  window.addEventListener("beforeunload", function(e) {
    if (document.querySelector(".rst[data-st=unsaved], .rst[data-st=changed], .rst[data-st=failed], .rst[data-st=saving], .form-unsaved")) { e.preventDefault(); e.returnValue = ""; }
  });
  return {snapshot: snapshot, watch: watch, dirty: dirty, state: state, check: check, values: values, setValues: setValues,
          submit: submit, saved: saved, failed: failed, ajax: ajax,
          rebuild: rebuild, tools: tools, confirmDelete: confirmDelete, undo: undo, effect: effect, form: form};
})();
