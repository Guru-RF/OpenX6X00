"""Bands and digital mode frequencies in params.db, for the Bands and Digital modes pages.

Same tables the radio GUI reads (sql/params.sql): `bands` with its per-band values in
`band_params` (vfoa_freq, vfob_freq, vfoa_mode...), and `digital_modes`."""

import math
import sqlite3

LSB, USB, AM, NFM, WFM = 0, 2, 6, 7, 9      # x6100_mode_t
TX_SHIFT_MAX = 10_000_000                   # Hz, the GUI's SettingsManager::TX_SHIFT_MAX
HF_MIN, HF_MAX = 100_000, 55_000_000        # where the radio transmits without a transverter
BAND_KEYS = ("name", "start_freq", "stop_freq", "type", "id")
FREQ_KEYS = ("label", "freq", "mode", "type", "id")


class Invalid(ValueError):
    """Something the page has to correct; the message is shown to the user."""


def _ordered(lo, hi):
    try:
        return lo < hi
    except TypeError:           # NULL or text in a hand-edited database
        return False


def _whole(value, what):
    """An integer from the request JSON. The pages send MHz * 1e6, so 7.074 MHz arrives as
    7073999.999999999: rounded, not cut to 7073999 as sqlite would store it for the GUI."""
    if isinstance(value, int):
        return int(value)
    if isinstance(value, float) and math.isfinite(value):
        return round(value)
    raise Invalid(f"Invalid {what}")


# Bands

def bands(con):
    """Every band by start frequency, with its band_params as "params" (rows with broken edges left out)."""
    out = []
    for row in con.execute("SELECT name, start_freq, stop_freq, type, id FROM bands ORDER BY start_freq"):
        band = dict(zip(BAND_KEYS, row))
        if _ordered(band["start_freq"], band["stop_freq"]):
            out.append(band)
    for band in out:
        band["params"] = {name: val for name, val in
                          con.execute("SELECT name, val FROM band_params WHERE bands_id = ?", (band["id"],))}
    return out


def _band_input(data):
    if not isinstance(data, dict) or not {"name", "start_freq", "stop_freq", "type"} <= data.keys():
        raise Invalid("Invalid band: name, start_freq, stop_freq and type are needed")
    if not isinstance(data["name"], str):
        raise Invalid("Invalid name")
    params = data.get("params", {})
    if not isinstance(params, dict) or not all(isinstance(k, str) for k in params):
        raise Invalid("Invalid params")
    band = {"name": data["name"], "start_freq": _whole(data["start_freq"], "start freq"),
            "stop_freq": _whole(data["stop_freq"], "stop freq"), "type": _whole(data["type"], "type"),
            "params": {k: _whole(v, k) for k, v in params.items()}}
    if band["start_freq"] >= band["stop_freq"]:
        raise Invalid("Stop freq should be greater than start freq")
    return band


def default_mode(freq):
    """The mode a new band opens in, as the GUI's default_mode() has it: LSB below 10 MHz but USB on 60 m, USB
    above; above HF (55 MHz) NFM, wide FM and AM on the FM broadcast and airband (87.5-137 MHz)."""
    if 87_500_000 <= freq < 108_000_000:
        return WFM
    if 108_000_000 <= freq <= 137_000_000:
        return AM
    if freq > 55_000_000:
        return NFM
    if 5_250_000 <= freq <= 5_450_000:
        return USB
    return LSB if freq < 10_000_000 else USB


def hf_tx(lo, hi):
    """True when the radio transmits somewhere in [lo, hi] Hz, by HF alone: what is known without the GUI, which knows
    the transverters too (band_use, the GUI socket's "band_use")."""
    return lo <= HF_MAX and hi >= HF_MIN


def _mhz(hz):
    return f"{hz / 1e6:.3f}"


def _check_tx_shift(band, shift, band_tx):
    """A TX shift (Hz, 0 off) the band can take: up to 10 MHz either way, on a band the radio transmits on, and the
    band shifted still one it transmits on. band_tx(lo, hi): does the radio transmit somewhere in [lo, hi]."""
    if not shift:
        return
    if abs(shift) > TX_SHIFT_MAX:
        raise Invalid("TX shift: 10000 kHz at most, up or down")
    lo, hi = band["start_freq"], band["stop_freq"]
    if not band_tx(lo, hi):
        raise Invalid(f'No TX shift on "{band["name"]}": the radio does not transmit on {_mhz(lo)}-{_mhz(hi)} MHz')
    if not band_tx(lo + shift, hi + shift):
        raise Invalid(f"TX shift {shift / 1000:g} kHz: the radio does not transmit on "
                      f"{_mhz(lo + shift)}-{_mhz(hi + shift)} MHz")


def _check_overlap(band, others):
    lo, hi = band["start_freq"], band["stop_freq"]
    for other in others:
        a, b = other["start_freq"], other["stop_freq"]
        if a < lo < b:
            raise Invalid(f'Start freq {lo} overlap with band "{other["name"]}"')
        if a < hi < b:
            raise Invalid(f'Stop freq {hi} overlap with band "{other["name"]}"')
        if (a <= lo and hi <= b) or (lo <= a and b <= hi):
            raise Invalid(f'Band "{band["name"]}" overlap with band "{other["name"]}"')


