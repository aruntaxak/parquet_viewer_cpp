# AGENTS.md — Parquet Viewer

Guidelines for AI agents and human contributors working in this repo.

## Overview

Native macOS parquet viewer in C++: Dear ImGui rendered through Metal, parquet and
Arrow from the pyarrow wheel, built by `clang++` with no build system. The whole
application is `main.cpp`, one Objective-C++ translation unit (1,825 lines,
69,828 bytes). It opens a file by reading the footer only, then pages the data
1,500 rows at a time.

**Key invariants:**

- `PAGE_ROWS = 1500` (`main.cpp:52`). Never load the whole file. 1,500 covers
  a full 24 h trading day (1,440 one-minute bars) plus context.
- Footer first: `parquet::ParquetFileReader::Open()` gives rows, columns, row
  groups, schema and chunk metadata. No data page is read until a page loads.
- A page read is `FileReader::GetRecordBatchReader({row_group}, column_indices)`,
  then a row slice. Arrow C++ 19 has no `with_offset` / `with_limit` and no
  `ProjectionMask`, so the whole row group is decoded; the last decoded group is
  kept in `cache_rg` / `cache_batches` so the next page inside it is a slice.
- Default page is the last page, scrolled to the bottom (`pending_scroll == 2`);
  `Show all` / `Tail 1500` toggles which end a file opens from.
- Only the visible rows are drawn; one full-height `ImGui::Dummy` keeps the
  scrollbar proportional to the page.

## Stack & versions

| Part | Version / path |
|---|---|
| Compiler | Apple clang 21.0.0, `-std=c++20 -O2` |
| Platform | macOS 27.0.1, arm64 |
| GUI | Dear ImGui 1.93.1 WIP, vendored in `imgui/` (MIT) |
| Data | Arrow C++ / parquet-cpp 19.0.1 from pyarrow's wheel |
| Font | `/System/Library/Fonts/SFNSMono.ttf` |
| Frameworks | Cocoa, AppKit, Metal, QuartzCore, Foundation, GameController |

- `main.cpp` is compiled **with** ARC (`-fobjc-arc`), as Objective-C++
  (`-x objective-c++`).
- `imgui_impl_osx.mm` and `imgui_impl_metal.mm` are compiled **without** ARC
  (`-fno-objc-arc`): both keep Objective-C pointers inside C structs and `memset`
  them, which ARC rejects.
- `-framework GameController` is required: `imgui_impl_osx.mm` references
  `GCController` even though the app never uses it.

## File layout

```
main.cpp                      # the whole app
build.sh                      # clang++ build: 4 steps, then link
imgui/                        # imgui*.cpp/h, backends/, LICENSE.txt
parquet-viewer                # built binary
build/                        # object files, created by build.sh
```

The directory is **not** a git repository.

## Build / run / verify

```bash
./build.sh                                        # clean build takes 6.5 s
./parquet-viewer ~/data.parquet                   # run
PYARROW_DIR=/path/to/pyarrow ./build.sh           # non-default pyarrow location
```

`build.sh` locates pyarrow with `python3 -c "import pyarrow"`, then:

1. compiles `imgui`, `imgui_draw`, `imgui_tables`, `imgui_widgets` (plain C++),
2. compiles the two macOS backends without ARC,
3. compiles `main.cpp` as Objective-C++ with ARC,
4. links `libparquet.*.dylib` and `libarrow.*.dylib` with
   `-Wl,-rpath,<pyarrow dir>`.

The rpath is absolute, so moving or upgrading pyarrow breaks the existing binary
until it is rebuilt with `PYARROW_DIR=...`.

Headless checks, no window is shown:

```bash
PARQUET_SELFTEST=1 ./parquet-viewer ~/data/file.parquet
PARQUET_SELFTEST=1 PARQUET_SELFTEST_DATE=2024-05-01 ./parquet-viewer ~/data/file.parquet
PARQUET_SELFTEST=1 PARQUET_SELFTEST_VIEW=grid PARQUET_SELFTEST_PNG=/tmp/g.png ./parquet-viewer ~/data/file.parquet
```

