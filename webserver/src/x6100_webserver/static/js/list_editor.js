/* Bands and Digital modes pages: the entries of a JSON list API in a table, one row edited at a time.
 * PUT url adds, POST url/<id> changes, DELETE url/<id> removes; after each change the list is read again.
 *
 * opts.table   the <table> (its tbody is filled)
 * opts.add     the "add" button
 * opts.url     the API
 * opts.columns [{cls, show(entry) -> text or node, edit(entry) -> input}], in the order of the table
 * opts.read    inputs -> the JSON to send
 * opts.blank   () -> a new entry (id null)
 * opts.what    entry -> its name in the "Remove ...?" question */
function listEditor(opts) {
  const body = $(opts.table).children("tbody");

  function button(cls, icon) {
    return $("<button class='action'>").addClass(cls).append($("<i>").addClass(icon));
  }

  function failed(xhr) {
    const r = xhr.responseJSON;
    alert(r && r.msg ? r.msg : "The radio did not answer (HTTP " + xhr.status + ")");
  }

  function load() {
    $.getJSON(opts.url, function(list) {
      body.empty();
      list.forEach(function(entry) { body.append(show($("<tr>"), entry)); });
    });
  }

  function show(tr, entry) {
    tr.empty();
    opts.columns.forEach(function(c) { $("<td>").addClass(c.cls || "").append(c.show(entry)).appendTo(tr); });
    $("<td class='action-cell'>").append(
      button("edit", "si-edit").on("click", function() { edit(tr, entry); }), " ",
      button("remove", "si-trash").on("click", function() { remove(entry); })).appendTo(tr);
    return tr;
  }

  function edit(tr, entry) {
    body.find(".action").hide();
    const inputs = opts.columns.map(function(c) { return c.edit(entry); });
    tr.children("td").each(function(i) { if (i < inputs.length) $(this).empty().append(inputs[i]); });
    tr.children(".action-cell").empty().append(
      button("save", "si-disk").on("click", function() { save(entry, inputs); }), " ",
      button("close", "si-close").on("click", function() { close(tr, entry); }));
  }

  function close(tr, entry) {
    if (entry.id === null) tr.remove();
    else show(tr, entry);
    body.find(".action").show();
  }

  function save(entry, inputs) {
    const add = entry.id === null;
    $.ajax({url: add ? opts.url : opts.url + "/" + entry.id, type: add ? "PUT" : "POST",
            data: JSON.stringify(opts.read(inputs)), contentType: "application/json; charset=utf-8",
            dataType: "json", success: load, error: failed});
  }

  function remove(entry) {
    if (!confirm("Remove " + opts.what(entry) + "?")) return;
    $.ajax({url: opts.url + "/" + entry.id, type: "DELETE", success: load, error: failed});
  }

  $(opts.add).on("click", function() {
    const entry = opts.blank(), tr = show($("<tr>"), entry);
    body.prepend(tr);
    edit(tr, entry);
  });
  load();
}

/* Helpers for frequency columns: Hz in the API, MHz on the page */
function mhzText(hz) { return (hz / 1e6).toFixed(3); }
function mhzInput(hz) { return $("<input type='number' step='0.001' min='0'>").val(hz / 1e6); }
function mhzToHz(input) { return Math.round(input.val() * 1e6); }   // 7.074 * 1e6 is 7073999.999999999
function modeTag(mode) { return mode_map[mode] ? $("<span class='tag'>").text(mode_map[mode]) : ""; }
function choice(names, value) {   // a <select> of names (array or object), value selected
  const sel = $("<select>");
  for (const k in names) sel.append($("<option>").val(k).text(names[k]).prop("selected", k == value));
  return sel;
}
