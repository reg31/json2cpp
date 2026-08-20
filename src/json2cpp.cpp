/*
MIT License

Copyright (c) 2026 Jason Turner, Regis Duflaut-Averty

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <format>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct compile_results
{
  std::vector<std::string> hpp;
  std::vector<std::string> impl;
};

namespace {

std::string sanitize_identifier(std::string_view name)
{
  static constexpr std::string_view keywords[] = {
    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break",
    "case", "catch", "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const",
    "consteval", "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield",
    "decltype", "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit", "export",
    "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable",
    "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private",
    "protected", "public", "register", "reinterpret_cast", "requires", "return", "short", "signed", "sizeof",
    "static", "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local", "throw",
    "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual", "void",
    "volatile", "wchar_t", "while", "xor", "xor_eq"
  };
  std::string result;
  result.reserve(name.size());
  for (char c : name) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      result.push_back(c);
    } else if (result.empty() || result.back() != '_') {
      result.push_back('_');
    }
  }
  if (result.empty()) return "json_doc";
  if (result[0] >= '0' && result[0] <= '9') result.insert(0, "json_");
  if (result[0] == '_') result.insert(0, "json");
  if (std::ranges::contains(keywords, result)) result += '_';
  return result;
}

std::string escape_string(const std::string &str)
{
  std::string result;
  result.reserve(str.size());
  for (const char character : str) {
    switch (character) {
    case '"': result += "\\\""; break;
    case '\\': result += "\\\\"; break;
    case '\b': result += "\\b"; break;
    case '\f': result += "\\f"; break;
    case '\n': result += "\\n"; break;
    case '\r': result += "\\r"; break;
    case '\t': result += "\\t"; break;
    default: {
      const auto byte = static_cast<unsigned char>(character);
      if (byte < 0x20u || byte == 0x7Fu) {
        result += '\\';
        result += static_cast<char>('0' + ((byte >> 6u) & 0x07u));
        result += static_cast<char>('0' + ((byte >> 3u) & 0x07u));
        result += static_cast<char>('0' + (byte & 0x07u));
      } else {
        result += character;
      }
      break;
    }
    }
  }
  return result;
}

std::string format_json_string(const std::string &str)
{
  return std::format("RAW_PREFIX(\"{}\")", escape_string(str));
}

uint32_t finalize_json_hash(uint32_t h)
{
  const uint32_t result = (h ^ (h >> 28)) & 0x0FFFFFFFu;
  return result != 0u ? result : 1u;
}

uint32_t hash_utf8(std::string_view str)
{
  uint32_t h = 0x811c9dc5u;
  for (const char c : str) {
    h ^= static_cast<uint8_t>(c);
    h *= 0x01000193u;
  }
  return finalize_json_hash(h);
}

void hash_utf16_unit(uint32_t &h, uint32_t unit)
{
  for (size_t i = 0; i < sizeof(char16_t); ++i) {
    h ^= static_cast<uint8_t>(unit);
    h *= 0x01000193u;
    unit >>= 8;
  }
}

uint32_t hash_utf16(std::string_view str)
{
  uint32_t h = 0x811c9dc5u;
  for (size_t i = 0; i < str.size();) {
    const auto c = static_cast<uint8_t>(str[i++]);
    uint32_t cp = c;
    if ((c & 0x80u) != 0u) {
      const size_t extra = (c & 0xE0u) == 0xC0u ? 1u : ((c & 0xF0u) == 0xE0u ? 2u : 3u);
      cp = c & (0x7Fu >> extra);
      for (size_t j = 0; j < extra && i < str.size(); ++j) {
        cp = (cp << 6) | (static_cast<uint8_t>(str[i++]) & 0x3Fu);
      }
    }
    if (cp <= 0xFFFFu) {
      hash_utf16_unit(h, cp);
    } else {
      cp -= 0x10000u;
      hash_utf16_unit(h, 0xD800u + (cp >> 10));
      hash_utf16_unit(h, 0xDC00u + (cp & 0x3FFu));
    }
  }
  return finalize_json_hash(h);
}

size_t utf16_length(std::string_view str)
{
  size_t length = 0;
  for (size_t i = 0; i < str.size();) {
    const auto c = static_cast<uint8_t>(str[i++]);
    uint32_t cp = c;
    if ((c & 0x80u) != 0u) {
      const size_t extra = (c & 0xE0u) == 0xC0u ? 1u : ((c & 0xF0u) == 0xE0u ? 2u : 3u);
      cp = c & (0x7Fu >> extra);
      for (size_t j = 0; j < extra && i < str.size(); ++j) {
        cp = (cp << 6) | (static_cast<uint8_t>(str[i++]) & 0x3Fu);
      }
    }
    length += cp <= 0xFFFFu ? 1u : 2u;
  }
  return length;
}

struct StringHash
{
  using is_transparent = void;

  std::size_t operator()(std::string_view value) const noexcept { return std::hash<std::string_view>{}(value); }
  std::size_t operator()(const std::string &value) const noexcept { return (*this)(std::string_view{ value }); }
};

// Tracker keys borrow the immutable input tree, which outlives analysis and emission.
struct JsonRefHash
{
  std::size_t operator()(const nlohmann::ordered_json *value) const
  { return std::hash<nlohmann::ordered_json>{}(*value); }
};

struct JsonRefEqual
{
  bool operator()(const nlohmann::ordered_json *lhs, const nlohmann::ordered_json *rhs) const
  { return *lhs == *rhs; }
};

template<typename T>
using JsonRefMap = std::unordered_map<const nlohmann::ordered_json *, T, JsonRefHash, JsonRefEqual>;

using JsonRefSet = std::unordered_set<const nlohmann::ordered_json *, JsonRefHash, JsonRefEqual>;

struct ReuseTrackerBase
{
  JsonRefMap<int> counts;
  JsonRefMap<std::string> value_to_var;
  std::size_t counter = 0;
  const std::string prefix;

  explicit ReuseTrackerBase(std::string p) : prefix(std::move(p)) {}

  template<typename Predicate>
  void prepare_reuse_variables(Predicate should_share)
  {
    for (const auto &[value, count] : counts) {
      if (should_share(count)) value_to_var.try_emplace(value);
    }
  }

  bool is_shared(const nlohmann::ordered_json &value) const { return value_to_var.contains(&value); }
  std::pair<std::string &, bool> get_var_name(const nlohmann::ordered_json &value)
  {
    auto &name = value_to_var.at(&value);
    if (!name.empty()) return { name, false };
    name = std::format("{}{}", prefix, counter++);
    return { name, true };
  }
  std::size_t get_reused_count() const { return value_to_var.size(); }

  int32_t get_total_references_saved() const
  {
    int32_t total = 0;
    for (const auto &entry : value_to_var) { total += counts.at(entry.first) - 1; }
    return total;
  }
};

struct DuplicateTracker : ReuseTrackerBase
{
  std::size_t min_size = 2;

  using ReuseTrackerBase::ReuseTrackerBase;

  void track(const nlohmann::ordered_json &value)
  {
    if ((value.is_object() || value.is_array()) && value.size() >= min_size) { ++counts[&value]; }
  }

  void prepare_variables() { prepare_reuse_variables([](int count) { return count > 1; }); }
};

struct ScalarTracker
{
  int min_references = 2;
  JsonRefMap<int> counts;
  JsonRefMap<std::uint32_t> value_to_index;
  std::vector<const nlohmann::ordered_json *> pooled_values;

  void track(const nlohmann::ordered_json &value) { if (value.is_string()) ++counts[&value]; }

  bool is_shared(const nlohmann::ordered_json &value) const
  { const auto it = counts.find(&value); return it != counts.end() && it->second >= min_references; }

  std::uint32_t get_pool_index(const nlohmann::ordered_json &value)
  {
    const auto index = static_cast<std::uint32_t>(pooled_values.size());
    const auto [it, inserted] = value_to_index.try_emplace(&value, index);
    if (inserted) pooled_values.emplace_back(&value);
    return it->second;
  }

  bool can_use_uint8_indices(const nlohmann::ordered_json &object) const
  {
    JsonRefSet pending;
    auto next_index = pooled_values.size();
    for (auto itr = object.begin(); itr != object.end(); ++itr) {
      if (const auto existing = value_to_index.find(&itr.value()); existing != value_to_index.end()) {
        if (existing->second > 0xFFu) return false;
      } else if (pending.emplace(&itr.value()).second && next_index++ > 0xFFu) {
        return false;
      }
    }
    return true;
  }

  std::size_t get_reused_count() const { return pooled_values.size(); }

  int32_t get_total_references_saved() const
  {
    int32_t total = 0;
    for (const auto &value : pooled_values) total += counts.at(value) - 1;
    return total;
  }

  int use_count(const nlohmann::ordered_json &value) const
  { const auto it = counts.find(&value); return it == counts.end() ? 0 : it->second; }
};

enum class ObjectLayout
{
  Regular,
  DenseIndex,
  BlobByReference,
  PerfectHashBlobByReference,
  PerfectHash16BlobByReference,
  IndexedPerfectHashBlobByReference,
};

std::string make_layout_signature(const nlohmann::ordered_json &value)
{
  std::string signature;
  for (auto itr = value.begin(); itr != value.end(); ++itr) {
    signature += std::to_string(itr.key().size());
    signature += ':';
    signature += itr.key();
    signature += '|';
  }
  return signature;
}

struct TrackerSet
{
  DuplicateTracker object_tracker{ "o" };
  DuplicateTracker array_tracker{ "a" };
  ScalarTracker scalar_tracker;
};

template<typename Index>
struct MphfPlan
{
  std::vector<Index> displacements;
  std::vector<Index> slots;
  Index bucket_count = 0;
  Index seed1 = 0;
  Index seed2 = 0;
};

using Mphf8Plan = MphfPlan<std::uint8_t>;
using Mphf16Plan = MphfPlan<std::uint16_t>;

constexpr std::size_t mphf_linear_prefix = 16;
struct Mphf8TableInfo
{
  std::string name;
  Mphf8Plan utf8;
  Mphf8Plan utf16;
};

struct Mphf16TableInfo
{
  std::string name;
  Mphf16Plan utf8;
  Mphf16Plan utf16;
};

template<typename Index>
struct MphfBuckets
{
  std::vector<std::vector<Index>> values;
  std::vector<Index> order;
};

void analyze_json(const nlohmann::ordered_json &value,
  TrackerSet &trackers)
{
  if (value.is_object()) {
    trackers.object_tracker.track(value);
    for (auto itr = value.begin(); itr != value.end(); ++itr) { analyze_json(itr.value(), trackers); }
  } else if (value.is_array()) {
    trackers.array_tracker.track(value);
    for (std::size_t i = 0; i < value.size(); ++i) { analyze_json(value[i], trackers); }
  } else {
    trackers.scalar_tracker.track(value);
  }
}

struct EmitContext
{
  struct LayoutUsage
  {
    bool uses_dense_index = false;
    bool uses_blob_ref = false;
    bool uses_mphf8_blob_ref = false;
    bool uses_mphf16_blob_ref = false;
    bool uses_indexed_mphf8_blob_ref = false;
    bool uses_scalar_pool = false;
  };

  std::size_t &node_count;
  std::vector<std::string> &lines;
  TrackerSet &trackers;
  LayoutUsage &layout_usage;
  std::unordered_map<std::string, Mphf8TableInfo> mphf8_tables;
  std::unordered_map<std::string, Mphf16TableInfo> mphf16_tables;
  std::size_t mphf8_table_count = 0;
  std::size_t mphf16_table_count = 0;
};

std::string emit_value(const nlohmann::ordered_json &value, EmitContext &ctx);
std::string emit_node_body(const nlohmann::ordered_json &value, EmitContext &ctx);

template<typename Tracker, typename ValueEmitter>
std::string ensure_emitted(Tracker &tracker,
  const nlohmann::ordered_json &value,
  std::vector<std::string> &lines,
  ValueEmitter emit_initializer)
{
  const auto [var_name, first_use] = tracker.get_var_name(value);
  if (first_use) {
    lines.emplace_back(std::format("constexpr auto {} = json{{{{ {} }}}};", var_name, emit_initializer()));
  }
  return var_name;
}

ReuseTrackerBase *shared_tracker(const nlohmann::ordered_json &value, EmitContext &ctx)
{
  if (value.is_object() && ctx.trackers.object_tracker.is_shared(value)) return &ctx.trackers.object_tracker;
  if (value.is_array() && ctx.trackers.array_tracker.is_shared(value)) return &ctx.trackers.array_tracker;
  return nullptr;
}

std::string emit_scalar_value(const nlohmann::ordered_json &value)
{
  if (value.is_number_float()) return std::format("double{{{}}}", value.get<double>());
  if (value.is_number_unsigned()) return std::format("std::uint64_t{{{}}}", value.get<std::uint64_t>());
  if (value.is_number_integer()) return std::format("std::int64_t{{{}}}", value.get<std::int64_t>());
  if (value.is_boolean()) return std::format("bool{{{}}}", value.get<bool>());
  if (value.is_string()) return format_json_string(value.get_ref<const std::string &>());
  if (value.is_null()) return "std::nullptr_t{}";
  return "unhandled";
}

std::pair<std::uint32_t, std::uint32_t> value_hashes(const nlohmann::ordered_json &value)
{
  if (!value.is_string()) return { 0u, 0u };
  const auto &str = value.get_ref<const std::string &>();
  return { hash_utf8(str), hash_utf16(str) };
}

double estimate_value_ref_entry_savings(const nlohmann::ordered_json &value, const EmitContext &ctx)
{
  if (value.is_object()) return ctx.trackers.object_tracker.is_shared(value) ? 8.0 : -1.0e18;
  if (value.is_array()) return ctx.trackers.array_tracker.is_shared(value) ? 8.0 : -1.0e18;
  if (!ctx.trackers.scalar_tracker.is_shared(value)) return -1.0e18;

  const auto value_count = static_cast<double>(ctx.trackers.scalar_tracker.use_count(value));
  if (value_count == 0.0) return -1.0e18;
  return 8.0 - (16.0 / value_count);
}

bool can_use_indexed_mphf_values(const nlohmann::ordered_json &value, const EmitContext &ctx)
{
  if (!value.is_object()) return false;
  std::size_t key_offset = 0;
  for (const auto &item : value.items()) {
    if (!item.value().is_string() || !ctx.trackers.scalar_tracker.is_shared(item.value())) return false;
    if (item.key().size() > 0xFFu) return false;
    key_offset += item.key().size();
    if (key_offset > 0xFFFFu) return false;
  }
  return ctx.trackers.scalar_tracker.can_use_uint8_indices(value);
}

bool can_use_blob_keys(const nlohmann::ordered_json &value)
{
  std::size_t offset = 0;
  for (auto itr = value.begin(); itr != value.end(); ++itr) {
    if (itr.key().size() >= 4096) return false;
    offset += itr.key().size();
    if (offset >= (std::size_t{1} << 16)) return false;
  }
  return true;
}

std::uint32_t mphf_mix(std::uint32_t value, const std::uint32_t seed)
{
  value ^= seed + 0x9e3779b9u + (value << 6u) + (value >> 2u);
  value ^= value >> 16u;
  value *= 0x7feb352du;
  value ^= value >> 15u;
  value *= 0x846ca68bu;
  value ^= value >> 16u;
  return value;
}

constexpr std::uint32_t mphf_pilot_mix(const std::uint32_t value, const std::uint32_t pilot)
{
  return ((value << 16u) | (value >> 16u)) ^ ((pilot + 1u) * 0x9e3779b9u);
}

std::uint32_t mphf_reduce(const std::uint32_t value, const std::uint32_t range)
{
  return static_cast<std::uint32_t>((static_cast<std::uint64_t>(value) * range) >> 32u);
}

template<typename Index>
MphfBuckets<Index> build_mphf_buckets(const std::vector<std::uint32_t> &hashes,
  const Index bucket_count,
  const Index seed1)
{
  const auto size = static_cast<Index>(hashes.size());
  MphfBuckets<Index> buckets{ std::vector<std::vector<Index>>(bucket_count),
    std::vector<Index>(bucket_count) };
  for (Index i = 0; i < size; ++i)
    buckets.values[mphf_reduce(mphf_mix(hashes[i], seed1), bucket_count)].emplace_back(i);
  for (Index i = 0; i < bucket_count; ++i) buckets.order[i] = i;
  std::sort(buckets.order.begin(), buckets.order.begin() + bucket_count, [&](auto lhs, auto rhs) {
    return buckets.values[lhs].size() > buckets.values[rhs].size();
  });
  return buckets;
}

template<typename Index>
bool try_build_mphf_plan(const std::vector<std::uint32_t> &hashes,
  const MphfBuckets<Index> &buckets,
  MphfPlan<Index> &plan,
  const Index bucket_count,
  const Index seed1,
  const Index seed2)
{
  const auto size = static_cast<Index>(hashes.size());
  std::vector<bool> used(size);
  plan.displacements.assign(bucket_count, 0);
  plan.slots.assign(size, static_cast<Index>(-1));
  std::vector<std::uint32_t> trial_used(size);
  std::uint32_t trial_generation = 1;
  for (const auto bucket_index : buckets.order) {
    const auto &bucket = buckets.values[bucket_index];
    if (bucket.empty()) continue;

    bool placed = false;
    for (std::uint32_t displacement = 0; displacement < size && !placed; ++displacement) {
      bool collision = false;
      for (const auto key_index : bucket) {
        const auto mixed_hash = mphf_mix(hashes[key_index], seed1);
        const auto slot = static_cast<Index>(
          mphf_reduce(mphf_pilot_mix(mixed_hash, seed2 + displacement), size));
        if (used[slot] || trial_used[slot] == trial_generation) {
          collision = true;
          break;
        }
        trial_used[slot] = trial_generation;
      }
      ++trial_generation;
      if (collision) continue;

      plan.displacements[bucket_index] = static_cast<Index>(displacement);
      for (const auto key_index : bucket) {
        const auto mixed_hash = mphf_mix(hashes[key_index], seed1);
        const auto slot = static_cast<Index>(
          mphf_reduce(mphf_pilot_mix(mixed_hash, seed2 + displacement), size));
        used[slot] = true;
        plan.slots[slot] = key_index;
      }
      placed = true;
    }
    if (!placed) return false;
  }

  plan.bucket_count = bucket_count;
  plan.seed1 = seed1;
  plan.seed2 = seed2;
  return true;
}

template<typename Index>
bool build_mphf_plan(const nlohmann::ordered_json &value,
  const bool utf16,
  MphfPlan<Index> &plan,
  const std::size_t min_size,
  const std::size_t max_size)
{
  constexpr std::size_t max_mphf_attempts = 1'000'000;
  if (!value.is_object() || value.size() < min_size || value.size() > max_size) return false;

  std::vector<std::uint32_t> hashes;
  hashes.reserve(value.size() - mphf_linear_prefix);
  std::size_t index = 0;
  for (auto itr = value.begin(); itr != value.end(); ++itr) {
    if (index++ < mphf_linear_prefix) continue;
    const auto hash = utf16 ? hash_utf16(itr.key()) : hash_utf8(itr.key());
    if (std::ranges::contains(hashes, hash)) return false;
    hashes.emplace_back(hash);
  }

  const auto size = static_cast<Index>(hashes.size());
  const auto min_buckets = static_cast<Index>((value.size() + 2u) / 3u);
  std::size_t attempts = 0;
  for (std::uint32_t bucket_count = min_buckets; bucket_count <= size; ++bucket_count)
    for (std::uint16_t seed1 = 0; seed1 <= 0xFFu; ++seed1) {
      const auto buckets = build_mphf_buckets(
        hashes, static_cast<Index>(bucket_count), static_cast<Index>(seed1));
      for (std::uint16_t seed2 = 0; seed2 <= 0xFFu; ++seed2) {
        if (++attempts > max_mphf_attempts) return false;
        if (try_build_mphf_plan(hashes,
              buckets,
              plan,
              static_cast<Index>(bucket_count),
              static_cast<Index>(seed1),
              static_cast<Index>(seed2)))
          return true;
      }
    }
  return false;
}

bool build_mphf8_plan(const nlohmann::ordered_json &value, const bool utf16, Mphf8Plan &plan)
{ return build_mphf_plan(value, utf16, plan, 64u, 0xFFu); }

bool build_mphf16_plan(const nlohmann::ordered_json &value, const bool utf16, Mphf16Plan &plan)
{ return build_mphf_plan(value, utf16, plan, 0x100u, 0xFFFFu); }

std::string emit_uint8_array(const std::vector<std::uint8_t> &values)
{
  std::string result = "{";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) result += ", ";
    result += std::to_string(values[i]);
  }
  result += "}";
  return result;
}

std::string emit_uint16_array(const std::vector<std::uint16_t> &values)
{
  std::string result = "{";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) result += ", ";
    result += std::to_string(values[i]);
  }
  result += "}";
  return result;
}

std::string emit_mphf8_table_array(const std::string &table_name, const Mphf8Plan &plan)
{
  std::vector<std::uint8_t> values;
  values.reserve(plan.displacements.size() + plan.slots.size());
  values.insert(values.end(), plan.displacements.begin(), plan.displacements.end());
  values.insert(values.end(), plan.slots.begin(), plan.slots.end());
  return std::format("constexpr std::uint8_t {}[] = {};", table_name, emit_uint8_array(values));
}

std::string emit_mphf16_table_array(const std::string &table_name, const Mphf16Plan &plan)
{
  std::vector<std::uint16_t> values;
  values.reserve(plan.displacements.size() + plan.slots.size());
  values.insert(values.end(), plan.displacements.begin(), plan.displacements.end());
  values.insert(values.end(), plan.slots.begin(), plan.slots.end());
  return std::format("constexpr std::uint16_t {}[] = {};", table_name, emit_uint16_array(values));
}

void emit_utf_variants(std::vector<std::string> &lines, std::string utf8, std::string utf16)
{
  lines.emplace_back("#ifdef JSON2CPP_USE_UTF16");
  lines.emplace_back(std::move(utf16));
  lines.emplace_back("#else");
  lines.emplace_back(std::move(utf8));
  lines.emplace_back("#endif");
}

void emit_mphf8_table_arrays(const std::string &table_name,
  const Mphf8Plan &utf8_plan,
  const Mphf8Plan &utf16_plan,
  std::vector<std::string> &lines)
{
  emit_utf_variants(
    lines, emit_mphf8_table_array(table_name, utf8_plan), emit_mphf8_table_array(table_name, utf16_plan));
}

void emit_mphf16_table_arrays(const std::string &table_name,
  const Mphf16Plan &utf8_plan,
  const Mphf16Plan &utf16_plan,
  std::vector<std::string> &lines)
{
  emit_utf_variants(
    lines, emit_mphf16_table_array(table_name, utf8_plan), emit_mphf16_table_array(table_name, utf16_plan));
}

std::uint64_t make_mphf_prefix_mask(const nlohmann::ordered_json &value, const bool utf16)
{
  std::uint64_t mask = 0;
  std::size_t index = 0;
  for (auto itr = value.begin(); itr != value.end() && index < mphf_linear_prefix; ++itr, ++index) {
    const auto hash = utf16 ? hash_utf16(itr.key()) : hash_utf8(itr.key());
    mask |= std::uint64_t{1} << (hash & 63u);
  }
  return mask;
}

const Mphf8TableInfo &ensure_mphf8_table(const nlohmann::ordered_json &value,
  const Mphf8Plan &utf8_plan,
  const Mphf8Plan &utf16_plan,
  EmitContext &ctx)
{
  auto [it, inserted] = ctx.mphf8_tables.try_emplace(make_layout_signature(value));
  if (inserted) {
    it->second.name = std::format("h{}", ctx.mphf8_table_count++);
    it->second.utf8 = utf8_plan;
    it->second.utf16 = utf16_plan;
    emit_mphf8_table_arrays(it->second.name, it->second.utf8, it->second.utf16, ctx.lines);
  }
  return it->second;
}

const Mphf16TableInfo &ensure_mphf16_table(const nlohmann::ordered_json &value,
  const Mphf16Plan &utf8_plan,
  const Mphf16Plan &utf16_plan,
  EmitContext &ctx)
{
  auto [it, inserted] = ctx.mphf16_tables.try_emplace(make_layout_signature(value));
  if (inserted) {
    it->second.name = std::format("w{}", ctx.mphf16_table_count++);
    it->second.utf8 = utf8_plan;
    it->second.utf16 = utf16_plan;
    emit_mphf16_table_arrays(it->second.name, it->second.utf8, it->second.utf16, ctx.lines);
  }
  return it->second;
}

void emit_mphf8_descriptor(const std::string &node_name,
  const std::size_t size,
  const std::uint64_t utf8_prefix_mask,
  const std::uint64_t utf16_prefix_mask,
  const Mphf8TableInfo &table,
  std::vector<std::string> &lines)
{
  lines.emplace_back(std::format("extern const blob_pair_t {}[];", node_name));
  lines.emplace_back(std::format("extern const pair_t {}_items[];", node_name));
  const auto format = [&](const Mphf8Plan &plan, std::uint64_t prefix_mask) {
    return std::format("constexpr mphf8_blob_object_t {}_mphf{{{} + 2, {}, {}, {}, {}, {}, 0x{:016x}ull, {}_items}};",
      node_name,
      node_name,
      table.name,
      size,
      plan.bucket_count,
      plan.seed1,
      plan.seed2,
      prefix_mask,
      node_name);
  };
  emit_utf_variants(lines, format(table.utf8, utf8_prefix_mask), format(table.utf16, utf16_prefix_mask));
}

void emit_mphf16_descriptor(const std::string &node_name,
  const std::size_t size,
  const std::uint64_t utf8_prefix_mask,
  const std::uint64_t utf16_prefix_mask,
  const Mphf16TableInfo &table,
  std::vector<std::string> &lines)
{
  lines.emplace_back(std::format("extern const blob_pair_t {}[];", node_name));
  lines.emplace_back(std::format("extern const pair_t {}_items[];", node_name));
  const auto format = [&](const Mphf16Plan &plan, std::uint64_t prefix_mask) {
    return std::format("constexpr mphf16_blob_object_t {}_mphf{{{}, {}, {}, {}, {}, {}, 0x{:016x}ull, &json2cpp::detail::find_mphf16_blob_entry<basicType>, {}_items}};",
      node_name,
      node_name,
      table.name,
      size,
      plan.bucket_count,
      plan.seed1,
      plan.seed2,
      prefix_mask,
      node_name);
  };
  emit_utf_variants(lines, format(table.utf8, utf8_prefix_mask), format(table.utf16, utf16_prefix_mask));
}

void emit_indexed_mphf8_descriptor(const std::string &node_name,
  const std::size_t size,
  const std::uint64_t utf8_prefix_mask,
  const std::uint64_t utf16_prefix_mask,
  const Mphf8TableInfo &table,
  std::vector<std::string> &lines)
{
  const auto format = [&](const Mphf8Plan &plan, std::uint64_t prefix_mask) {
    return std::format("constexpr indexed_mphf8_blob_object_t {}_mphf{{{}.entries.data(), {}_keys, {{}}, {}.value_hashes.data(), {}.prefix_hashes.data(), {}, {}, {}, {}, {}, 0x{:016x}ull, {}_items}};",
      node_name,
      node_name,
      node_name,
      node_name,
      node_name,
      table.name,
      size,
      plan.bucket_count,
      plan.seed1,
      plan.seed2,
      prefix_mask,
      node_name);
  };
  emit_utf_variants(lines, format(table.utf8, utf8_prefix_mask), format(table.utf16, utf16_prefix_mask));
}
bool can_use_dense_index_object(const nlohmann::ordered_json &value)
{
  if (!value.is_object() || value.size() < 4u || value.size() > 0xFFFFu) return false;
  std::size_t index = 0;
  std::size_t key_size = 0;
  for (auto entry = value.begin(); entry != value.end(); ++entry, ++index) {
    const auto expected = std::to_string(index);
    if (entry.key() != expected) return false;
    key_size += expected.size();
    if (key_size > 0xFFFFu) return false;
  }
  return true;
}


ObjectLayout choose_object_layout(const nlohmann::ordered_json &value, const EmitContext &ctx)
{
  if (can_use_dense_index_object(value)) return ObjectLayout::DenseIndex;
  if (!value.is_object() || value.size() < 64u || !can_use_blob_keys(value))
    return ObjectLayout::Regular;
  for (auto itr = value.begin(); itr != value.end(); ++itr)
    if (estimate_value_ref_entry_savings(itr.value(), ctx) < -1.0e17)
      return ObjectLayout::Regular;
  return ObjectLayout::BlobByReference;
}

std::string emit_value_reference(const nlohmann::ordered_json &value, EmitContext &ctx)
{
  if (auto *tracker = shared_tracker(value, ctx))
    return std::format("&{}", ensure_emitted(*tracker, value, ctx.lines, [&] { return emit_node_body(value, ctx); }));

  ctx.layout_usage.uses_scalar_pool = true;
  return std::format("&s[{}]", ctx.trackers.scalar_tracker.get_pool_index(value));
}

std::string make_blob_literal(const nlohmann::ordered_json &value)
{
  std::string result = "J2C(";
  for (auto itr = value.begin(); itr != value.end(); ++itr) {
    result += std::format("\"{}\"", escape_string(itr.key()));
  }
  result += ")";
  return value.empty() ? "J2C(\"\")" : result;
}

std::string scalar_pool_index(std::string_view value_ref)
{
  constexpr std::string_view prefix = "&s[";
  if (!value_ref.starts_with(prefix) || !value_ref.ends_with(']')) return {};
  return std::string(value_ref.substr(prefix.size(), value_ref.size() - prefix.size() - 1u));
}

std::string emit_blob_entry(const std::string &value_ref,
  const nlohmann::ordered_json &value,
  const std::string &key,
  const std::size_t key_offset,
  const std::size_t utf16_key_offset)
{
  const auto utf16_key_length = utf16_length(key);
  const auto [value_hash_utf8, value_hash_utf16] = value_hashes(value);
  const auto pool_index = scalar_pool_index(value_ref);
  const auto macro_name = pool_index.empty() ? "J2B" : "J2BS";
  const auto macro_value = pool_index.empty() ? value_ref : pool_index;
  return std::format("{}({}, {}, {}, {}, {}, {}, {}, {}, {}),",
    macro_name,
    macro_value,
    key_offset,
    key_offset - utf16_key_offset,
    key.size(),
    key.size() - utf16_key_length,
    hash_utf8(key),
    hash_utf16(key),
    value_hash_utf8,
    value_hash_utf16);
}

std::string emit_object(const nlohmann::ordered_json &value, EmitContext &ctx, const std::string &node_name)
{
  if (value.empty()) return "object_t{}";
  auto layout = choose_object_layout(value, ctx);
  Mphf8Plan utf8_mphf, utf16_mphf;
  Mphf16Plan utf8_mphf16, utf16_mphf16;
  const bool use_mphf = layout == ObjectLayout::BlobByReference
                     && build_mphf8_plan(value, false, utf8_mphf)
                     && build_mphf8_plan(value, true, utf16_mphf);
  const bool use_mphf16 = layout == ObjectLayout::BlobByReference && !use_mphf
                       && build_mphf16_plan(value, false, utf8_mphf16)
                       && build_mphf16_plan(value, true, utf16_mphf16);
  if (use_mphf) {
    layout = can_use_indexed_mphf_values(value, ctx) ? ObjectLayout::IndexedPerfectHashBlobByReference
                                                     : ObjectLayout::PerfectHashBlobByReference;
  } else if (use_mphf16) {
    layout = ObjectLayout::PerfectHash16BlobByReference;
  } else if (layout == ObjectLayout::BlobByReference) {
    layout = ObjectLayout::Regular;
  }
  if (layout == ObjectLayout::DenseIndex) ctx.layout_usage.uses_dense_index = true;
  if (layout == ObjectLayout::BlobByReference || layout == ObjectLayout::PerfectHashBlobByReference
      || layout == ObjectLayout::PerfectHash16BlobByReference)
    ctx.layout_usage.uses_blob_ref = true;
  if (layout == ObjectLayout::PerfectHashBlobByReference) ctx.layout_usage.uses_mphf8_blob_ref = true;
  if (layout == ObjectLayout::PerfectHash16BlobByReference) ctx.layout_usage.uses_mphf16_blob_ref = true;
  if (layout == ObjectLayout::IndexedPerfectHashBlobByReference) {
    ctx.layout_usage.uses_indexed_mphf8_blob_ref = true;
  }

  std::vector<std::string> entries;
  std::vector<std::string> item_entries;
  if (layout != ObjectLayout::Regular) item_entries.reserve(value.size());

  entries.reserve(value.size()
                + (layout == ObjectLayout::PerfectHashBlobByReference
                     || layout == ObjectLayout::PerfectHash16BlobByReference ? 2u
                   : layout == ObjectLayout::BlobByReference           ? 1u
                                                                        : 0u));

  if (layout == ObjectLayout::BlobByReference || layout == ObjectLayout::PerfectHashBlobByReference
      || layout == ObjectLayout::PerfectHash16BlobByReference
      || layout == ObjectLayout::IndexedPerfectHashBlobByReference) {
    ctx.lines.emplace_back(std::format("constexpr basicType {}_keys[] = {};", node_name, make_blob_literal(value)));
    if (layout == ObjectLayout::PerfectHashBlobByReference) {
      emit_mphf8_descriptor(node_name,
        value.size(),
        make_mphf_prefix_mask(value, false),
        make_mphf_prefix_mask(value, true),
        ensure_mphf8_table(value, utf8_mphf, utf16_mphf, ctx),
        ctx.lines);
      entries.emplace_back(std::format("blob_pair_t{{&{}_mphf, blob_pair_t::header_t{{}}}},", node_name));
    }
    if (layout == ObjectLayout::PerfectHash16BlobByReference) {
      emit_mphf16_descriptor(node_name,
        value.size(),
        make_mphf_prefix_mask(value, false),
        make_mphf_prefix_mask(value, true),
        ensure_mphf16_table(value, utf8_mphf16, utf16_mphf16, ctx),
        ctx.lines);
      entries.emplace_back(std::format("blob_pair_t{{&{}_mphf, blob_pair_t::header_t{{}}}},", node_name));
    }
    if (layout != ObjectLayout::IndexedPerfectHashBlobByReference) {
      entries.emplace_back(std::format("blob_pair_t{{{}_keys, blob_pair_t::header_t{{}}}},", node_name));
    }
  }

  std::size_t key_offset = 0;
  std::size_t utf16_key_offset = 0;
  for (auto itr = value.begin(); itr != value.end(); ++itr) {
    const auto value_repr = emit_value(itr.value(), ctx);
    if (layout != ObjectLayout::Regular) {
      const bool blob_keys = layout == ObjectLayout::PerfectHashBlobByReference
                          || layout == ObjectLayout::PerfectHash16BlobByReference
                          || layout == ObjectLayout::IndexedPerfectHashBlobByReference;
      const auto utf16_key_length = utf16_length(itr.key());
      const auto key_repr = blob_keys
        ? std::format("J2K({}_keys, {}, {}, {}, {})",
            node_name, key_offset, key_offset - utf16_key_offset,
            itr.key().size(), itr.key().size() - utf16_key_length)
        : format_json_string(itr.key());
      item_entries.emplace_back(
        std::format("pair_t{{{}, {}}},", key_repr, value_repr));
    }
    if (layout == ObjectLayout::IndexedPerfectHashBlobByReference) {
      const auto utf16_key_length = utf16_length(itr.key());
      key_offset += itr.key().size();
      utf16_key_offset += utf16_key_length;
    } else if (layout == ObjectLayout::PerfectHashBlobByReference
               || layout == ObjectLayout::PerfectHash16BlobByReference) {
      entries.emplace_back(emit_blob_entry(emit_value_reference(itr.value(), ctx),
        itr.value(),
        itr.key(),
        key_offset,
        utf16_key_offset));
      const auto utf16_key_length = utf16_length(itr.key());
      key_offset += itr.key().size();
      utf16_key_offset += utf16_key_length;
    } else {
      entries.emplace_back(std::format("pair_t{{{}, {}}},", format_json_string(itr.key()), value_repr));
    }
  }

  if (layout != ObjectLayout::Regular) {
    ctx.lines.emplace_back(std::format("constexpr pair_t {}_items[] = {{", node_name));
    for (const auto &entry : item_entries)
      ctx.lines.emplace_back(std::format("  {}", entry));
    ctx.lines.emplace_back("};");
  }

  if (layout == ObjectLayout::IndexedPerfectHashBlobByReference) {
    ctx.lines.emplace_back(std::format(
      "constexpr auto {} = json2cpp::detail::make_indexed_blob_storage({}_items);",
      node_name, node_name));
    emit_indexed_mphf8_descriptor(node_name,
      value.size(),
      make_mphf_prefix_mask(value, false),
      make_mphf_prefix_mask(value, true),
      ensure_mphf8_table(value, utf8_mphf, utf16_mphf, ctx),
      ctx.lines);
    return std::format("&{}_mphf", node_name);
  }
  if (layout == ObjectLayout::DenseIndex) {
    return std::format("dense_object_ref_t{{{}_items, {}}}", node_name, value.size());
  }

  const auto entry_type = layout == ObjectLayout::PerfectHashBlobByReference
                               || layout == ObjectLayout::PerfectHash16BlobByReference
                          ? "blob_pair_t" : "pair_t";
  ctx.lines.emplace_back(std::format("constexpr {} {}[] = {{", entry_type, node_name));

  for (const auto &entry : entries) { ctx.lines.emplace_back(std::format("  {}", entry)); }
  ctx.lines.emplace_back("};");
  if (layout == ObjectLayout::PerfectHashBlobByReference
      || layout == ObjectLayout::PerfectHash16BlobByReference) return std::format("&{}_mphf", node_name);
  return std::format("object_t{{{}}}", node_name);
}

std::string emit_array(const nlohmann::ordered_json &value, EmitContext &ctx, const std::string &node_name)
{
  if (value.empty()) return "array_t{}";
  std::vector<std::string> entries;
  entries.reserve(value.size());
  for (const auto &child : value) { entries.emplace_back(std::format("{},", emit_value(child, ctx))); }

  ctx.lines.emplace_back(std::format("constexpr json {}[] = {{", node_name));
  for (const auto &entry : entries) { ctx.lines.emplace_back(std::format("  {}", entry)); }
  ctx.lines.emplace_back("};");
  return std::format("array_t{{{}}}", node_name);
}

std::string emit_node_body(const nlohmann::ordered_json &value, EmitContext &ctx)
{
  const std::string node_name = std::format("d{}", ctx.node_count++);
  if (value.is_object()) return emit_object(value, ctx, node_name);
  if (value.is_array()) return emit_array(value, ctx, node_name);
  return {};
}

std::string emit_value(const nlohmann::ordered_json &value, EmitContext &ctx)
{
  if (auto *tracker = shared_tracker(value, ctx))
    return ensure_emitted(*tracker, value, ctx.lines, [&] { return emit_node_body(value, ctx); });

  if (value.is_object() || value.is_array()) return emit_node_body(value, ctx);
  return emit_scalar_value(value);
}

TrackerSet build_trackers(const nlohmann::ordered_json &json)
{
  TrackerSet trackers;
  analyze_json(json, trackers);
  trackers.object_tracker.prepare_variables();
  trackers.array_tracker.prepare_variables();
  return trackers;
}

compile_results compile_impl(const std::string_view original_name, const nlohmann::ordered_json &json)
{
  const std::string document_name = sanitize_identifier(original_name);
  auto trackers = build_trackers(json);
  compile_results results;

  results.hpp.emplace_back(std::format("#ifndef {}_COMPILED_JSON", document_name));
  results.hpp.emplace_back(std::format("#define {}_COMPILED_JSON", document_name));
  results.hpp.emplace_back("#include <json2cpp/json2cpp.hpp>");
  results.hpp.emplace_back(std::format("namespace compiled_json::{} {{", document_name));
  results.hpp.emplace_back("  const json2cpp::json &get();");
  results.hpp.emplace_back("}");
  results.hpp.emplace_back("#endif");

  EmitContext::LayoutUsage layout_usage;
  std::vector<std::string> impl_body;
  results.impl.emplace_back(std::format("#ifndef {}_COMPILED_JSON_IMPL", document_name));
  results.impl.emplace_back(std::format("#define {}_COMPILED_JSON_IMPL", document_name));
  results.impl.emplace_back("#include <json2cpp/json2cpp.hpp>");
  results.impl.emplace_back(std::format(R"(
using namespace std::literals::string_view_literals;
namespace compiled_json::{}::impl {{
  #ifdef JSON2CPP_USE_UTF16
  typedef char16_t basicType;
  #define RAW_PREFIX(str) u"" str ""sv
  #else
  typedef char basicType;
  #define RAW_PREFIX(str) str ""sv
  #endif
  using json = json2cpp::basic_json<basicType>;
  using array_t = json2cpp::basic_array_t<basicType>;
  using object_t = json2cpp::basic_object_t<basicType>;
  using pair_t = json2cpp::basic_value_pair_t<basicType>;
  )",
    document_name));

  std::size_t node_count = 0;
  EmitContext ctx{ node_count, impl_body, trackers, layout_usage, {}, {}, 0, 0 };
  const auto root_repr = emit_value(json, ctx);

  if (layout_usage.uses_dense_index) {
    results.impl.emplace_back("  using dense_object_ref_t = json2cpp::basic_dense_object_ref_t<basicType>;");
  }
  if (layout_usage.uses_blob_ref || layout_usage.uses_indexed_mphf8_blob_ref || layout_usage.uses_dense_index) {
    results.impl.emplace_back(R"(  #ifdef JSON2CPP_USE_UTF16
  #define J2C(str) u"" str
  #define J2D(utf8_size, utf16_delta) (utf8_size - utf16_delta)
  #else
  #define J2C(str) str
  #define J2D(utf8_size, utf16_delta) utf8_size
  #endif
  #define J2K(keys, offset, offset_delta, length, length_delta) std::basic_string_view<basicType>{keys + J2D(offset, offset_delta), J2D(length, length_delta)}
)");
  }
  if (layout_usage.uses_blob_ref) {
    results.impl.emplace_back(R"(  #ifdef JSON2CPP_USE_UTF16
  #define J2H(utf8_hash, utf16_hash) utf16_hash
  #else
  #define J2H(utf8_hash, utf16_hash) utf8_hash
    #endif)");
    results.impl.emplace_back(
      "  #define J2B(value, offset, offset_delta, length, length_delta, hash_utf8, hash_utf16, value_hash_utf8, value_hash_utf16) "
      "blob_pair_t{value, J2D(offset, offset_delta), J2D(length, length_delta), J2H(hash_utf8, hash_utf16), "
      "J2H(value_hash_utf8, value_hash_utf16)}");
    results.impl.emplace_back("  #define J2BS(value_index, ...) J2B(&s[value_index], __VA_ARGS__)");
    results.impl.emplace_back("  using blob_pair_t = json2cpp::basic_blob_ref_value_pair_t<basicType>;");
    results.impl.emplace_back("  using blob_object_t = json2cpp::basic_blob_ref_object_t<basicType>;");
    if (layout_usage.uses_mphf8_blob_ref) {
      results.impl.emplace_back("  using mphf8_blob_object_t = json2cpp::detail::basic_mphf8_blob_ref_object_t<basicType>;");
    }
    if (layout_usage.uses_mphf16_blob_ref) {
      results.impl.emplace_back(
        "  using mphf16_blob_object_t = json2cpp::detail::basic_mphf16_blob_ref_object_t<basicType>;");
    }
  }
  if (layout_usage.uses_indexed_mphf8_blob_ref) {
    results.impl.emplace_back(
      "  using indexed_mphf8_blob_object_t = json2cpp::detail::basic_indexed_mphf8_blob_ref_object_t<basicType>;");
  }
  if (layout_usage.uses_scalar_pool && !trackers.scalar_tracker.pooled_values.empty()) {
    results.impl.emplace_back("  constexpr json s[] = {");
    for (const auto value : trackers.scalar_tracker.pooled_values) {
      results.impl.emplace_back(std::format("    json{{{{ {} }}}},", emit_scalar_value(*value)));
    }
    results.impl.emplace_back("  };");
  }
  results.impl.insert(results.impl.end(), impl_body.begin(), impl_body.end());

  results.impl.emplace_back(std::format(R"(
  constexpr auto document = json{{ {} }};
}}
#endif)",
    root_repr));

  std::cout << node_count << " JSON nodes emitted.\n"
            << trackers.array_tracker.get_reused_count() << " duplicate arrays reused (min size: "
            << trackers.array_tracker.min_size << "), saving "
            << trackers.array_tracker.get_total_references_saved() << " references.\n"
            << trackers.object_tracker.get_reused_count() << " duplicate objects reused (min size: "
            << trackers.object_tracker.min_size << "), saving "
            << trackers.object_tracker.get_total_references_saved() << " references.\n"
            << trackers.scalar_tracker.get_reused_count() << " duplicate scalar values reused (min references: "
            << trackers.scalar_tracker.min_references << "), saving "
            << trackers.scalar_tracker.get_total_references_saved() << " references.\n";

  return results;
}

}

compile_results compile(const std::string_view document_name, const nlohmann::json &json)
{
  return compile_impl(document_name, nlohmann::ordered_json(json));
}

compile_results compile(const std::string_view document_name, const std::filesystem::path &filename)
{
  std::cout << "Loading file: '" << filename.string() << "'\n";
  std::ifstream input(filename);
  if (!input) throw std::runtime_error(std::format("Unable to open input file '{}'", filename.string()));

  nlohmann::ordered_json document;
  input >> document;
  std::cout << "File loaded\n";
  return compile_impl(document_name, document);
}

void write_compilation(std::string_view document_name,
  const compile_results &results,
  const std::filesystem::path &base_output)
{
  const std::string sanitized_name = sanitize_identifier(document_name);
  const auto append_extension = [](std::filesystem::path name, std::string_view ext) { return name += ext; };
  const auto hpp_name = append_extension(base_output, ".hpp");
  const auto cpp_name = append_extension(base_output, ".cpp");
  const auto impl_name = append_extension(base_output, "_impl.hpp");

  if (const auto parent = base_output.parent_path(); !parent.empty())
    std::filesystem::create_directories(parent);

  const auto write_lines = [](const std::filesystem::path &path, const std::vector<std::string> &lines) {
    std::ofstream output(path);
    if (!output) throw std::runtime_error(std::format("Unable to open output file '{}'", path.string()));
    for (const auto &line : lines) output << line << '\n';
    if (!output) throw std::runtime_error(std::format("Unable to write output file '{}'", path.string()));
  };

  write_lines(hpp_name, results.hpp);
  write_lines(impl_name, results.impl);

  std::ofstream cpp(cpp_name);
  if (!cpp) throw std::runtime_error(std::format("Unable to open output file '{}'", cpp_name.string()));
  cpp << std::format("#include \"{}\"\n", impl_name.filename().string());
  cpp << std::format(
    "namespace compiled_json::{} {{\nconst json2cpp::json &get() {{ return compiled_json::{}::impl::document; }}\n}}\n",
    sanitized_name,
    sanitized_name);
  if (!cpp) throw std::runtime_error(std::format("Unable to write output file '{}'", cpp_name.string()));
}

void compile_to(const std::string_view document_name,
  const nlohmann::json &json,
  const std::filesystem::path &base_output)
{
  write_compilation(document_name, compile(document_name, json), base_output);
}

void compile_to(const std::string_view document_name,
  const std::filesystem::path &filename,
  const std::filesystem::path &base_output)
{
  write_compilation(document_name, compile(document_name, filename), base_output);
}