`PARQUET_SELFTEST_VIEW=grid|layout` renders three frames offscreen to a PNG. Three
frames are needed because a scroll request is applied to the next frame. This is
the only way to check the UI when there is no screen access.

Cross-check data against pyarrow directly:

```bash
python3 -c "
import pyarrow.parquet as pq
t = pq.ParquetFile('BTCUSDT-1m.parquet').read().slice(120000, 3).to_pylist()
print(t)"
```

Test files: `~/Documents/Crypto/BTCUSDT-1m.parquet` (95,580,670 bytes, 4,854,079
rows × 6 columns, 40 row groups of 121,352 rows, 3,237 pages, epoch-second `Time`)
and `~/Documents/Crypto/BTCUSDT-1D.parquet` (3,377 rows, 1 row group, ISO text
`Date`).

## Source map (`main.cpp`)

| Area | Where |
|---|---|
| civil-date maths, `timestamp_str()` | `:116`, `:138`, `:147` |
| `cell_text()` — one arrow cell as a grid string | `:180` |
| `stat_strings()` — chunk min/max/null count | `:280` |
| `build_layout()` — the whole Layout tab payload | `:339` |
| `struct App` | `:405` |
| `clear_cache()` / `fill_cache()` / `read_range()` — cached row-group read + slice | `:457`, `:465`, `:482` |
| `reset_column_widths()` | `:513` |
| `load_page()` / `load_at()` | `:528`, `:554` |
| `open_path()` | `:572` |
| `rg_outside()` — chunk min/max skip for `goto_date()` | `:643` |
| `goto_date()` | `:662` |
| `draw_cell_text()` — centred/left text, truncation, faux bold | `:774` |
| `struct UI` — `toolbar()` `:889`, `grid()` `:996`, `layout_view()` `:1149`, `draw()` `:1296` | `:883` |
| `ParquetView` — NSView, drag & drop | `:1346` |
| `apply_style()`, `resolve_arg()`, `main()` | `:1419`, `:1460`, `:1483` |

### Data layer

- `cell_text()` switches on `type_id()`: `STRING`, `LARGE_STRING`, `INT32`,
  `INT64`, `FLOAT`, `DOUBLE`, `BOOL`, `DATE32` (as `YYYY-MM-DD`), `TIMESTAMP`
  (UTC `YYYY-MM-DDTHH:MM:SS`, fraction only when non-zero). Anything else falls
  back to `Array::GetScalar(i)->ToString()`. `IsNull(i)` returns an empty string.
- `stat_strings()` formats min/max per physical type; byte-array and FLBA values
  go through `ba_to_string()`, which prints text when printable and hex otherwise.
- `build_layout()` reads file size with `std::filesystem`, maps
  `meta->version()` (`ParquetVersion::PARQUET_1_0` → 1, otherwise 2 — it is an
  enum, not the footer integer), and fills schema, key-value metadata and every
  row group's chunks.
- `read_range()` finds the first row group containing `offset`, then takes rows
  from `fill_cache()` — which decodes a group into `cache_batches` only when
  `cache_rg` is a different group or `cache_indices` differs — and writes into a
  flat row-major `std::vector<std::string>` (`cells`). A page inside the cached
  group costs 0.3 ms instead of 5.2 ms; the held group is ~6 MB.
- `goto_date()` skips row groups through `rg_outside()`, which uses the INT32 /
  INT64 chunk statistics already read for the Layout tab: a group is skipped when
  `max < lo` or `min >= hi`. The scan then compares `Int32Array` / `Int64Array`
  values directly instead of building a string per row, so 2024-05-01 on the 40
  group test file drops from ~96 ms to ~12.5 ms.

### App state

