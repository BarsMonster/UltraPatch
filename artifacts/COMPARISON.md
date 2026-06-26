# Sensor Watch Pro — build comparison

Board: **OSO-SWAT-C1-00** (COLOR=PRO) · MCU: Microchip SAM L22 (Cortex-M0+)
Toolchain: arm-gnu-toolchain 13.3.Rel1 · Optimization: `-Os` (release; project has no debug variant)
Build cmd: `cd movement/make && make COLOR=PRO`

## Change
Registered the pre-existing **`counter_face`** in `movement/movement_config.h`'s
`watch_faces[]` array (1 line). No new source files. The face's `.c` was already
compiled; `--gc-sections` had been stripping it because nothing referenced it.

## Artifacts
- `baseline/`            — unmodified PRO firmware
- `with_counter_face/`   — same + counter_face registered

| file      | baseline | with_face | delta |
|-----------|---------:|----------:|------:|
| watch.bin | 113124   | 113484    | +360  |
| watch.uf2 | 226304   | 227328    | +1024 (UF2 pads to 256-byte blocks: 442→446 blocks) |
| watch.elf | 183432   | 183868    | +436  |
| watch.hex | 318235   | 319241    | +1006 |

## Section deltas (arm-none-eabi-size)
| region | baseline | modified | delta |
|--------|---------:|---------:|------:|
| text (.text + rodata, → flash) | 111148 | 111508 | **+360** |
| data (.relocate, init RAM data) | 1976 | 1976 | **0** |
| bss  (zero-init RAM) | 12304 | 12328 | **+24** |
Flash use: 45.23% → 45.37% of 240 KB. RAM: 43.58% → 43.65% of 32 KB.

## Symbol-level diff (7 added, 0 removed) — all from counter_face.c
| symbol | bytes | where |
|--------|------:|-------|
| beep_counter        | 132 | .text |
| counter_face_loop   | 130 | .text |
| print_counter       |  28 | .text |
| counter_face_setup  |  26 | .text |
| counter_face_activate | 16 | .text |
| counter_face_resign |   2 | .text |
| sound_seq.0         |  15 | .rodata (const tune table) |

## Existing symbols that grew (driven by MOVEMENT_NUM_FACES 9 → 10)
| symbol | baseline | modified | region |
|--------|---------:|---------:|--------|
| watch_faces[]          | 180 | 200 | .rodata  (+1 × watch_face_t = 5 ptrs = 20 B) |
| watch_face_contexts[]  |  36 |  40 | .bss     (+1 × void* = 4 B) |
| scheduled_tasks[]      |  36 |  40 | .bss     (+1 × watch_date_time = 4 B) |

## Byte budget
- text +360 ≈ 334 (6 new functions) + 15 (sound_seq const) + 20 (watch_faces entry) − ~9 alignment slack
- bss  +24  ≈ 4 (watch_face_contexts) + 4 (scheduled_tasks) + alignment/padding
- data +0   — no new initialized data
The entire diff is attributable to the one added face plus the face-count-dimensioned
bookkeeping arrays. Nothing else in the firmware changed.
