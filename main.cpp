// Parquet Viewer — C++ port of src/main.rs (iced/Rust).
//
// Single translation unit, compiled as Objective-C++ so the macOS platform
// layer (NSWindow, NSOpenPanel, Finder drag-and-drop) lives next to the viewer
// logic. The parquet/footer/data work is plain C++ over Arrow C++ / parquet-cpp.
//
// Layout of the file:
//   1. constants + small helpers (formatting, dates)
//   2. parquet data layer (footer metadata, layout info, columnar reads)
//   3. App state and page/date logic        (mirrors ParquetViewerApp)
//   4. ImGui UI: toolbar, grid, layout view
//   5. Objective-C++ platform: metal view, render loop, main()

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "backends/imgui_impl_osx.h"
#include "backends/imgui_impl_metal.h"

#include <arrow/api.h>
#include <arrow/array.h>
#include <arrow/io/file.h>
#include <arrow/scalar.h>
#include <parquet/arrow/reader.h>
#include <parquet/file_reader.h>
#include <parquet/metadata.h>
#include <parquet/schema.h>
#include <parquet/statistics.h>
#include <parquet/types.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------- constants

// PAGE_ROWS clashes with the mach/vm_param.h macro pulled in by Metal.
static constexpr int64_t PAGE_ROWS = 1500;

static constexpr float UI_FONT = 13.0f;
static constexpr float GRID_ROW_H = 23.6f;
static constexpr float GRID_MIN_COL_W = 48.0f;
static constexpr float RESIZE_HANDLE_W = 5.0f;
static constexpr float DATE_FIELD_W = 38.0f;
static constexpr float YEAR_FIELD_W = 56.0f;

// ------------------------------------------------------------------ helpers

static std::string fmt_size(int64_t n) {
  if (n < 0) n = 0;
  static const char* U[] = {"B", "KB", "MB", "GB"};
  double v = (double)n;
  int u = 0;
  while (v >= 1024.0 && u + 1 < 4) { v /= 1024.0; ++u; }
  char buf[64];
  if (u == 0) std::snprintf(buf, sizeof buf, "%lld B", (long long)n);
  else std::snprintf(buf, sizeof buf, "%.1f %s", v, U[u]);
  return buf;
}

static std::string short_str(const std::string& s, size_t max) {
  if (s.size() <= max) return s;
  // cut on a utf-8 boundary
  size_t cut = max;
  while (cut > 0 && (s[cut] & 0xC0) == 0x80) --cut;
  return s.substr(0, cut) + "\xE2\x80\xA6";  // …
}

static std::string ba_to_string(const std::string& data) {
  if (data.empty()) return {};
  int printable = 0;
  for (unsigned char c : data) {
    if (c >= 0x20 || c == '\n' || c == '\t') ++printable;
  }
  if (printable * 2 >= (int)data.size()) return short_str(data, 32);
  std::string hex = "0x";
  char b[3];
  for (size_t i = 0; i < data.size() && i < 16; ++i) {
    std::snprintf(b, sizeof b, "%02x", (unsigned char)data[i]);
    hex += b;
  }
  return hex;
}

static std::string i64_to_str(int64_t v) {
  char buf[32];
  auto r = std::to_chars(buf, buf + sizeof buf, v);
  return std::string(buf, r.ptr);
}

static std::string f64_to_str(double v) {
  if (!std::isfinite(v)) {
    if (std::isnan(v)) return "NaN";
    return v > 0 ? "inf" : "-inf";
  }
  char buf[64];
  auto r = std::to_chars(buf, buf + sizeof buf, v);
  return std::string(buf, r.ptr);
}

// Howard Hinnant's civil_from_days.
static void civil_from_days(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int64_t)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : -9);
  y += (m <= 2);
}

static std::string ymd_from_days(int64_t days) {
  int64_t y;
  unsigned m, d;
  civil_from_days(days, y, m, d);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04lld-%02u-%02u", (long long)y, m, d);
  return buf;
}