```
path, total_rows, columns, col_indices, cells (row-major strings),
page_rows, page_start, rg_first, rg_rows, col_widths,
current_page, total_pages, show_all, show_layout, schema_open, kv_open,
status, sel_year, sel_month, sel_day, expanded_rgs, layout, has_layout,
last_scroll_x, last_window_w, pending_scroll (0 none, 1 top, 2 bottom, 3 row),
pending_row, resize_col/valid/start_x/left/right, footer, fred,
cache_rg, cache_indices, cache_batches
```

### `goto_date()`

1. Date column = first column whose name contains `date`, else column 0.
2. Samples up to 5 values. If all are digits, length 10 / 13 / 16 selects a
   multiplier of 1 / 1,000 / 1,000,000; a mixed sample means text.
3. For an epoch, the `YYYY-MM-DD` selection becomes the UTC day
   `[00:00, next 00:00)` and is compared as an integer range.
4. For text, it builds permutations (`yyyy-mm-dd`, `dd-mm-yyyy`, `dd/mm/yyyy`,
   `yyyy/mm/dd` from either input order) and matches equality, `contains`, or the
   first 10 characters.
5. Scans row groups in order with a single-column reader, skipping groups whose
   statistics cannot contain the day and comparing integer arrays as numbers. On a
   hit it calls `load_at(found - 1)` and `pending_scroll = 3` with
   `pending_row = 1`, so the matched row is the second visible row.

### UI composition

- `draw()` opens one borderless full-viewport window (`NoTitleBar`, `NoResize`,
  `NoMove`, `NoCollapse`, `NoScrollbar`, `NoScrollWithMouse`,
  `NoBringToFrontOnFocus`, `NoNavFocus`), calls `toolbar()` then `grid()` or
  `layout_view()`, and sets the NSWindow title to `Parquet Viewer — <path>`.
- `toolbar()` is one row: `Open`, `Show all`/`Tail 1500`, `Data`, `Layout`,
  `Prev`, `Page n/m`, `Next`, `Go to Date`, day, month, year, `Go`, then the
  status text clipped to the remaining width. A 1 px rule follows.
- `grid()` draws the header with `ImDrawList` at `origin.x - last_scroll_x`, puts
  5 px `InvisibleButton` resize handles over the column boundaries, then opens a
  child with `ImGuiWindowFlags_HorizontalScrollbar` and draws only the rows in
  `[floor(scroll_y / GRID_ROW_H), +viewport + 2]`.
- Cell x starts at `window_pos.x - scroll_x`; the child has no padding, so the
  content pixel origin is the window origin minus the scroll offset.
- `layout_view()` uses collapsible buttons (`v` / `>`) for Schema, key-value
  metadata and each row group; the chunk table scrolls horizontally.
- `apply_style()` sets white background, greys at 0.90 / 0.84 / 0.77, 12 px
  scrollbars, zero rounding except scrollbar/grab, and
  `style.FontScaleMain = 1 / backing_scale` so a font loaded at
  `12 * backing_scale` still renders at 12 logical points.
- The main loop sleeps in `nextEventMatchingMask:untilDate:[NSDate distantFuture]`
  while `g_need_frame` is false. That is why idle CPU is 0.0 %.

## Constants

| Name | Value | Use |
|---|---|---|
| `PAGE_ROWS` | 1500 | rows per page |
| `UI_FONT` | 13.0f | toolbar text |
| `GRID_ROW_H` | 23.6f | cell height and row pitch |
| `GRID_MIN_COL_W` | 48.0f | column width floor |
| `RESIZE_HANDLE_W` | 5.0f | divider hit area |
| `DATE_FIELD_W` / `YEAR_FIELD_W` | 38.0f / 56.0f | dropdown widths |
| window | 750 × 950, min 700 × 500 | `main()` |

## Pitfalls

1. **`PAGE_SIZE` is a macro.** `mach/vm_param.h`, pulled in by Metal, defines
   `PAGE_SIZE`; a constant called `PAGE_SIZE` fails with an unreadable
   `std::min` error. Keep the name `PAGE_ROWS`.
2. **`IMGUI_DEFINE_MATH_OPERATORS` must precede `imgui.h`** or `ImVec2 + ImVec2`
   does not compile.
