# Parquet Viewer

A native C++ desktop viewer for parquet files with millions of rows. It reads the
file footer to show the schema and the row count, then pages the data 1,500 rows at
a time. The UI is Dear ImGui drawn through Metal; the file is read with Arrow C++ /
parquet-cpp. There is no web view, no Python at runtime, and no framework to
install: `clang++` compiles one Objective-C++ file and links two dylibs that are
already on the machine.

```
./parquet-viewer ~/data/BTCUSDT-1m.parquet
```

## Features

- **Footer-only open** — size, version, rows, row groups, schema and per-chunk
  metadata come from `ParquetFileReader::metadata()`. No data page is touched
  until a page is loaded.
- **Tail default** — a file opens on its **last** page scrolled to the bottom, so
  `Prev` works immediately and `Next` is disabled at the end. The toolbar button
  reads `Show all` in that state and `Tail 1500` once you are showing all pages.
- **Pagination** — 1,500 rows per page, `Prev` / `Next`, `Page n/m` in the toolbar.
- **Virtualized grid** — only the rows inside the viewport are drawn; a single
  full-height spacer keeps the scrollbar proportional to the whole page.
- **Sticky header** — the header is drawn above the scrolling body and its
  horizontal offset is mirrored from the body, so columns stay aligned while
  scrolling sideways.
- **Resizable columns** — drag the divider at the right edge of a header cell; the
  width is split between that column and the next one, floor 48 px. Widths rescale
  proportionally when the window resizes.
- **Layout inspector** — file info, a row-group byte strip proportional to
  `total_byte_size`, the schema with physical and logical type per column,
  key-value metadata, and one collapsible entry per row group with its chunk
  table (codec, values, compressed/uncompressed size, data offset, min/max, null
  count, flags, encodings).
- **Go to Date** — day / month / year dropdowns plus `Go`. It samples the date
  column, detects epoch seconds (10 digits), milliseconds (13) or microseconds
  (16) as well as text dates, matches the **UTC day**, scans the row groups, and
  loads from one row before the match so the matched date is the second row.
- **Open** with a CLI argument, the `Open` button (`NSOpenPanel`), or by dropping a
  file on the window.
- Light theme, one-row toolbar, `.SF NS Mono` at 13 pt, 0 % CPU when idle.

## Requirements

| Item | What is needed |
|---|---|
| macOS | arm64, tested on macOS 27.0.1 |
| Compiler | Xcode Command Line Tools (`clang++`, Apple clang 21) |
| Arrow C++ / parquet-cpp | headers plus `libarrow.*.dylib` and `libparquet.*.dylib` from a **pyarrow wheel** (tested with 19.0.1) |
| Dear ImGui | vendored in `imgui/` (1.93.1 WIP, MIT) |

No `brew`, no CMake, no Ninja, no `pkg-config`, no install step. `build.sh` finds
pyarrow through `python3 -c "import pyarrow"` and links against the wheel's dylibs
by absolute path.

## Build

```bash
./build.sh                 # -> ./parquet-viewer (repo root)
PYARROW_DIR=/path/to/pyarrow ./build.sh   # if pyarrow is not on sys.path
```

`build.sh` runs four steps: the four ImGui core `.cpp` files, the two macOS
backends (`imgui_impl_osx.mm`, `imgui_impl_metal.mm`, compiled **without** ARC),
`main.cpp` as Objective-C++ **with** ARC, then the link. Object files go to
`build/`, the binary to the repo root. Clean build: **6.2–6.5 s**, binary
**1,204,528 bytes**.

The binary records the pyarrow directory as its rpath:

```
@rpath/libarrow.1900.dylib
@rpath/libparquet.1900.dylib
```

If pyarrow is moved or upgraded, rebuild with `PYARROW_DIR=...`; otherwise the
loader cannot find the two dylibs.

## Run

```bash
./parquet-viewer                            # start empty, use Open
./parquet-viewer ~/data/BTCUSDT-1m.parquet  # open directly
```

Arguments that begin with `-` are ignored and the first argument that resolves to
an existing file is opened. `~/` and `file://` prefixes are resolved.

Toolbar, one row:

```
Open | Show all / Tail 1500 | Data | Layout | Prev | Page n/m | Next | Go to Date | day | month | year | Go | status
```

