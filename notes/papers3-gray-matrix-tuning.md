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

## Round 2 (photos of 2026-08-25)

Measured band luminances from the A/B selftest photo (left = shipped, right
= candidate B) plus a palette swatch that independently reproduces the left
half's defects:

```
level:   0    1    2    3    4    5    6    7    8    9   10   11   12   13   14   15
left :  40   53   56   74   91   81   78   96   96   67   81  103  115  120  120  113
right:  32   44   45   58   55   67   98   84   86   85  101  103  109  115  117  112
```

Candidate B fixed the level-9 collapse and put 10/11 in order, but level 6
overshot light (above 7, 8 and 9), level 4 dipped just under 3, and 7/8/9
clumped into one tone. The apparent 15-under-14 is mostly a shadow at the
photo's left edge; mid-panel, 14 and 15 merge rather than invert.

Refitting the linear pass-weight model on all 32 measurements (per-half
intercept to absorb the lighting gradient) gives R^2 = 0.91, but the
realised shifts from B's individual code flips were 2-4x the model's
predictions and inconsistent between rows (level 6's flip realised +0.34
normalised against a predicted +0.08; level 8's flip barely moved). The
glass response is state-dependent, not a linear sum of passes, so
extrapolated pass patterns cannot be trusted.

## Candidate C: reassignment, no extrapolation

The two rounds provide measured tones for 22 distinct pass patterns. C
assigns to each level the already-measured pattern closest to its target
tone, monotone in measured tone through level 14 (normalised to the
right-half 0/14 anchors):

```
level:   0     1     2     3     4     5     6     7     8     9    10    11    12    13    14    15
tone : 0.000 0.131 0.157 0.271 0.298 0.346 0.435 0.473 0.598 0.624 0.660 0.748 0.812 0.898 0.968 0.960
from :  A0    A1    A2    B4    A9    A3    A6    A10   A4    B9    A8    A11   B10   A12   A13   A15
```

(Ax = shipped matrix's level-x row, Bx = round-1 candidate's level-x row.)
Known residuals: nothing measured falls between 0.47 and 0.60, so the 7-8
step stays wide; and 14/15 stay nearly merged, because eight straight
lightens measures no lighter than the mixed 13/14 patterns on this glass.

## Verdict loop, round 2: the lettered sheet

The selftest now paints 16 vertical bars in bit-reversed level order
(0,8,4,12,2,10,6,14,1,9,5,13,3,11,7,15 left to right) with letters A..P
under each bar, shipped matrix on the top half and the candidate on the
bottom half. The reporter sorts each half's letters darkest to lightest; a
correctly ordered ramp reads

```
A I E M C K G O B J F N D L H P
```

and any deviation names the misplaced levels directly (letter -> position ->
level via the permutation). Human ordinal judgment is immune to the lighting
gradients that bias photo luminance, and the scramble both breaks the
monotonic-expectation illusion and turns any residual gradient into noise
rather than bias. Ties ("G and K look identical") are useful data too. A
photo alongside still supplies coarse interval information.

## If reassignment is not enough: ported tables

Web findings (2026-08-26): the shipped matrix descends from FastEPD's
u8M5Matrix, which upstream hand-tuned by eye on one unit and documents as
per-unit variable (the `gray_matrix_editor` example exists for exactly this
reason). Two publicly available better-founded tables, in order of porting
cost:

1. M5GFX `Panel_EPD.cpp` `lut_quality` (~lines 76-160): M5Stack's own
   15-frame, 16-level, target-indexed table for this exact product, plus a
   2-frame `lut_eraser` pre-shift. Codes 1=to-black, 2=to-white, 3=no-op,
   0=end. Port = transpose + code remap + raising GRAY_PASSES to 15.
2. epdiy `src/waveforms/epdiy_ED047TC2.h` (same data as LilyGo's published
   `ED047TC2.h`): vendor-derived, temperature-indexed (7 ranges, 15-38 C),
   full from-gray x to-gray transition matrices, 38-57 phases in GC16 mode.
   Fixed exactly this non-monotonic symptom in epdiy issue #171. Does not
   compress into 8 passes; long-term option.