3. **ImGui 1.93 renamed the child flag** `ImGuiChildFlags_Border` →
   `ImGuiChildFlags_Borders`.
4. **Backends must stay `-fno-objc-arc`.** ARC rejects their C structs holding
   `NSCursor*`, `NSWindow*` and friends.
5. **`GCController`** — without `-framework GameController` the link fails on
   `_OBJC_CLASS_$_GCController` from `imgui_impl_osx.o`.
6. **First-frame grid crash.** `col_widths` is empty until the first frame reports
   a width; `grid()` seeds `last_window_w` before calling
   `reset_column_widths()`. Do not index `col_widths` before that.
7. **A scroll request needs a retry.** `SetScrollY()` sets a target that is
   clamped against the *previous* content size, so `pending_scroll` is only
   cleared once `GetScrollY()` is within 1 px of the target, and `draw()` keeps
   the frame loop awake while it is set.
8. **Column width budget** is `GetContentRegionAvail().x - ScrollbarSize - 1`.
   Forgetting the scrollbar width produces a permanent horizontal scrollbar.
9. **`meta->version()` returns `ParquetVersion`,** not the footer integer. Map it
   to 1 or 2 for display.
10. **No `ProjectionMask` in these headers.** Use `GetRecordBatchReader(row_groups,
    column_indices)`; `parquet::arrow::ProjectionMask` does not exist in Arrow 19.
11. **No offset/limit means whole-row-group decode.** A 1,500-row page inside a
    121,352-row group decodes the group; that decoded group is held in
    `cache_batches`, so the RSS floor is one group, not a leak.
12. **No `column_index_offset()` / `offset_index_offset()`** in these headers, so
    the chunk flags can never show `C` or `O`. Do not fabricate them.
13. **Header/body alignment** depends on `last_scroll_x` being the body's scroll
    x and on the cell x starting at `window_pos.x - scroll_x`. Any extra padding
    term shifts every column.
14. **No recurring timer.** A per-frame subscription/redraw keeps GPU buffers
    alive and burns CPU. `g_need_frame` is the only wake-up flag.
15. **A popup is not drawn on the frame it opens.** ImGui hides a freshly
    activated popup for one frame to measure it (`imgui.cpp`, "Hide popup/tooltip
    window when re-opening while we measure size"), and this loop draws one frame
    per event. Without the `popup_frames` request at the end of the loop, a
    dropdown only appears when the mouse is moved again. Keep that block.
16. **A skipped row group still has rows.** `goto_date()` skips groups through
    `rg_outside()`, so `global_idx += rg_rows[rg]` must run before `continue`;
    otherwise every skipped group shifts the reported row by its row count.
17. **Combo item lists are `\0`-separated strings.** `"1\0 2\0"` gives every item
    but the first a leading space, so the popup rows do not line up. Write
    adjacent literals: `"1\0" "2\0"`.

## Editing rules

- Keep the app in the single file `main.cpp`.
- Keep `PAGE_ROWS` at 1500 unless asked otherwise.
- Preserve the tail default and its bottom scroll.
- Format cells in `cell_text()`, not in `grid()`.
- Never add an install step: no Homebrew, CMake, Ninja, `pkg-config`, no copying
  of the pyarrow dylibs into the repo, no `.app` bundle.
- Verify with `PARQUET_SELFTEST` plus a pyarrow cross-check before reporting a
  data-layer change as done; verify the UI with `PARQUET_SELFTEST_VIEW` before
  reporting a layout change as done.

## Look & feel

Adjust only when asked:

- White background, black text, 1 px `#BDBDBD` grid lines
- toolbar buttons 0.90 grey, hover 0.84, pressed 0.77; active tab 0.80
- `.SF NS Mono` 13 pt app-wide, status text the same size, clipped
- row height 23.6, 4 px cell padding, left-aligned date/time columns
- toolbar spacing 4, 1 px rule under the bar
- drop overlay `rgba(0, 0, 0, 0.35)` with the label at 2× font scale (24 px)