- `Data` / `Layout` switch the body; the active tab is drawn darker.
- The year list runs from 2010 to the current year.
- `Date`, `Time` and `Datetime` columns are left-aligned; every other column is
  centered.
- Dropping a `.parquet` file on the window opens it; a dark overlay marks the
  drag.

## Layout tab

Everything there comes from the footer:

- File: size, format version, rows, row groups, columns, created-by.
- Row-group byte strip: one segment per row group, width proportional to
  `total_byte_size`, four-colour palette.
- `Schema (n columns)`: name, physical type, logical type.
- `Key-value metadata (n)`: writer key/value pairs.
- `Row groups (n)`: one collapsible row per group; expanded it shows a chunk table
  with column, physical type, codec, values, compressed and uncompressed size,
  data offset, min, max, null count, flags (`B` bloom, `D` dictionary page,
  `I` index page) and encodings.

## Measured

On `~/Documents/Crypto/BTCUSDT-1m.parquet` — 95,580,670 bytes, 4,854,079 rows ×
6 columns, 40 row groups of 121,352 rows, 3,237 pages:

| Metric | Value |
|---|---|
| Page load (first page) | 4.9–6.0 ms |
| Page load (tail page) | 5.4–5.5 ms |
| `Go to Date` (2024-05-01) | 12.4–12.7 ms |
| `phys_footprint` with the tail page loaded | 48 MB |
| `ps -o rss` | 110 MB |
| Idle CPU | 0.0 % |
| Window | 750 × 950 content, 750 × 982 on screen, min 700 × 500 |

Checked against pyarrow on the same file: page 81 (rows 120,000–121,499, which
spans two row groups) matches cell for cell; row group 0 chunk 0 reports
121,352 values, 153,543 compressed / 970,923 uncompressed bytes,
min 1502942400, max 1510248660, nulls 0, `PLAIN,RLE`; `Go to Date` 2024-05-01 lands
on row 3,517,759. On `BTCUSDT-1D.parquet` (3,377 rows, text dates) the same date
lands on row 2,449.

## Known limits

- **A page read decodes its whole row group.** Arrow C++ 19 has no
  `with_offset` / `with_limit` and no `parquet::arrow::ProjectionMask`, so
  `read_range()` opens a `GetRecordBatchReader` for the row groups that overlap the
  page, then slices. The last decoded group is kept, so the next page inside the
  same group costs 0.3 ms instead of 5.2 ms; that held group is where the RSS
  floor comes from.
- **`C` / `O` flags never appear** in the chunk table: the shipped headers expose
  `bloom_filter_offset()` but not column-index or offset-index offsets.
- **Bold is faked** by drawing header text twice with a sub-pixel offset, because
  one font is loaded (SF Mono Regular).
- **`Go to Date` reads the date column of the row groups that can contain the
  date.** Groups whose chunk min/max does not overlap the day are skipped, and an
  integer epoch column is compared as numbers, not as strings.
- **Terminal only.** No `.app` bundle and no AppleEvent handler, so macOS
  "Open With" is not wired up; CLI argument, `Open` panel and drag-and-drop are.
- **Runtime depends on the pyarrow directory** recorded as the rpath at build time.

## Project layout

```
main.cpp                     # the whole app (1,825 lines, Objective-C++)
build.sh                     # clang++ build, 4 steps, no CMake
imgui/                       # Dear ImGui 1.93.1 WIP + osx/metal backends + LICENSE
parquet-viewer               # built binary (1,204,528 bytes)
build/                       # object files, created by build.sh
```

## Diagnostics

Environment variables, all opt-in:

| Variable | Effect |
|---|---|
| `PARQUET_SELFTEST=1` | Print row/page counts, first rows, last row, layout summary, timings; renders no window |
| `PARQUET_SELFTEST_DATE=YYYY-MM-DD` | With the above: run `Go to Date` and print the result |
| `PARQUET_SELFTEST_VIEW=grid\|layout` | With the above: render one frame offscreen to a PNG |
| `PARQUET_SELFTEST_PNG=/path.png` | Output path for that PNG |
| `PARQUET_SCROLLDBG=1` | Print the grid's scroll state each frame |

```bash
PARQUET_SELFTEST=1 PARQUET_SELFTEST_DATE=2024-05-01 ./parquet-viewer ~/data/file.parquet
```
