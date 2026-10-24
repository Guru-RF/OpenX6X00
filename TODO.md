# To do

## FT8/FT4/FT2: answer other stations' CQs by themselves (when not worked yet)

Asked 2026-10-24. Today Auto answers the stations that call *you*, also while you call CQ (what
other programs call "Res"), and sends the next message of a QSO by itself; it never answers
somebody else's CQ (docs/MANUAL.md, FT8 section, "Auto"). The wish: an automatic hunt mode that
answers a CQ it decodes when that station is not in the log yet for this band and mode.

- **Worked before, with a time window**: "not in the log" counts QSOs within a window the
  operator chooses: this band and mode ever, this year, the last month, the last week, today.
  The log already answers worked / worked in this mode for a call and band
  (`qso_log_search_worked()` in src/qso_log.c, used for the list's colours); it needs a date
  limit (`QSO_DATE`/`TIME_ON` in the qso_log table) and the window as a setting.
- **Which CQs**: honour the CQ's modifier (`CQ DX` from the own continent, `CQ NA`, `CQ POTA`...
  only when it applies), skip stations already being worked, prefer the strongest or a POTA/
  SOTA/TOTA... activator (the cluster's activity and the FT8 POTA tag), never answer while a QSO
  of our own is going on, keep Hold Freq and TX Call as they are.
- **Safety**: an explicit switch, off at every start (the radio transmits by itself), visible on
  the screen and in the browser panel; a stop after N answers without a reply or after a time;
  the same retries as a normal QSO; logged as usual.
- **Other digital modes**: FT4 and FT2 share the FT8 app and come with it. JS8 deliberately
  never transmits to the group by itself (MANUAL, JS8 section): an auto answer there would be a
  separate decision. RTTY/PSK are keyboard modes and WSPR a beacon: not candidates.
