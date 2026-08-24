# PaperS3 grey matrix tuning (issue #21)

The first bring-up photos (2026-08-24, selftest bars + server palette swatch,
two independent shots under different light and opposite band order) show the
same greyscale defects on the reporter's unit:

- levels 5 and 6 render darker than level 4
- level 8 renders too light
- level 9 collapses to darker than level 4
- levels 10 and 11 lag behind where they should sit

The driver was compared line by line against upstream FastEPD (matrix bytes,
LUT construction, clear cycle, row control pulse widths, write-row ordering,
pass settle, i80 config, power sequencing): all identical, so the defects are
per-unit waveform character, not a porting bug.

## Measurement

Band mean luminances were sampled from both photos, normalised per photo on
the level-0 / level-15 anchors, then averaged:

```
level:  0     1     2     3     4     5     6     7     8     9    10    11    12    13    14    15
tone : 0.000 0.144 0.218 0.403 0.611 0.545 0.505 0.673 0.818 0.511 0.636 0.713 0.907 0.966 1.006 1.000
```

## Model

Each matrix row is 8 pass codes (1 darken / 2 lighten). Fitting
`tone = a + sum_p x_p * w_p` (x = +1 lighten, -1 darken) by least squares
gives per-pass weights

```
pass:    1       2       3       4       5       6       7       8
w   : +0.056  -0.007  +0.045  +0.063  +0.097  +0.048  +0.148  +0.136
```

at R^2 = 0.94: late passes dominate, pass 2 is noise. The rows that measure
wrong are exactly the ones balancing many darkens against late lightens
(row 9 = six darkens then two lightens), i.e. the rows most sensitive to
per-unit variation.

## Candidate B

Constraints: only touch rows that measure wrong, at most two code flips per
row, never flip pass 2 (weight indistinguishable from zero), keep rows 0 and
15 saturated. Changed-row tones are estimated as model prediction plus that
row's fitted residual; unchanged rows keep their measured tone. Result
(7 flips across 6 rows) with estimated tones:

```
level:  4      6      8      9      10     11
old  : 0.611  0.505  0.818  0.511  0.636  0.713
new  : 0.485  0.601  0.691  0.713  0.763  0.803   (estimated)
```

Full estimated ramp is monotone. Row 5 is deliberately untouched: its
realised tone runs ~0.09 above model, so level 6 was raised around it rather
than lowering 5 into uncertainty.

## Verdict loop

The selftest paints the shipped matrix on the first half of each scan row and
candidate B on the second half (`gray_passes_bands_ab`), so one flash + one
photo judges the retune. If B reads even, promote it to
`EPD_PAR_GRAY_MATRIX`; if not, re-measure from the new photo and refit. The
fitting script inputs are the band luminance vectors above; the procedure is
deterministic from there.
