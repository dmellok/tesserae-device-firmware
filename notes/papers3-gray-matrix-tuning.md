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

## Round-2 result and candidate C2 (2026-08-26)

The reporter's photo (comment 5416708006, updated with per-half data +
v2 ODS in 5417662747) confirmed candidate C: his bottom-half ordering and
an independent per-half measurement of the same photo agree letter for
letter except the 11/12 pair (both datasets < 1% apart there: a tie).
Lighting was ruled out by anchor bars: the six patterns identical in both
halves moved <= 3 luminance units top-to-bottom, the retuned ones 16-28.

Bottom-half tones (0-255 luminance, independent measurement):

```
level:   0     1     2     3     4     5     6     7     8     9    10    11    12    13    14    15
tone :  29.6  58.6  67.2 101.9 109.8 103.8 109.8 154.0 124.8 150.8 155.8 165.5 166.5 181.5 179.1 192.9
```

Misorders: 4/5 inverted, 7/8/9 out of order (needs 124.8 / 150.8 / 154.0,
i.e. patterns rotated one step: 7 takes old 8's, 8 old 9's, 9 old 7's),
13/14 inside noise. The reporter hand-edited the matrix and flashed it:
his 4/5 swap matches; his 7/8/9 rotation went the OPPOSITE way (9's
pattern to 7), and his eyes-only ordering shows the resulting signature
(level 9 right after 6), which is what pinned the correct direction. His
11/12 swap chases the tie; his 13/14 swap he reverted himself ("still
inverted"): that pair is likely this glass's resolution limit.

C2 (in the board header as of this commit) = C with 4/5 swapped and 7/8/9
rotated the ascending way. Nothing else moved. Residuals accepted: the
124.8 -> 150.8 hole between levels 7 and 8 (no measured pattern falls in
it; only a new waveform could) and the 13/14 tie.

## Verdict loop, round 3: ordered sheet added

Per the reporter's request, the selftest now paints two frames: the
scrambled measurement sheet as before, then after 45 s (rails down during
the dwell) the same bars in level order with white gutters, for the
"does it look finished" check by eye. Ordered bars keep their scrambled-
sheet letters (strip spells A I E M C K G O B J F N D L H P), so bar
names stay stable across both sheets.

## Round 3 result (2026-08-26, comment 5423825693)

Reporter's morning-daylight photos of both sheets, C2 on the bottom half.
His eye claims ("K,G darker than M,C; J and N darker than O,B") decode to
levels 5,6 < 3,4 and 9,11 < 7,8. His eye report is taken as authoritative
per the thread's standing commitment (and his closing line, "not that I
need perfect grey ramp but trying to make it ready for anyone's to use",
reads as loop fatigue: do not request a round 4).

Photo medians below are secondary evidence only (iPhone local tone
mapping can shift regional tones by more than the level gaps; where they
disagree with the reporter's eyes, the eyes win). For what they are
worth (median per bar, flat bars confirmed by sub-bar sampling;
scrambled / ordered are separate photos, exposures differ):

```
level:      0    1    2    3    4    5    6    7    8    9   10   11   12   13   14   15
scr top:   23   40   52   66   84   81   74   94   99   78   94  107  113  118  118  123
scr C2 :   17   30   38   57   60   71   67   87   94   84  104  104  108  115  115  130
ord top:   15   31   40   63   89   81   70   94  109   70   92   97  125  133  137  126
ord C2 :    9   26   33   60   62   66   70   93   98   89  124  100  124  133  140  146
```

C2 bottom-half residuals:

- Levels 0-8 and 12-15 are monotone on the ordered (sign-off) sheet.
- Level 9 renders below BOTH 7 and 8 on both sheets (consistent, real).
  The A10 pattern assigned to 9 realised ~0.59 normalised here vs 0.76
  when the same pattern sat at level 7 in round 2, with no code change.
- 10/11 disagree between sheets: dead tie on scrambled (104/104),
  inverted on ordered (124/100). A8's realised tone moves ~0.10
  normalised with context.

Conclusion: pattern tones on this glass are context-dependent enough that
reassignment cannot reliably order the midtones; the approach has hit
its floor, and the reporter is done with tuning rounds. Decision
2026-08-26: close the loop. Promote the candidate with level 9 given
level 8's pattern (turns the inversion he flagged into a tie, safe by
construction), leave the rest untouched, and escalate to the M5GFX
lut_quality port (section above) as the real fix. That port is also the
honest answer to "ready for anyone to use": a matrix tuned to one unit's
photos was never going to generalise across units. No further testing to
be requested from the reporter; he flashes the vendor-table release when
it lands, or not.
