# Third-party notices

ps3recomp itself is MIT, (c) Ned Heller / sp00nznet. Third-party components
and method credits are listed here.

## NHL Legacy Native (method credit)

The c690 native Edge work, including `tools/c690_ref/`, follows the stage
split and the corpus/differential method of the NHL Legacy Native port
(`#268`, `#291`, `#292`), which is MIT:

> Portions of the method derived from NHL Legacy Native
> (c) 2026 HeckYouMan, MIT.

The NHL port's `edge/edge_ref.c`, `edge/cullsim.py` and
`nhl/port/nhl_edge_native.c` were studied as prior art. No NHL code is copied
into this repository: MLB The Show 16 uses a different per-game Edge layout,
so the c690 reference and kernels are derived from MLB's own lifted
disassembly. If a future change does translate NHL code, keep the MIT notice
in that file and extend this entry.