def _set_param(con, band_id, name, val):
    if con.execute("UPDATE band_params SET val = ? WHERE bands_id = ? AND name = ?", (val, band_id, name)).rowcount == 0:
        con.execute("INSERT INTO band_params (bands_id, name, val) VALUES (?, ?, ?)", (band_id, name, val))


def add_band(con, data, band_tx=hf_tx):
    """A new band; a TX shift in its params checked (band_tx: _check_tx_shift), split on with it."""
    band = _band_input(data)
    _check_overlap(band, bands(con))
    shift = band["params"].get("tx_shift", 0)
    _check_tx_shift(band, shift, band_tx)
    band_id = con.execute("INSERT INTO bands (name, start_freq, stop_freq, type) VALUES (?, ?, ?, ?)",
                          (band["name"], band["start_freq"], band["stop_freq"], band["type"])).lastrowid
    params = band["params"]
    params.setdefault("vfoa_freq", band["start_freq"])
    params.setdefault("vfoa_mode", default_mode(band["start_freq"]))
    if shift:
        params["split"] = 1     # as the GUI's tx_shift_set: the shift works in split
    con.executemany("INSERT INTO band_params (bands_id, name, val) VALUES (?, ?, ?)",
                    [(band_id, k, v) for k, v in params.items()])


def update_band(con, band_id, data, band_tx=hf_tx):
    """New name, edges, Active flag, mode and TX shift. A VFO frequency left outside the new edges moves to the start.
    A new TX shift turns split on (off with 0), as on the radio; one checked again when the edges change."""
    band = _band_input(data)
    others = bands(con)
    old = next((b for b in others if b["id"] == band_id), None)
    if old is None:
        raise Invalid(f"Band parameters with id={band_id} not found")
    others.remove(old)
    _check_overlap(band, others)
    old_shift = old["params"].get("tx_shift", 0)
    shift = band["params"].get("tx_shift", old_shift)
    if shift != old_shift or (old["start_freq"], old["stop_freq"]) != (band["start_freq"], band["stop_freq"]):
        _check_tx_shift(band, shift, band_tx)
    con.execute("UPDATE bands SET name = ?, start_freq = ?, stop_freq = ?, type = ? WHERE id = ?",
                (band["name"], band["start_freq"], band["stop_freq"], band["type"], band_id))
    for vfo in ("vfoa_freq", "vfob_freq"):
        if vfo in old["params"] and not band["start_freq"] <= old["params"][vfo] <= band["stop_freq"]:
            _set_param(con, band_id, vfo, band["start_freq"])
    if "vfoa_mode" in band["params"]:
        _set_param(con, band_id, "vfoa_mode", band["params"]["vfoa_mode"])
    if shift != old_shift:      # unchanged: a split set by hand stays as it is
        _set_param(con, band_id, "tx_shift", shift)
        _set_param(con, band_id, "split", int(shift != 0))


def delete_band(con, band_id):
    con.execute("DELETE FROM bands WHERE id = ?", (band_id,))
    con.execute("DELETE FROM band_params WHERE bands_id = ?", (band_id,))


# Digital mode frequencies (FT8/FT4/JS8 lists)

def digital_modes(con):
    return [dict(zip(FREQ_KEYS, row))
            for row in con.execute("SELECT label, freq, mode, type, id FROM digital_modes ORDER BY freq ASC")]


def _freq_input(data):
    if not isinstance(data, dict) or not {"label", "freq", "mode", "type"} <= data.keys():
        raise Invalid("Invalid frequency: label, freq, mode and type are needed")
    if not isinstance(data["label"], str):
        raise Invalid("Invalid label")
    return (data["label"], _whole(data["freq"], "frequency"), _whole(data["mode"], "mode"), _whole(data["type"], "type"))


def _write_freq(con, sql, values):
    """The table refuses a second entry with the same frequency and type, a mode out of range, freq <= 0."""
    try:
        return con.execute(sql, values)
    except sqlite3.IntegrityError as e:
        raise Invalid("This frequency is already in the list of that app" if "UNIQUE" in str(e)
                      else "Invalid frequency or mode")


def add_digital_mode(con, data):
    _write_freq(con, "INSERT INTO digital_modes (label, freq, mode, type) VALUES (?, ?, ?, ?)", _freq_input(data))


def update_digital_mode(con, mode_id, data):
    """False when there is no such entry."""
    cur = _write_freq(con, "UPDATE digital_modes SET label = ?, freq = ?, mode = ?, type = ? WHERE id = ?",
                      _freq_input(data) + (mode_id,))
    return cur.rowcount > 0


def delete_digital_mode(con, mode_id):
    con.execute("DELETE FROM digital_modes WHERE id = ?", (mode_id,))
