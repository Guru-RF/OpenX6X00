# Code audits October 2026

The latest review is [Codex-Audit-2026-10-26.md](Codex-Audit-2026-10-26.md), covering revision `e4f0dc7` (`v0.3.6`). It records 14 current findings, C01 to C14, the disposition of B01 to B17, and improvement ideas I01 to I09. Commits that fix them are named "audit C01" and so on.

The review before it, [Codex-Audit-2026-10-24.md](Codex-Audit-2026-10-24.md), covered revision `00f91e1` (`v0.3.1`): 17 findings, B01 to B17, and the disposition of all 22 original findings. [Reproduction harnesses](repro-2026-10-24/README.md) accompany it. All 17 are fixed in the commits that follow `4cad3f5` (one per finding, named "audit B01" to "audit B17"); the harnesses were flipped with each fix and run as a regression suite.

The original review of revision `d6bf443` (2026-10-22) is preserved as the record the earlier fixes refer to by ID:

- [Codex-Audit.md](Codex-Audit.md): 22 correctness findings, A01 to A22, with evidence and priorities.
- [Codex-Possible-Improvements.md](Codex-Possible-Improvements.md): usability ideas, U01 to U12.
- [Codex-Remote-Improvements.md](Codex-Remote-Improvements.md): browser remote ideas, R1 to R8. Written before the browser panel landed; its stages 1 to 4 are shipped.

Commits that close a finding name its review ID, for example "audit A09" or "audit B01".