static int64_t days_from_ymd(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static std::string timestamp_str(int64_t value, arrow::TimeUnit::type unit) {
  int64_t per_sec = 1, frac_digits = 0;
  switch (unit) {
    case arrow::TimeUnit::SECOND: per_sec = 1; frac_digits = 0; break;
    case arrow::TimeUnit::MILLI: per_sec = 1000; frac_digits = 3; break;
    case arrow::TimeUnit::MICRO: per_sec = 1000000; frac_digits = 6; break;
    case arrow::TimeUnit::NANO: per_sec = 1000000000; frac_digits = 9; break;
    default: per_sec = 1; break;
  }
  int64_t secs = value / per_sec;
  int64_t rem = value % per_sec;
  if (rem < 0) { rem += per_sec; secs -= 1; }
  int64_t days = secs >= 0 ? secs / 86400 : (secs - 86399) / 86400;
  int64_t tod = secs - days * 86400;
  int64_t y;
  unsigned m, d;
  civil_from_days(days, y, m, d);
  char buf[64];
  std::snprintf(buf, sizeof buf, "%04lld-%02u-%02uT%02lld:%02lld:%02lld",
                (long long)y, m, d, (long long)(tod / 3600),
                (long long)((tod % 3600) / 60), (long long)(tod % 60));
  std::string out = buf;
  if (frac_digits > 0 && rem != 0) {
    char frac[32];
    std::snprintf(frac, sizeof frac, "%0*lld", (int)frac_digits, (long long)rem);
    std::string f = frac;
    while (!f.empty() && f.back() == '0') f.pop_back();
    if (!f.empty()) out += "." + f;
  }
  return out;
}

// One cell of one arrow array, as the grid shows it.
static std::string cell_text(const arrow::Array& a, int64_t i) {
  if (a.IsNull(i)) return {};
  switch (a.type_id()) {
    case arrow::Type::STRING:
      return std::string(static_cast<const arrow::StringArray&>(a).GetView(i));
    case arrow::Type::LARGE_STRING:
      return std::string(static_cast<const arrow::LargeStringArray&>(a).GetView(i));
    case arrow::Type::INT32:
      return i64_to_str(static_cast<const arrow::Int32Array&>(a).Value(i));
    case arrow::Type::INT64:
      return i64_to_str(static_cast<const arrow::Int64Array&>(a).Value(i));
    case arrow::Type::FLOAT:
      return f64_to_str((double)static_cast<const arrow::FloatArray&>(a).Value(i));
    case arrow::Type::DOUBLE:
      return f64_to_str(static_cast<const arrow::DoubleArray&>(a).Value(i));
    case arrow::Type::BOOL:
      return static_cast<const arrow::BooleanArray&>(a).Value(i) ? "true" : "false";
    case arrow::Type::DATE32:
      return ymd_from_days(static_cast<const arrow::Date32Array&>(a).Value(i));
    case arrow::Type::TIMESTAMP: {
      const auto& ta = static_cast<const arrow::TimestampArray&>(a);
      auto ts_type = std::static_pointer_cast<arrow::TimestampType>(ta.type());
      return timestamp_str(ta.Value(i), ts_type->unit());
    }
    default: {
      auto sc = a.GetScalar(i);
      if (sc.ok()) return sc.ValueOrDie()->ToString();
      return {};
    }
  }
}

static const char* codec_name(parquet::Compression::type c) {
  using C = parquet::Compression;
  switch (c) {
    case C::UNCOMPRESSED: return "UNCOMPRESSED";
    case C::SNAPPY: return "SNAPPY";
    case C::GZIP: return "GZIP";
    case C::BROTLI: return "BROTLI";
    case C::ZSTD: return "ZSTD";
    case C::LZ4: return "LZ4";
    case C::LZ4_FRAME: return "LZ4_FRAME";
    case C::LZO: return "LZO";
    case C::BZ2: return "BZ2";
    case C::LZ4_HADOOP: return "LZ4_HADOOP";
  }
  return "UNKNOWN";
}

static std::string phys_name(parquet::Type::type t) {
  switch (t) {
    case parquet::Type::BOOLEAN: return "BOOLEAN";
    case parquet::Type::INT32: return "INT32";
    case parquet::Type::INT64: return "INT64";
    case parquet::Type::INT96: return "INT96";
    case parquet::Type::FLOAT: return "FLOAT";
    case parquet::Type::DOUBLE: return "DOUBLE";
    case parquet::Type::BYTE_ARRAY: return "BYTE_ARRAY";
    case parquet::Type::FIXED_LEN_BYTE_ARRAY: return "FIXED_LEN_BYTE_ARRAY";
    case parquet::Type::UNDEFINED: break;
  }
  return "UNDEFINED";
}

// ------------------------------------------------------- parquet data layer

struct LayoutChunk {
  std::string name, phys, codec, encodings;
  int64_t num_values = 0, compressed = 0, uncompressed = 0;
  int64_t data_offset = 0;
  bool has_dict = false, has_index_page = false;
  std::string stat_min, stat_max;
  int64_t null_count = -1;
  bool has_bloom = false, has_col_index = false, has_offset_index = false;
};

struct LayoutRG {
  int idx = 0;
  int64_t num_rows = 0, total_bytes = 0, compressed_bytes = 0;
  std::vector<LayoutChunk> chunks;
};

struct SchemaCol {
  std::string name, phys, logical;
};

struct LayoutInfo {
  int64_t file_size = 0;
  int32_t version = 0;
  int64_t num_rows = 0;
  int num_rgs = 0, num_cols = 0;
  std::string created_by;
  std::vector<std::pair<std::string, std::string>> kv;
  std::vector<SchemaCol> schema_cols;
  std::vector<LayoutRG> row_groups;
};

using parquet::TypedStatistics;

// min/max/null-count of one chunk, formatted like the Rust stat_strings().
static void stat_strings(const std::shared_ptr<parquet::Statistics>& st,
                         std::string& mn, std::string& mx, int64_t& nulls) {
  mn = "\xE2\x80\x94";  // —
  mx = "\xE2\x80\x94";
  nulls = -1;
  if (!st) return;
  if (st->HasNullCount()) nulls = st->null_count();
  switch (st->physical_type()) {
    case parquet::Type::BOOLEAN: {
      auto* s = static_cast<const parquet::BoolStatistics*>(st.get());
      if (s->HasMinMax()) { mn = s->min() ? "true" : "false"; mx = s->max() ? "true" : "false"; }
      break;
    }
    case parquet::Type::INT32: {
      auto* s = static_cast<const parquet::Int32Statistics*>(st.get());
      if (s->HasMinMax()) { mn = i64_to_str(s->min()); mx = i64_to_str(s->max()); }
      break;
    }
    case parquet::Type::INT64: {
      auto* s = static_cast<const parquet::Int64Statistics*>(st.get());
      if (s->HasMinMax()) { mn = i64_to_str(s->min()); mx = i64_to_str(s->max()); }
      break;
    }
    case parquet::Type::FLOAT: {
      auto* s = static_cast<const parquet::FloatStatistics*>(st.get());
      if (s->HasMinMax()) { mn = f64_to_str((double)s->min()); mx = f64_to_str((double)s->max()); }
      break;
    }
    case parquet::Type::DOUBLE: {
      auto* s = static_cast<const parquet::DoubleStatistics*>(st.get());
      if (s->HasMinMax()) { mn = f64_to_str(s->min()); mx = f64_to_str(s->max()); }
      break;
    }
    case parquet::Type::BYTE_ARRAY: {
      auto* s = static_cast<const parquet::ByteArrayStatistics*>(st.get());
      if (s->HasMinMax()) {
        auto& a = s->min();
        auto& b = s->max();
        mn = ba_to_string(std::string((const char*)a.ptr, a.len));
        mx = ba_to_string(std::string((const char*)b.ptr, b.len));
      }
      break;
    }
    case parquet::Type::FIXED_LEN_BYTE_ARRAY: {
      auto* s = static_cast<const parquet::FLBAStatistics*>(st.get());
      if (s->HasMinMax()) {
        auto* a = s->min().ptr;
        auto* b = s->max().ptr;
        auto len = st->descr()->type_length();
        mn = ba_to_string(std::string((const char*)a, len));
        mx = ba_to_string(std::string((const char*)b, len));
      }
      break;
    }
    default:
      break;
  }
}

static bool build_layout(const std::string& path,
                         const std::shared_ptr<parquet::FileMetaData>& meta,
                         LayoutInfo& out, std::string& err) {
  std::error_code ec;
  auto sz = std::filesystem::file_size(path, ec);
  out.file_size = ec ? 0 : (int64_t)sz;
  out.version = meta->version() == parquet::ParquetVersion::PARQUET_1_0 ? 1 : 2;
  out.num_rows = meta->num_rows();
  out.created_by = meta->created_by();
  out.kv.clear();
  if (auto kv = meta->key_value_metadata()) {
    for (int i = 0; i < kv->size(); ++i)
      out.kv.emplace_back(kv->key(i), kv->value(i));
  }
  const auto* descr = meta->schema();
  out.schema_cols.clear();
  for (int i = 0; i < descr->num_columns(); ++i) {
    const auto* c = descr->Column(i);
    std::string logical;
    if (c->logical_type() && !c->logical_type()->is_none())
      logical = c->logical_type()->ToString();
    else if (c->converted_type() != parquet::ConvertedType::NONE)
      logical = parquet::ConvertedTypeToString(c->converted_type());
    else
      logical = "NONE";
    out.schema_cols.push_back({c->path()->ToDotString(), phys_name(c->physical_type()), logical});
  }
  out.num_cols = (int)out.schema_cols.size();
  out.num_rgs = meta->num_row_groups();
  out.row_groups.clear();
  for (int i = 0; i < out.num_rgs; ++i) {
    auto rg = meta->RowGroup(i);
    LayoutRG r;
    r.idx = i;
    r.num_rows = rg->num_rows();
    r.total_bytes = rg->total_byte_size();
    r.compressed_bytes = rg->total_compressed_size();
    for (int c = 0; c < rg->num_columns(); ++c) {
      auto cc = rg->ColumnChunk(c);
      LayoutChunk ch;
      ch.name = cc->path_in_schema()->ToDotString();
      ch.phys = phys_name(cc->type());
      ch.codec = codec_name(cc->compression());
      ch.num_values = cc->num_values();
      ch.compressed = cc->total_compressed_size();
      ch.uncompressed = cc->total_uncompressed_size();
      ch.data_offset = cc->data_page_offset();
      ch.has_dict = cc->has_dictionary_page();
      ch.has_index_page = cc->index_page_offset() > 0;
      ch.has_bloom = cc->bloom_filter_offset().has_value();
      for (auto e : cc->encodings()) {
        if (!ch.encodings.empty()) ch.encodings += ",";
        ch.encodings += parquet::EncodingToString(e);
      }
      stat_strings(cc->is_stats_set() ? cc->statistics() : nullptr, ch.stat_min,
                   ch.stat_max, ch.null_count);
      r.chunks.push_back(std::move(ch));
    }
    out.row_groups.push_back(std::move(r));
  }
  (void)err;
  return true;
}

// -------------------------------------------------------------- app state

struct App {
  std::string path;
  int64_t total_rows = 0;
  std::vector<std::string> columns;
  std::vector<int> col_indices;
  std::vector<std::string> cells;  // row-major, page_rows * columns
  int64_t page_rows = 0;
  int64_t page_start = 0;
  std::vector<int64_t> rg_first;
  std::vector<int64_t> rg_rows;

  std::vector<float> col_widths;
  int64_t current_page = 0, total_pages = 1;
  bool show_all = false, show_layout = false;
  bool schema_open = false, kv_open = false;
  std::string status = "No file loaded - choose a parquet file";
  int sel_year = 2026, sel_month = 1, sel_day = 1;
  std::set<int> expanded_rgs;
  LayoutInfo layout;
  bool has_layout = false;

  float last_scroll_x = 0.0f;
  float debug_scroll_y = 0.0f;
  float debug_max_x = 0.0f;
  float debug_max_y = 0.0f;
  float last_window_w = 0.0f;

  // scroll request: 0 none, 1 top, 2 bottom, 3 row
  int pending_scroll = 0;
  int64_t pending_row = 0;

  // A page change lands one frame late in ImGui: Begin() takes the child's
  // content size from the previous frame, so ScrollMax and the scrollbar still
  // describe the old page on the frame that draws the new one. Count frames to
  // draw so the range is corrected without waiting for another event.
  int redraw_frames = 0;

  // column resize (splits width between col and col+1, like the Rust app)
  int resize_col = -1;
  bool resize_valid = false;
  float resize_start_x = 0.0f;
  float resize_left = 0.0f, resize_right = 0.0f;

  std::unique_ptr<parquet::ParquetFileReader> footer;
  std::unique_ptr<parquet::arrow::FileReader> fred;

  // Arrow 19 has no offset/limit, so a page read decodes its whole row group.
  // Many pages fit in one group (121,352 rows / 1,500 per page), so keep the
  // last decoded group and slice the next page out of it instead of decoding
  // again. Holds ~6 MB per 121,352-row group.
  int cache_rg = -1;
  std::vector<int> cache_indices;
  std::vector<std::shared_ptr<arrow::RecordBatch>> cache_batches;

  std::string title_suffix;

  // ---- data access

  void clear_cache() {
    cache_rg = -1;
    cache_indices.clear();
    cache_batches.clear();
  }

  // Decode every batch of one row group into cache_batches. Returns false and
  // leaves the cache empty when the group cannot be read.
  bool fill_cache(int rg, const std::vector<int>& indices) {
    clear_cache();
    auto res = fred->GetRecordBatchReader({rg}, indices);
    if (!res.ok()) return false;
    auto rbr = std::move(res).ValueOrDie();
    std::shared_ptr<arrow::RecordBatch> batch;
    while (true) {
      auto st = rbr->ReadNext(&batch);
      if (!st.ok() || !batch) break;
      cache_batches.push_back(batch);
    }
    if (cache_batches.empty()) return false;
    cache_rg = rg;
    cache_indices = indices;
    return true;
  }

  bool read_range(int64_t offset, int64_t length, const std::vector<int>& indices,
                  std::vector<std::string>& out) {
    out.assign((size_t)(length * (int64_t)indices.size()), std::string());
    if (length <= 0 || indices.empty() || !fred) return true;
    int rg = (int)(std::upper_bound(rg_first.begin(), rg_first.end(), offset) -
                   rg_first.begin()) - 1;
    if (rg < 0) rg = 0;
    int64_t got = 0;
    for (; rg < (int)rg_first.size() && got < length; ++rg) {
      if (rg != cache_rg || cache_indices != indices) {
        if (!fill_cache(rg, indices)) return false;
      }
      int64_t row_in_rg = 0;
      for (const auto& batch : cache_batches) {
        int64_t nb = batch->num_rows();
        for (int64_t r = 0; r < nb; ++r) {
          int64_t g = rg_first[rg] + row_in_rg + r;
          if (g < offset) continue;
          if (g >= offset + length) break;
          for (int c = 0; c < (int)indices.size(); ++c)
            out[(size_t)((g - offset) * (int64_t)indices.size() + c)] =
                cell_text(*batch->column(c), r);
          got = g - offset + 1;
        }
        row_in_rg += nb;
        if (got >= length) break;
      }
    }
    return got == length;
  }

  void reset_column_widths() {
    int n = (int)columns.size();
    col_widths.clear();
    if (n == 0 || last_window_w <= 0) return;
    float separators = (float)(n - 1);
    float total = std::max(last_window_w - separators, 320.0f);
    float w = std::max(total / (float)n, GRID_MIN_COL_W);
    col_widths.assign(n, w);
  }

  void update_total_pages() {
    total_pages = total_rows == 0 ? 1 : (total_rows + PAGE_ROWS - 1) / PAGE_ROWS;
    if (current_page >= total_pages) current_page = total_pages - 1;
  }

  void load_page(int64_t page) {
    if (path.empty() || columns.empty()) return;
    if (page >= total_pages) page = total_pages - 1;
    if (page < 0) page = 0;
    current_page = page;
    int64_t start = page * PAGE_ROWS;
    int64_t len = std::min(PAGE_ROWS, total_rows - start);
    if (len <= 0) {
      cells.clear();
      page_rows = 0;
      return;
    }
    if (!read_range(start, len, col_indices, cells)) {
      status = "Failed to load page";
      cells.clear();
      page_rows = 0;
      return;
    }
    page_start = start;
    page_rows = len;
    if (!show_all && page == total_pages - 1)
      pending_scroll = 2;  // bottom
    else
      pending_scroll = 1;  // top
    redraw_frames = 2;
  }

  void load_at(int64_t start) {
    if (path.empty() || columns.empty()) return;
    if (start < 0) start = 0;
    if (start >= total_rows) start = total_rows - 1;
    int64_t len = std::min(PAGE_ROWS, total_rows - start);
    if (!read_range(start, len, col_indices, cells)) {
      status = "Failed to load window";
      cells.clear();
      page_rows = 0;
      return;
    }
    page_start = start;
    page_rows = len;
    current_page = start / PAGE_ROWS;
    pending_scroll = 3;
    pending_row = 1;
    redraw_frames = 2;
  }

  bool open_path(const std::string& p) {
    auto file_res = arrow::io::ReadableFile::Open(p);
    if (!file_res.ok()) {
      status = "Failed to open: " + file_res.status().ToString();
      return false;
    }
    auto infile = file_res.ValueOrDie();
    auto footer_res = parquet::ParquetFileReader::Open(infile);
    if (!footer_res) {  // unique_ptr, may be null on error
    }
    try {
      footer = parquet::ParquetFileReader::Open(infile);
    } catch (const std::exception& e) {
      status = std::string("Failed to read footer: ") + e.what();
      return false;
    }
    auto meta = footer->metadata();

    auto fred_res = parquet::arrow::OpenFile(infile, arrow::default_memory_pool());
    if (!fred_res.ok()) {
      status = "Failed to open arrow reader: " + fred_res.status().ToString();
      return false;
    }
    fred = std::move(fred_res).ValueOrDie();
    clear_cache();  // batches belong to the previous file

    path = p;
    total_rows = meta->num_rows();
    columns.clear();
    col_indices.clear();
    rg_first.clear();
    rg_rows.clear();
    expanded_rgs.clear();
    const auto* descr = meta->schema();
    for (int i = 0; i < descr->num_columns(); ++i) {
      columns.push_back(descr->Column(i)->path()->ToDotString());
      col_indices.push_back(i);
    }
    int64_t acc = 0;
    for (int i = 0; i < meta->num_row_groups(); ++i) {
      rg_first.push_back(acc);
      int64_t r = meta->RowGroup(i)->num_rows();
      rg_rows.push_back(r);
      acc += r;
    }
    reset_column_widths();
    update_total_pages();
    has_layout = build_layout(path, meta, layout, status);
    if (!has_layout) layout = LayoutInfo();
    status = std::to_string(total_rows) + " rows x " + std::to_string(columns.size()) + " cols";
    if (show_all)
      load_page(0);
    else
      load_page(total_pages - 1);
    title_suffix = path + "  \xE2\x80\x94  " + std::to_string(total_rows) + " rows \xC3\x97 " +
                   std::to_string(columns.size()) + " cols";
    return true;
  }

  std::string date_column() const {
    for (const auto& c : columns) {
      std::string l = c;
      std::transform(l.begin(), l.end(), l.begin(), ::tolower);
      if (l.find("date") != std::string::npos) return c;
    }
    return columns.empty() ? std::string() : columns.front();
  }

  // Exact for INT32/INT64 chunk statistics: when a group's min/max does not
  // overlap [lo, hi), no row inside it can hold the date, so the group can be
  // skipped without reading it.
  bool rg_outside(int rg, int col, int64_t lo, int64_t hi) {
    if (!footer) return false;
    auto meta = footer->metadata();
    if (rg < 0 || rg >= meta->num_row_groups()) return false;
    auto chunk = meta->RowGroup(rg)->ColumnChunk(col);
    if (!chunk || !chunk->is_stats_set()) return false;
    auto st = chunk->statistics();
    if (!st || !st->HasMinMax()) return false;
    if (st->physical_type() == parquet::Type::INT32) {
      auto* s = static_cast<const parquet::Int32Statistics*>(st.get());
      return (int64_t)s->max() < lo || (int64_t)s->min() >= hi;
    }
    if (st->physical_type() == parquet::Type::INT64) {
      auto* s = static_cast<const parquet::Int64Statistics*>(st.get());
      return s->max() < lo || s->min() >= hi;
    }
    return false;
  }

  void goto_date(const std::string& date_str) {
    if (path.empty()) { status = "No file loaded"; return; }
    if (date_str.empty()) { status = "Empty date"; return; }
    std::string date_col = date_column();
    if (date_col.empty()) { status = "No columns"; return; }
    int date_idx = 0;
    for (int i = 0; i < (int)columns.size(); ++i)
      if (columns[i] == date_col) { date_idx = i; break; }

    // sample 5 rows to detect the date format
    std::vector<std::string> sample;
    read_range(0, std::min<int64_t>(5, total_rows), {date_idx}, sample);
    bool is_epoch = !sample.empty();
    int64_t mult = 1;
    bool mult_set = false;
    for (const auto& vs : sample) {
      if (vs.empty() || !std::all_of(vs.begin(), vs.end(), [](unsigned char c) { return ::isdigit(c); })) {
        is_epoch = false;
        break;
      }
      int64_t m = vs.size() == 10 ? 1 : vs.size() == 13 ? 1000 : vs.size() == 16 ? 1000000 : 0;
      if (m == 0) { is_epoch = false; break; }
      if (mult_set && m != mult) { is_epoch = false; break; }
      mult = m;
      mult_set = true;
    }

    // UTC day bounds in the column's unit
    bool have_bounds = false;
    int64_t lo = 0, hi = 0;
    if (is_epoch) {
      int y = 0, mo = 0, d = 0;
      if (std::sscanf(date_str.c_str(), "%d-%d-%d", &y, &mo, &d) == 3) {
        int64_t start = days_from_ymd(y, (unsigned)mo, (unsigned)d) * 86400;
        int64_t end = start + 86400;
        lo = start * mult;
        hi = end * mult;
        have_bounds = true;
      }
    }

    // text variants
    std::vector<std::string> variants;
    auto add = [&](const std::string& v) {
      if (!v.empty() && std::find(variants.begin(), variants.end(), v) == variants.end())
        variants.push_back(v);
    };
    add(date_str);
    auto split = [](const std::string& s, char sep) {
      std::vector<std::string> p;
      std::string cur;
      for (char c : s) {
        if (c == sep) { p.push_back(cur); cur.clear(); }
        else cur += c;
      }
      p.push_back(cur);
      return p;
    };
    auto digits = [](const std::string& s, size_t from, size_t n) {
      if (s.size() < from + n) return false;
      for (size_t i = from; i < from + n; ++i)
        if (!::isdigit((unsigned char)s[i])) return false;
      return true;
    };
    if (date_str.size() >= 10 && digits(date_str, 0, 4) && date_str[4] == '-' && date_str[7] == '-') {
      auto p = split(date_str, '-');
      if (p.size() == 3) {
        add(p[0] + "-" + p[1] + "-" + p[2]);
        add(p[2] + "-" + p[1] + "-" + p[0]);
        add(p[2] + "/" + p[1] + "/" + p[0]);
        add(p[0] + "/" + p[1] + "/" + p[2]);
      }
    } else if (date_str.size() >= 10 && digits(date_str, 0, 4) && date_str[4] == '/') {
      auto p = split(date_str, '/');
      if (p.size() == 3) {
        add(p[2] + "-" + p[1] + "-" + p[0]);
        add(p[2] + "/" + p[1] + "/" + p[0]);
        add(p[0] + "-" + p[1] + "-" + p[2]);
      }
    } else if (date_str.size() >= 10 && digits(date_str, 0, 2) && date_str[2] == '-') {
      auto p = split(date_str, '-');
      if (p.size() == 3) {
        add(p[2] + "-" + p[1] + "-" + p[0]);
        add(p[0] + "/" + p[1] + "/" + p[2]);
        add(p[2] + "/" + p[1] + "/" + p[0]);
      }
    } else if (date_str.size() >= 10 && digits(date_str, 0, 2) && date_str[2] == '/') {
      auto p = split(date_str, '/');
      if (p.size() == 3) {
        add(p[2] + "-" + p[1] + "-" + p[0]);
        add(p[2] + "-" + p[1] + "-" + p[0]);
        add(p[0] + "/" + p[1] + "/" + p[2]);
      }
    }

    // scan row groups, one date column at a time
    // An integer epoch column compares as numbers: no std::string per row, and
    // chunk statistics can skip groups that cannot contain the date.
    bool numeric = false;
    if (is_epoch && have_bounds && footer) {
      auto phys = footer->metadata()->schema()->Column(date_idx)->physical_type();
      numeric = (phys == parquet::Type::INT64 || phys == parquet::Type::INT32);
    }
    int64_t found = -1;
    int64_t global_idx = 0;
    for (int rg = 0; rg < (int)rg_first.size() && found < 0; ++rg) {
      if (numeric && rg_outside(rg, date_idx, lo, hi)) {
        global_idx += rg_rows[rg];  // skipped rows still count
        continue;
      }
      auto res = fred->GetRecordBatchReader({rg}, {date_idx});
      if (!res.ok()) continue;
      auto rbr = std::move(res).ValueOrDie();
      std::shared_ptr<arrow::RecordBatch> batch;
      while (found < 0 && rbr->ReadNext(&batch).ok() && batch) {
        const arrow::Array& arr = *batch->column(0);
        if (numeric && arr.type_id() == arrow::Type::INT64) {
          const auto& a = static_cast<const arrow::Int64Array&>(arr);
          for (int64_t r = 0; r < a.length(); ++r) {
            if (!a.IsNull(r)) {
              int64_t n = a.Value(r);
              if (n >= lo && n < hi) { found = global_idx; break; }
            }
            ++global_idx;
          }
        } else if (numeric && arr.type_id() == arrow::Type::INT32) {
          const auto& a = static_cast<const arrow::Int32Array&>(arr);
          for (int64_t r = 0; r < a.length(); ++r) {
            if (!a.IsNull(r)) {
              int64_t n = (int64_t)a.Value(r);
              if (n >= lo && n < hi) { found = global_idx; break; }
            }
            ++global_idx;
          }
        } else {
          for (int64_t r = 0; r < batch->num_rows(); ++r) {
            std::string val = cell_text(arr, r);
            bool hit = false;
            if (have_bounds) {
              char* endp = nullptr;
              long long n = std::strtoll(val.c_str(), &endp, 10);
              if (endp && endp != val.c_str() && *endp == '\0')
                hit = ((int64_t)n >= lo && (int64_t)n < hi);
            } else {
              for (const auto& v : variants) {
                if (val == v || val.find(v) != std::string::npos ||
                    (val.size() >= 10 && v.size() >= 10 && val.compare(0, 10, v, 0, 10) == 0)) {
                  hit = true;
                  break;
                }
              }
            }
            if (hit) { found = global_idx; break; }
            ++global_idx;
          }
        }
      }
    }

    if (found >= 0) {
      load_at(found - 1);
      status = "Found at row " + std::to_string(found) + " page " +
               std::to_string(found / PAGE_ROWS + 1);
    } else if (have_bounds) {
      status = "Date '" + date_str + "' not found in '" + date_col + "' (epoch " +
               std::to_string(lo) + ".." + std::to_string(hi) + ")";
    } else {
      std::string tried;
      for (size_t i = 0; i < variants.size(); ++i) {
        if (i) tried += ", ";
        tried += variants[i];
      }
      status = "Date '" + date_str + "' not found in '" + date_col + "' (tried " + tried + ")";
    }
  }
};

// --------------------------------------------------------------------- UI

static const char* kParquetDocFontPath = "/System/Library/Fonts/SFNSMono.ttf";

static std::string g_pending_open;      // drag & drop / CLI
static bool g_hovering = false;         // file hovering the window
static bool g_need_frame = true;
static bool g_quit = false;
static NSString* g_window_title = nil;

static ImU32 col_black() { return IM_COL32(0, 0, 0, 255); }
static ImU32 col_line() { return IM_COL32(189, 189, 189, 255); }
static ImU32 col_white() { return IM_COL32(255, 255, 255, 255); }

static bool is_date_col(const std::string& name) {
  std::string n = name;
  std::transform(n.begin(), n.end(), n.begin(), ::tolower);
  return n == "date" || n == "time" || n == "datetime";
}

// text in a rect, vertically centred, 4px horizontal padding, optional fake bold
static void draw_cell_text(ImDrawList* dl, ImVec2 rmin, ImVec2 rmax, const std::string& text,
                           bool bold, bool left_align) {
  if (text.empty()) return;
  ImVec2 ts = ImGui::CalcTextSize(text.c_str());
  float avail = (rmax.x - rmin.x) - 8.0f;
  std::string shown = text;
  if (ts.x > avail && avail > 8.0f) {
    // trim until it fits
    while (!shown.empty()) {
      shown.pop_back();
      ts = ImGui::CalcTextSize((shown + "\xE2\x80\xA6").c_str());
      if (ts.x <= avail) { shown += "\xE2\x80\xA6"; break; }
    }
  }
  ts = ImGui::CalcTextSize(shown.c_str());
  float x = left_align ? rmin.x + 4.0f : rmin.x + (rmax.x - rmin.x - ts.x) * 0.5f;
  float y = rmin.y + (rmax.y - rmin.y - ts.y) * 0.5f;
  dl->PushClipRect(rmin, rmax, true);
  dl->AddText(ImVec2(x, y), col_black(), shown.c_str());
  if (bold) dl->AddText(ImVec2(x + 0.7f, y), col_black(), shown.c_str());
  dl->PopClipRect();
}

struct UI {
  App& app;
  NSWindow* win = nil;

  explicit UI(App& a) : app(a) {}

  void toolbar() {
    const float bar_h = ImGui::GetFrameHeight() + 4.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));

    if (ImGui::Button("Open")) {
      NSOpenPanel* panel = [NSOpenPanel openPanel];
      [panel setCanChooseFiles:YES];
      [panel setCanChooseDirectories:NO];
      [panel setAllowsMultipleSelection:NO];
      [panel setAllowedFileTypes:@[ @"parquet" ]];
      if ([panel runModal] == NSModalResponseOK) {
        NSURL* url = [[panel URLs] firstObject];
        if (url) g_pending_open = [[url path] UTF8String];
      }
    }
    ImGui::SameLine();
    ImGui::Button(app.show_all ? "Tail 1500" : "Show all");
    if (ImGui::IsItemClicked()) {
      app.show_all = !app.show_all;
      app.load_page(app.show_all ? 0 : app.total_pages - 1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Data")) { app.show_layout = false; }
    ImGui::SameLine();
    if (ImGui::Button("Layout")) { app.show_layout = true; }

    bool can_prev = app.total_pages > 1 && app.current_page > 0;
    ImGui::SameLine();
    if (ImGui::Button("Prev") && can_prev) app.load_page(app.current_page - 1);
    ImGui::SameLine();
    ImGui::Text("Page %lld/%lld", (long long)(app.current_page + 1), (long long)app.total_pages);
    bool can_next = app.total_pages > 1 && app.current_page + 1 < app.total_pages;
    ImGui::SameLine();
    if (ImGui::Button("Next") && can_next) app.load_page(app.current_page + 1);

    ImGui::SameLine();
    ImGui::TextUnformatted("Go to Date");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(DATE_FIELD_W);
    static char day_items[] = "1\0" "2\0" "3\0" "4\0" "5\0" "6\0" "7\0" "8\0" "9\0" "10\0"
                              "11\0" "12\0" "13\0" "14\0" "15\0" "16\0" "17\0" "18\0" "19\0"
                              "20\0" "21\0" "22\0" "23\0" "24\0" "25\0" "26\0" "27\0" "28\0"
                              "29\0" "30\0" "31\0";
    int day_idx = app.sel_day - 1;
    if (ImGui::Combo("##day", &day_idx, day_items, 31)) app.sel_day = day_idx + 1;

    ImGui::SameLine();
    ImGui::SetNextItemWidth(DATE_FIELD_W);
    static char month_items[] = "1\0" "2\0" "3\0" "4\0" "5\0" "6\0" "7\0" "8\0" "9\0" "10\0"
                                "11\0" "12\0";
    int month_idx = app.sel_month - 1;
    if (ImGui::Combo("##month", &month_idx, month_items, 12)) app.sel_month = month_idx + 1;

    ImGui::SameLine();
    ImGui::SetNextItemWidth(YEAR_FIELD_W);
    static std::string year_items_buf;
    static std::vector<const char*> year_ptrs;
    if (year_ptrs.empty()) {
      time_t now = time(nullptr);
      struct tm tmv;
      localtime_r(&now, &tmv);
      int max_year = tmv.tm_year + 1900;
      for (int y = 2010; y <= max_year; ++y) {
        year_items_buf += std::to_string(y);
        year_items_buf.push_back('\0');
      }
      size_t pos = 0;
      while (pos < year_items_buf.size()) {
        year_ptrs.push_back(year_items_buf.c_str() + pos);
        pos += std::strlen(year_items_buf.c_str() + pos) + 1;
      }
    }
    int year_idx = app.sel_year - 2010;
    if (year_idx < 0) year_idx = 0;
    if (year_idx >= (int)year_ptrs.size()) year_idx = (int)year_ptrs.size() - 1;
    if (ImGui::Combo("##year", &year_idx, year_ptrs.data(), (int)year_ptrs.size()))
      app.sel_year = 2010 + year_idx;

    ImGui::SameLine();
    if (ImGui::Button("Go")) {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", app.sel_year, app.sel_month, app.sel_day);
      app.goto_date(buf);
    }

    // status text on the remaining width, clipped
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::PushClipRect(ImGui::GetCursorScreenPos(),
                        ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                               ImGui::GetCursorScreenPos().y + bar_h),
                        true);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
    ImGui::TextUnformatted(app.status.c_str());
    ImGui::PopStyleColor();
    ImGui::PopClipRect();

    ImGui::PopStyleVar();

    // 1px rule under the toolbar
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(ImVec2(ImGui::GetWindowPos().x, p.y), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), p.y + 1.0f), col_line());
    ImGui::Dummy(ImVec2(0.0f, 1.0f));
  }

  void grid() {
    if (app.columns.empty()) {
      ImVec2 avail = ImGui::GetContentRegionAvail();
      const char* msg = "No file loaded - drag & drop parquet here";
      ImVec2 ts = ImGui::CalcTextSize(msg);
      ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + (avail.x - ts.x) * 0.5f,
                                 ImGui::GetCursorPosY() + (avail.y - ts.y) * 0.5f));
      ImGui::TextUnformatted(msg);
      return;
    }
    const int n = (int)app.columns.size();
    // Reserve the vertical scrollbar so the columns never overflow into a
    // horizontal scrollbar at the default window width.
    float win_w = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize - 1.0f;
    if (app.last_window_w <= 0.0f) app.last_window_w = win_w;
    if ((int)app.col_widths.size() != n) app.reset_column_widths();

    // window width changed: scale every column like the Rust app does
    if (app.last_window_w > 0.0f && std::fabs(win_w - app.last_window_w) > 0.5f &&
        (int)app.col_widths.size() == n) {
      float old_avail = std::max(app.last_window_w - (float)(n - 1), 1.0f);
      float new_avail = std::max(win_w - (float)(n - 1), 1.0f);
      float scale = new_avail / old_avail;
      for (auto& w : app.col_widths) w = std::max(w * scale, GRID_MIN_COL_W);
    }
    app.last_window_w = win_w;

    float total_w = (float)(n - 1);  // separators
    for (float w : app.col_widths) total_w += w;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float body_w = ImGui::GetContentRegionAvail().x;

    // ---- header (sticky, mirrored from the body's horizontal scroll)
    dl->AddRectFilled(origin, ImVec2(origin.x + body_w, origin.y + GRID_ROW_H), col_white());
    dl->PushClipRect(origin, ImVec2(origin.x + body_w, origin.y + GRID_ROW_H), true);
    float hx = origin.x - app.last_scroll_x;
    for (int i = 0; i < n; ++i) {
      float w = app.col_widths[i];
      ImVec2 rmin(hx, origin.y), rmax(hx + w, origin.y + GRID_ROW_H);
      if (rmax.x > origin.x && rmin.x < origin.x + body_w)
        draw_cell_text(dl, rmin, rmax, app.columns[i], true, is_date_col(app.columns[i]));
      hx += w;
      if (i + 1 < n) {
        dl->AddRectFilled(ImVec2(hx, origin.y), ImVec2(hx + 1.0f, origin.y + GRID_ROW_H), col_line());
        hx += 1.0f;
      }
    }
    dl->PopClipRect();
    dl->AddRectFilled(ImVec2(origin.x, origin.y + GRID_ROW_H - 1.0f),
                      ImVec2(origin.x + body_w, origin.y + GRID_ROW_H), col_line());

    // column resize handles
    {
      float cx = origin.x - app.last_scroll_x;
      for (int i = 0; i + 1 < n; ++i) {
        cx += app.col_widths[i];
        float handle_x = cx - RESIZE_HANDLE_W * 0.5f;
        if (handle_x >= origin.x && handle_x <= origin.x + body_w) {
          ImGui::SetCursorScreenPos(ImVec2(handle_x, origin.y));
          char id[24];
          std::snprintf(id, sizeof id, "##rh%d", i);
          ImGui::InvisibleButton(id, ImVec2(RESIZE_HANDLE_W, GRID_ROW_H));
          if (ImGui::IsItemActivated()) {
            app.resize_col = i;
            app.resize_valid = (i + 1 < (int)app.col_widths.size());
            app.resize_start_x = ImGui::GetIO().MousePos.x;
            app.resize_left = app.col_widths[i];
            app.resize_right = app.col_widths[i + 1];
          }
        }
        cx += 1.0f;
      }
      if (app.resize_col >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left) && app.resize_valid) {
        float total = app.resize_left + app.resize_right;
        float dx = ImGui::GetIO().MousePos.x - app.resize_start_x;
        float new_left = std::clamp(app.resize_left + dx, GRID_MIN_COL_W, total - GRID_MIN_COL_W);
        app.col_widths[app.resize_col] = new_left;
        app.col_widths[app.resize_col + 1] = total - new_left;
      }
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) app.resize_col = -1;
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + GRID_ROW_H));

    // ---- body
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1, 1, 1, 1));
    ImGui::BeginChild("##grid", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float pitch = GRID_ROW_H;
    float content_h = (float)app.page_rows * pitch;
    ImGui::Dummy(ImVec2(total_w, content_h));

    // A scroll request is retried until it sticks: on the first frame after a
    // page load the child's content size is still the previous one, so the
    // request gets clamped and has to be repeated on the next frame.
    if (app.pending_scroll) {
      float target = 0.0f;
      if (app.pending_scroll == 2) target = content_h - avail.y;
      else if (app.pending_scroll == 3) target = (float)app.pending_row * pitch;
      target = std::max(0.0f, target);
      ImGui::SetScrollY(target);
      if (app.pending_scroll != 3) ImGui::SetScrollX(0.0f);
      if (std::fabs(ImGui::GetScrollY() - target) < 1.0f) app.pending_scroll = 0;
    }

    float sx = ImGui::GetScrollX();
    float sy = ImGui::GetScrollY();
    app.last_scroll_x = sx;
    app.debug_scroll_y = sy;
    app.debug_max_x = ImGui::GetScrollMaxX();
    app.debug_max_y = ImGui::GetScrollMaxY();
    if (std::getenv("PARQUET_SCROLLDBG"))
      std::printf("grid: rows=%lld content_h=%.1f avail_h=%.1f sy=%.1f pending=%d\n",
                  (long long)app.page_rows, content_h, avail.y,
                  sy, app.pending_scroll);

    ImDrawList* bdl = ImGui::GetWindowDrawList();
    // child has no padding, so content pixel (0,0) sits at window pos - scroll
    ImVec2 wp = ImGui::GetWindowPos();
    ImVec2 body_origin(wp.x - sx, wp.y - sy);

    int64_t first = (int64_t)std::floor(sy / pitch);
    int64_t visible = (int64_t)std::ceil(avail.y / pitch) + 2;
    if (first < 0) first = 0;
    if (first > app.page_rows) first = app.page_rows;
    int64_t last = std::min(app.page_rows, first + visible);

    for (int64_t r = first; r < last; ++r) {
      float y0 = body_origin.y + (float)r * pitch;
      float cx = body_origin.x;
      if (y0 > wp.y + ImGui::GetWindowHeight()) break;
      for (int i = 0; i < n; ++i) {
        float w = app.col_widths[i];
        ImVec2 rmin(cx, y0), rmax(cx + w, y0 + pitch);
        if (rmax.x > wp.x && rmin.x < wp.x + ImGui::GetWindowWidth()) {
          const std::string& txt = app.cells[(size_t)(r * n + i)];
          draw_cell_text(bdl, rmin, rmax, txt, false, is_date_col(app.columns[i]));
        }
        cx += w;
        if (i + 1 < n) {
          if (cx > wp.x && cx < wp.x + ImGui::GetWindowWidth())
            bdl->AddRectFilled(ImVec2(cx, y0), ImVec2(cx + 1.0f, y0 + pitch), col_line());
          cx += 1.0f;
        }
      }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
  }

  void layout_view() {
    if (!app.has_layout) {
      ImVec2 avail = ImGui::GetContentRegionAvail();
      const char* msg = "Layout unavailable for this file.";
      ImVec2 ts = ImGui::CalcTextSize(msg);
      ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + (avail.x - ts.x) * 0.5f,
                                 ImGui::GetCursorPosY() + (avail.y - ts.y) * 0.5f));
      ImGui::TextUnformatted(msg);
      return;
    }
    const LayoutInfo& l = app.layout;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##layout", ImVec2(0, 0), ImGuiChildFlags_None);
    ImGui::Dummy(ImVec2(0, 2));

    ImGui::SetWindowFontScale(1.25f);
    ImGui::TextUnformatted("File");
    ImGui::SetWindowFontScale(1.0f);

    auto info_row = [](const char* label, const std::string& value) {
      ImGui::TextUnformatted(label);
      ImGui::SameLine(110.0f);
      ImGui::TextUnformatted(value.c_str());
    };
    info_row("Size:", fmt_size(l.file_size));
    info_row("Version:", "v" + std::to_string(l.version));
    info_row("Rows:", std::to_string(l.num_rows));
    info_row("Row groups:", std::to_string(l.num_rgs));
    info_row("Columns:", std::to_string(l.num_cols));
    info_row("Created by:", short_str(l.created_by, 60));

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextUnformatted("Row-group byte strip (proportional to total_byte_size):");
    {
      int64_t total = 0;
      for (auto& rg : l.row_groups) total += rg.total_bytes;
      float strip_w = avail.x - 16.0f;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      ImVec2 p = ImGui::GetCursorScreenPos();
      if (total > 0 && strip_w > 0) {
        float x = p.x;
        for (size_t i = 0; i < l.row_groups.size(); ++i) {
          float portion = (float)((double)std::max<int64_t>(l.row_groups[i].total_bytes, 0) / (double)total);
          float w = std::max(strip_w * portion, 1.0f);
          static const ImU32 cols[4] = {IM_COL32(93, 165, 242, 255), IM_COL32(142, 199, 123, 255),
                                        IM_COL32(242, 178, 93, 255), IM_COL32(200, 130, 220, 255)};
          dl->AddRectFilled(ImVec2(x, p.y), ImVec2(std::min(x + w, p.x + strip_w), p.y + 16.0f),
                            cols[i % 4], 2.0f);
          x += w;
        }
      }
      ImGui::Dummy(ImVec2(strip_w, 16.0f));
      ImGui::TextUnformatted(("total " + fmt_size(total)).c_str());
    }

    ImGui::Dummy(ImVec2(0, 6));
    if (ImGui::Button((std::string(app.schema_open ? "v " : "> ") + "Schema (" +
                       std::to_string(l.schema_cols.size()) + " columns)").c_str()))
      app.schema_open = !app.schema_open;
    if (app.schema_open) {
      ImGui::TextUnformatted("Column");
      ImGui::SameLine(avail.x * 0.35f);
      ImGui::TextUnformatted("Physical");
      ImGui::SameLine(avail.x * 0.55f);
      ImGui::TextUnformatted("Logical");
      for (auto& c : l.schema_cols) {
        ImGui::TextUnformatted(c.name.c_str());
        ImGui::SameLine(avail.x * 0.35f);
        ImGui::TextUnformatted(c.phys.c_str());
        ImGui::SameLine(avail.x * 0.55f);
        ImGui::TextUnformatted(short_str(c.logical, 60).c_str());
      }
    }

    ImGui::Dummy(ImVec2(0, 6));
    if (ImGui::Button((std::string(app.kv_open ? "v " : "> ") + "Key-value metadata (" +
                       std::to_string(l.kv.size()) + ")").c_str()))
      app.kv_open = !app.kv_open;
    if (app.kv_open) {
      for (auto& [k, v] : l.kv) {
        ImGui::TextUnformatted(k.c_str());
        ImGui::SameLine(200.0f);
        ImGui::TextUnformatted(short_str(v, 80).c_str());
      }
    }

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::SetWindowFontScale(1.25f);
    ImGui::Text("Row groups (%d)", (int)l.row_groups.size());
    ImGui::SetWindowFontScale(1.0f);

    for (auto& rg : l.row_groups) {
      bool open = app.expanded_rgs.count(rg.idx) > 0;
      std::string title = std::string(open ? "v " : "> ") + "RG " + std::to_string(rg.idx) +
                          " \xE2\x80\x94 " + std::to_string(rg.num_rows) + " rows \xE2\x80\x94 " +
                          fmt_size(rg.total_bytes) + " (compressed " +
                          fmt_size(rg.compressed_bytes) + ")";
      if (ImGui::Button(title.c_str())) {
        if (open)
          app.expanded_rgs.erase(rg.idx);
        else
          app.expanded_rgs.insert(rg.idx);
      }
      if (open) {
        ImGui::BeginChild(("##rg" + std::to_string(rg.idx)).c_str(), ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        const char* heads[] = {"Column", "Phys", "Codec", "Values", "Comp", "Uncomp",
                               "DataOff", "Min", "Max", "Nulls / flags", "Encodings"};
        const float widths[] = {170, 80, 80, 70, 70, 80, 90, 100, 100, 100, 200};
        float x = 0.0f;
        for (int i = 0; i < 11; ++i) {
          ImGui::TextUnformatted(heads[i]);
          ImGui::SameLine(x += widths[i] + 6.0f);
        }
        ImGui::NewLine();
        for (auto& c : rg.chunks) {
          std::string flags;
          if (c.has_bloom) flags += "B";
          if (c.has_col_index) flags += "C";
          if (c.has_offset_index) flags += "O";
          if (c.has_dict) flags += "D";
          if (c.has_index_page) flags += "I";
          std::string nulls = c.null_count >= 0 ? std::to_string(c.null_count) : "\xE2\x80\x94";
          std::string cells_row[] = {
              short_str(c.name, 28), c.phys, c.codec, std::to_string(c.num_values),
              fmt_size(c.compressed), fmt_size(c.uncompressed), std::to_string(c.data_offset),
              short_str(c.stat_min, 18), short_str(c.stat_max, 18),
              "n=" + nulls + " " + flags, short_str(c.encodings, 24)};
          x = 0.0f;
          for (int i = 0; i < 11; ++i) {
            ImGui::TextUnformatted(cells_row[i].c_str());
            ImGui::SameLine(x += widths[i] + 6.0f);
          }
          ImGui::NewLine();
        }
        ImGui::EndChild();
      }
    }

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextUnformatted(
        "Legend: n=nulls, B=bloom, C=column index, O=offset index, D=dict page, I=index page.");
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::EndChild();
  }

  void draw() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);

    toolbar();
    if (app.show_layout)
      layout_view();
    else
      grid();

    ImGui::End();
    ImGui::PopStyleVar();

    // window title, recomputed from state (like title() in Rust)
    NSString* want = app.path.empty()
                         ? @"Parquet Viewer"
                         : [NSString stringWithFormat:@"Parquet Viewer \xE2\x80\x94 %s",
                                                      app.path.c_str()];
    if (![want isEqualToString:g_window_title]) {
      g_window_title = want;
      [win setTitle:want];
    }

    // a scroll request that needed another frame keeps the loop awake
    if (app.pending_scroll) g_need_frame = true;
    if (app.redraw_frames > 0) {
      --app.redraw_frames;
      g_need_frame = true;
    }

    // drag & drop overlay
    if (g_hovering) {
      ImDrawList* dl = ImGui::GetForegroundDrawList();
      ImVec2 p0 = vp->Pos, p1 = ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
      dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 90));
      const char* msg = "Drop parquet file";
      ImGui::SetWindowFontScale(2.0f);
      ImVec2 ts = ImGui::CalcTextSize(msg);
      dl->AddText(ImVec2(p0.x + (p1.x - p0.x - ts.x) * 0.5f, p0.y + (p1.y - p0.y - ts.y) * 0.5f),
                  IM_COL32(255, 255, 255, 255), msg);
      ImGui::SetWindowFontScale(1.0f);
    }
  }
};

// --------------------------------------------------- Objective-C++ platform

@interface ParquetView : NSView <NSDraggingDestination>
@property(nonatomic, assign) BOOL hoveringNow;
@end

@interface ParquetWindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation ParquetWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
  g_quit = YES;
  return YES;
}
- (void)windowDidResize:(NSNotification*)note {
  g_need_frame = YES;
}
- (void)windowDidChangeBackingProperties:(NSNotification*)note {
  g_need_frame = YES;
}
@end

@implementation ParquetView

- (instancetype)initWithFrame:(NSRect)frame {
  self = [super initWithFrame:frame];
  if (self) {
    self.wantsLayer = YES;
    [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
  }
  return self;
}

- (BOOL)isOpaque {
  return YES;
}

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (BOOL)wantsUpdateLayer {
  return YES;
}

- (CALayer*)makeBackingLayer {
  return [CAMetalLayer layer];
}

// Finder drop
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
  g_hovering = YES;
  g_need_frame = YES;
  return NSDragOperationCopy;
}

- (void)draggingExited:(id<NSDraggingInfo>)sender {
  g_hovering = NO;
  g_need_frame = YES;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
  g_hovering = NO;
  g_need_frame = YES;
  NSPasteboard* pb = [sender draggingPasteboard];
  NSArray<NSURL*>* urls =
      [pb readObjectsForClasses:@[ [NSURL class] ]
                        options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
  if (urls.count == 0) return NO;
  g_pending_open = urls.firstObject.path.UTF8String;
  return YES;
}

@end

static void apply_style(float font_scale) {
  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = 0.0f;
  s.ChildRounding = 0.0f;
  s.FrameRounding = 0.0f;
  s.PopupRounding = 0.0f;
  s.ScrollbarRounding = 2.0f;
  s.GrabRounding = 2.0f;
  s.WindowBorderSize = 0.0f;
  s.ChildBorderSize = 0.0f;
  s.FrameBorderSize = 0.0f;
  s.WindowPadding = ImVec2(0, 0);
  s.FramePadding = ImVec2(6, 3);
  s.ItemSpacing = ImVec2(4, 4);
  s.ScrollbarSize = 12.0f;

  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = ImVec4(0, 0, 0, 1);
  c[ImGuiCol_WindowBg] = ImVec4(1, 1, 1, 1);
  c[ImGuiCol_ChildBg] = ImVec4(1, 1, 1, 1);
  c[ImGuiCol_PopupBg] = ImVec4(0.97f, 0.97f, 0.97f, 1);
  c[ImGuiCol_Border] = ImVec4(0.74f, 0.74f, 0.74f, 1);
  c[ImGuiCol_FrameBg] = ImVec4(0.90f, 0.90f, 0.90f, 1);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.84f, 0.84f, 0.84f, 1);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.77f, 0.77f, 0.77f, 1);
  c[ImGuiCol_Button] = ImVec4(0.90f, 0.90f, 0.90f, 1);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.84f, 0.84f, 0.84f, 1);
  c[ImGuiCol_ButtonActive] = ImVec4(0.77f, 0.77f, 0.77f, 1);
  c[ImGuiCol_Header] = ImVec4(0.90f, 0.90f, 0.90f, 1);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.84f, 0.84f, 0.84f, 1);
  c[ImGuiCol_HeaderActive] = ImVec4(0.77f, 0.77f, 0.77f, 1);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0.96f, 0.96f, 0.96f, 1);
  c[ImGuiCol_ScrollbarGrab] = ImVec4(0.70f, 0.70f, 0.70f, 1);
  c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.66f, 0.66f, 0.66f, 1);
  c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.62f, 0.62f, 0.62f, 1);
  c[ImGuiCol_TextSelectedBg] = ImVec4(0.70f, 0.70f, 0.70f, 1);
  s.AntiAliasedLines = true;
  s.AntiAliasedFill = true;
  s.FontScaleMain = font_scale;
}

static std::string resolve_arg(const std::string& arg) {
  std::string p = arg;
  if (p.rfind("file://", 0) == 0) {
    // strip scheme, percent-decode the basics
    p = p.substr(7);
    std::string d;
    for (size_t i = 0; i < p.size(); ++i) {
      if (p[i] == '%' && i + 2 < p.size()) {
        d += (char)std::strtol(p.substr(i + 1, 2).c_str(), nullptr, 16);
        i += 2;
      } else {
        d += p[i];
      }
    }
    p = d;
  }
  if (p.rfind("~/", 0) == 0) {
    const char* home = std::getenv("HOME");
    if (home) p = std::string(home) + p.substr(1);
  }
  return p;
}

int main(int argc, char** argv) {
  @autoreleasepool {
    std::string initial;
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a.empty() || a[0] == '-') continue;
      std::string p = resolve_arg(a);
      std::error_code ec;
      if (std::filesystem::is_regular_file(p, ec)) {
        initial = p;
        break;
      }
    }

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp finishLaunching];

    NSRect rect = NSMakeRect(0, 0, 750, 950);
    NSWindow* win = [[NSWindow alloc]
        initWithContentRect:rect
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [win setTitle:@"Parquet Viewer"];
    [win center];
    [win setMinSize:NSMakeSize(700, 500)];
    [win setReleasedWhenClosed:NO];
    static ParquetWindowDelegate* del = nil;
    del = [[ParquetWindowDelegate alloc] init];
    [win setDelegate:del];
    [NSApp setMainMenu:({ 
      NSMenu* bar = [[NSMenu alloc] init];
      NSMenuItem* appItem = [[NSMenuItem alloc] init];
      [bar addItem:appItem];
      NSMenu* appMenu = [[NSMenu alloc] init];
      [appMenu addItemWithTitle:@"Quit Parquet Viewer" action:@selector(terminate:) keyEquivalent:@"q"];
      [appItem setSubmenu:appMenu];
      bar;
    })];

    ParquetView* view = [[ParquetView alloc] initWithFrame:rect];
    [win setContentView:view];
    [win makeFirstResponder:view];

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue = [device newCommandQueue];
    CAMetalLayer* layer = (CAMetalLayer*)view.layer;
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.opaque = YES;
    layer.contentsScale = [win backingScaleFactor];
    layer.drawableSize = CGSizeMake(rect.size.width * layer.contentsScale,
                                   rect.size.height * layer.contentsScale);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    float scale = (float)layer.contentsScale;
    if (scale < 1.0f) scale = 1.0f;
    ImFont* font = io.Fonts->AddFontFromFileTTF(kParquetDocFontPath, UI_FONT * scale);
    if (!font) io.Fonts->AddFontDefault();
    apply_style(1.0f / scale);

    ImGui_ImplOSX_Init(view);
    ImGui_ImplMetal_Init(device);

    App app;
    {
      time_t now = time(nullptr);
      struct tm tmv;
      localtime_r(&now, &tmv);
      app.sel_year = tmv.tm_year + 1900;
      app.sel_month = tmv.tm_mon + 1;
      app.sel_day = tmv.tm_mday;
    }
    UI ui(app);
    ui.win = win;
    if (!initial.empty()) app.open_path(initial);

    // Headless check of the data layer, used to verify the port from a
    // terminal: PARQUET_SELFTEST=1 ./parquet-viewer file.parquet
    if (std::getenv("PARQUET_SELFTEST")) {
      std::printf("rows=%lld cols=%d pages=%lld page=%lld show_all=%d\n",
                  (long long)app.total_rows, (int)app.columns.size(),
                  (long long)app.total_pages, (long long)(app.current_page + 1),
                  (int)app.show_all);
      std::string names;
      for (auto& c : app.columns) { names += c; names += " "; }
      std::printf("columns: %s\n", names.c_str());
      std::printf("status: %s\n", app.status.c_str());
      if (app.has_layout) {
        const LayoutInfo& l = app.layout;
        std::printf("layout: size=%lld version=v%d rgs=%d cols=%d created_by=%s kv=%d\n",
                    (long long)l.file_size, l.version, l.num_rgs, l.num_cols,
                    l.created_by.c_str(), (int)l.kv.size());
        if (!l.row_groups.empty() && !l.row_groups[0].chunks.empty()) {
          const auto& rg = l.row_groups[0];
          const auto& c = rg.chunks[0];
          std::printf("rg0: rows=%lld bytes=%lld compressed=%lld\n", (long long)rg.num_rows,
                      (long long)rg.total_bytes, (long long)rg.compressed_bytes);
          std::printf("rg0c0: %s %s %s vals=%lld comp=%lld uncomp=%lld min=%s max=%s nulls=%lld "
                      "enc=%s\n",
                      c.name.c_str(), c.phys.c_str(), c.codec.c_str(), (long long)c.num_values,
                      (long long)c.compressed, (long long)c.uncompressed, c.stat_min.c_str(),
                      c.stat_max.c_str(), (long long)c.null_count, c.encodings.c_str());
        }
        for (auto& sc : l.schema_cols)
          std::printf("schema: %s %s %s\n", sc.name.c_str(), sc.phys.c_str(), sc.logical.c_str());
      }
      int nc = (int)app.columns.size();
      int64_t show = std::min<int64_t>(app.page_rows, 3);
      for (int64_t r = 0; r < show; ++r) {
        std::printf("row+%lld:", (long long)(app.page_start + r));
        for (int c = 0; c < nc; ++c) std::printf(" %s", app.cells[(size_t)(r * nc + c)].c_str());
        std::printf("\n");
      }
      if (app.page_rows > 3) {
        int64_t r = app.page_rows - 1;
        std::printf("last row+%lld:", (long long)(app.page_start + r));
        for (int c = 0; c < nc; ++c) std::printf(" %s", app.cells[(size_t)(r * nc + c)].c_str());
        std::printf("\n");
      }
      {
        using clk = std::chrono::steady_clock;
        auto t0 = clk::now();
        app.load_page(0);
        double page_ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
        t0 = clk::now();
        app.load_page(app.total_pages - 1);
        double tail_ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
        t0 = clk::now();
        app.goto_date("2024-05-01");
        double scan_ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
        std::printf("timing: page0=%.1f ms tail=%.1f ms goto_date=%.1f ms\n", page_ms, tail_ms,
                    scan_ms);
      }
      if (const char* d = std::getenv("PARQUET_SELFTEST_DATE")) {
        app.goto_date(d);
        std::printf("goto %s: %s\n", d, app.status.c_str());
        std::printf("window start row=%lld rows=%lld\n", (long long)app.page_start,
                    (long long)app.page_rows);
        for (int64_t r = 0; r < std::min<int64_t>(app.page_rows, 2); ++r) {
          std::printf("  wrow+%lld:", (long long)(app.page_start + r));
          for (int c = 0; c < nc; ++c) std::printf(" %s", app.cells[(size_t)(r * nc + c)].c_str());
          std::printf("\n");
        }
      }
      std::fflush(stdout);

      // Optional offscreen render of one frame, so the UI can be inspected
      // from the terminal without touching the visible window:
      //   PARQUET_SELFTEST=1 PARQUET_SELFTEST_VIEW=grid|layout ...
      if (const char* v = std::getenv("PARQUET_SELFTEST_VIEW")) {
        id<MTLBuffer> rb_readback = nil;
        const int W = 750, H = 950;
        const float sc = 2.0f;
        int pw = (int)(W * sc), ph = (int)(H * sc);
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                     width:pw
                                                                                    height:ph
                                                                                 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> tex = [device newTextureWithDescriptor:td];
        MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        rpd.colorAttachments[0].texture = tex;
        rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
        rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
        rpd.colorAttachments[0].clearColor = MTLClearColorMake(1, 1, 1, 1);

        io.DisplaySize = ImVec2((float)W, (float)H);
        io.DisplayFramebufferScale = ImVec2(sc, sc);
        io.DeltaTime = 1.0f / 60.0f;
        app.show_layout = (std::strcmp(v, "layout") == 0);
        if (app.show_layout) {
          // exercise the collapsible sections in the offscreen render
          app.schema_open = app.kv_open = true;
          app.expanded_rgs.insert(0);
        }

        NSUInteger row_bytes = (NSUInteger)pw * 4;
        const int kFrames = 3;
        for (int fi = 0; fi < kFrames; ++fi) {
          ImGui_ImplMetal_NewFrame(rpd);
          ImGui::NewFrame();
          ui.draw();
          ImGui::Render();
          id<MTLCommandBuffer> cb = [queue commandBuffer];
          id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
          ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
          [enc endEncoding];
          if (fi == kFrames - 1) {
            id<MTLBuffer> rb = [device newBufferWithLength:row_bytes * (NSUInteger)ph
                                                   options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:tex
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(pw, ph, 1)
                         toBuffer:rb
                destinationOffset:0
           destinationBytesPerRow:row_bytes
         destinationBytesPerImage:row_bytes * (NSUInteger)ph];
            [blit endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            rb_readback = rb;
          } else {
            [cb commit];
            [cb waitUntilCompleted];
          }
        }
        id<MTLBuffer> rb = rb_readback;

        const unsigned char* px = (const unsigned char*)rb.contents;
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                                                       pixelsWide:pw
                                                                       pixelsHigh:ph
                                                                    bitsPerSample:8
                                                                  samplesPerPixel:4
                                                                         hasAlpha:YES
                                                                         isPlanar:NO
                                                                   colorSpaceName:NSDeviceRGBColorSpace
                                                                      bytesPerRow:row_bytes
                                                                     bitsPerPixel:32];
        unsigned char* dstpx = rep.bitmapData;
        for (int i = 0; i < pw * ph; ++i) {
          dstpx[i * 4 + 0] = px[i * 4 + 2];
          dstpx[i * 4 + 1] = px[i * 4 + 1];
          dstpx[i * 4 + 2] = px[i * 4 + 0];
          dstpx[i * 4 + 3] = 255;
        }
        NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        const char* out = std::getenv("PARQUET_SELFTEST_PNG");
        [png writeToFile:[NSString stringWithUTF8String:(out ? out : "/tmp/parquet-viewer.png")]
              atomically:YES];
        std::printf("rendered %s %dx%d verts=%d scroll=%.1f max=(%.1f,%.1f) -> %s\n", v, pw, ph,
                    ImGui::GetDrawData()->TotalVtxCount, app.debug_scroll_y, app.debug_max_x,
                    app.debug_max_y, out ? out : "/tmp/parquet-viewer.png");
        std::fflush(stdout);
      }

      ImGui_ImplMetal_Shutdown();
      ImGui_ImplOSX_Shutdown();
      ImGui::DestroyContext();
      return 0;
    }

    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    // ImGui hides a popup for one frame while it measures it (imgui.cpp:
    // "Hide popup/tooltip window when re-opening while we measure size"), and
    // this loop draws only when an event arrives. Draw a couple of frames on
    // demand so a dropdown opens on the click itself, not on the next event.
    int popup_frames = 2;

    while (!g_quit) {
      @autoreleasepool {
        if (!g_need_frame) {
          NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                          untilDate:[NSDate distantFuture]
                                             inMode:NSDefaultRunLoopMode
                                            dequeue:YES];
          if (e) [NSApp sendEvent:e];
          if ([view.window backingScaleFactor] != layer.contentsScale) {
            layer.contentsScale = [view.window backingScaleFactor];
            g_need_frame = YES;
          }
        }
        while (true) {
          NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                          untilDate:[NSDate distantPast]
                                             inMode:NSDefaultRunLoopMode
                                            dequeue:YES];
          if (!e) break;
          [NSApp sendEvent:e];
        }
        g_need_frame = NO;

        if (!g_pending_open.empty()) {
          std::string p = g_pending_open;
          g_pending_open.clear();
          app.open_path(p);
        }

        NSSize cs = view.bounds.size;
        CGFloat bs = [view.window backingScaleFactor];
        CGSize want_drawable = CGSizeMake(cs.width * bs, cs.height * bs);
        if (fabs(layer.drawableSize.width - want_drawable.width) > 0.5 ||
            fabs(layer.drawableSize.height - want_drawable.height) > 0.5) {
          layer.contentsScale = bs;
          layer.drawableSize = want_drawable;
        }

        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (!drawable) continue;

        MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        rpd.colorAttachments[0].texture = drawable.texture;
        rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
        rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
        rpd.colorAttachments[0].clearColor = MTLClearColorMake(1, 1, 1, 1);

        ImGui_ImplMetal_NewFrame(rpd);
        ImGui_ImplOSX_NewFrame(view);
        ImGui::NewFrame();
        ui.draw();
        ImGui::Render();

        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
        [enc endEncoding];
        [cb presentDrawable:drawable];
        [cb commit];

        // a frame was requested while drawing (page load, resize, hover)
        if (io.WantCaptureMouse || io.WantCaptureKeyboard) g_need_frame = false;
        // a page change needs one more frame before the child's scroll range
        // matches it, and the capture check above must not cancel that frame
        if (app.redraw_frames > 0) g_need_frame = true;

        if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
          if (popup_frames > 0) {
            --popup_frames;
            g_need_frame = true;
          }
        } else {
          popup_frames = 2;
        }
      }
    }

    ImGui_ImplMetal_Shutdown();
    ImGui_ImplOSX_Shutdown();
    ImGui::DestroyContext();
  }
  return 0;
}
