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
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <set>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <unordered_map>
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
  std::string result;
  result.reserve(name.size());
  for (char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      result.push_back(c);
    } else {
      result.push_back('_');
    }
  }
  if (!result.empty() && std::isdigit(static_cast<unsigned char>(result[0]))) { result.insert(0, "_"); }
  return result.empty() ? "json_doc" : result;
}

std::string escape_string(const std::string &str)
{
  std::string result;
  result.reserve(str.size());
  for (char c : str) {
    switch (c) {
    case '"': result += "\\\""; break;
    case '\\': result += "\\\\"; break;
    case '\n': result += "\\n"; break;
    case '\r': result += "\\r"; break;
    case '\t': result += "\\t"; break;
    default: result += c; break;
    }
  }
  return result;
}

std::string format_json_string(const std::string &str)
{
  return fmt::format("RAW_PREFIX(\"{}\")", escape_string(str));
}

uint32_t finalize_json_hash(uint32_t h)
{
  const uint32_t result = (h ^ (h >> 28)) & 0x0FFFFFFFu;
  return result != 0u ? result : 1u;
}

uint32_t hash_utf8(std::string_view str)
{
  uint32_t h = 0x811c9dc5u;
  for (const unsigned char c : str) {
    h ^= c;
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

inline void hash_combine(std::size_t &seed, std::size_t value)
{
  seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

struct JsonHasher
{
  std::size_t operator()(const nlohmann::ordered_json &j) const
  {
    std::size_t seed = 0;

    hash_combine(seed, static_cast<std::size_t>(j.type()));
    switch (j.type()) {
    case nlohmann::ordered_json::value_t::null:
    case nlohmann::ordered_json::value_t::discarded:
      break;
    case nlohmann::ordered_json::value_t::object:
      for (auto it = j.begin(); it != j.end(); ++it) {
        hash_combine(seed, std::hash<std::string>{}(it.key()));
        hash_combine(seed, (*this)(it.value()));
      }
      break;
    case nlohmann::ordered_json::value_t::array:
      for (const auto &element : j) { hash_combine(seed, (*this)(element)); }
      break;
    case nlohmann::ordered_json::value_t::string:
      hash_combine(seed, std::hash<std::string>{}(j.get_ref<const std::string &>()));
      break;
    case nlohmann::ordered_json::value_t::boolean:
      hash_combine(seed, std::hash<bool>{}(j.get<bool>()));
      break;
    case nlohmann::ordered_json::value_t::number_integer:
      hash_combine(seed, std::hash<std::int64_t>{}(j.get<std::int64_t>()));
      break;
    case nlohmann::ordered_json::value_t::number_unsigned:
      hash_combine(seed, std::hash<std::uint64_t>{}(j.get<std::uint64_t>()));
      break;
    case nlohmann::ordered_json::value_t::number_float:
      hash_combine(seed, std::hash<double>{}(j.get<double>()));
      break;
    default:
      break;
    }
    return seed;
  }
};

struct JsonEqual
{
  bool operator()(const nlohmann::ordered_json &a, const nlohmann::ordered_json &b) const { return a == b; }
};

struct ReuseTrackerBase
{
  std::unordered_map<nlohmann::ordered_json, int, JsonHasher, JsonEqual> counts;
  std::unordered_map<nlohmann::ordered_json, std::string, JsonHasher, JsonEqual> value_to_var;
  std::set<std::string> processed_vars;
  std::size_t counter = 0;
  const std::string prefix;

  explicit ReuseTrackerBase(std::string p) : prefix(std::move(p)) {}

  template<typename Predicate>
  void prepare_reuse_variables(Predicate should_share)
  {
    for (const auto &[value, count] : counts) {
      if (should_share(count)) value_to_var[value] = fmt::format("{}{}", prefix, counter++);
    }
  }

  bool is_shared(const nlohmann::ordered_json &value) const { return value_to_var.contains(value); }
  bool is_processed(const std::string &var_name) const { return processed_vars.contains(var_name); }
  void mark_as_processed(const std::string &var_name) { processed_vars.insert(var_name); }
  const std::string &get_var_name(const nlohmann::ordered_json &value) const { return value_to_var.at(value); }
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
    if ((value.is_object() || value.is_array()) && value.size() >= min_size) { ++counts[value]; }
  }

  void prepare_variables() { prepare_reuse_variables([](int count) { return count > 1; }); }
};

struct ScalarTracker : ReuseTrackerBase
{
  int min_references = 2;
  std::unordered_map<nlohmann::ordered_json, std::uint32_t, JsonHasher, JsonEqual> value_to_index;
  std::vector<nlohmann::ordered_json> pooled_values;

  using ReuseTrackerBase::ReuseTrackerBase;

  void track(const nlohmann::ordered_json &value) { if (value.is_string()) ++counts[value]; }

  void prepare_variables()
  {
    prepare_reuse_variables([this](int count) { return count >= min_references; });
    pooled_values.reserve(value_to_var.size());
    for (const auto &[value, _] : value_to_var) {
      value_to_index.emplace(value, static_cast<std::uint32_t>(pooled_values.size()));
      pooled_values.emplace_back(value);
    }
  }

  std::uint32_t get_pool_index(const nlohmann::ordered_json &value) const { return value_to_index.at(value); }

  int use_count(const nlohmann::ordered_json &value) const
  { const auto it = counts.find(value); return it == counts.end() ? 0 : it->second; }
};

enum class ObjectLayout
{
  Regular,
  CompactInline,
  ValueByReference,
  BlobByReference,
  PerfectHashBlobByReference,
  IndexedPerfectHashBlobByReference,
};

struct KeyLayoutTracker
{
  std::unordered_map<std::string, std::size_t> layout_counts;
  std::unordered_map<std::string, std::size_t> key_counts;
  std::unordered_map<std::string, std::string> key_to_var;
  std::set<std::string> emitted_keys;
  std::size_t counter = 0;
  double min_compact_savings = 64.0;

  static std::string make_layout_signature(const nlohmann::ordered_json &value)
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

  void count_layout(const nlohmann::ordered_json &value)
  {
    ++layout_counts[make_layout_signature(value)];
    for (auto itr = value.begin(); itr != value.end(); ++itr) { ++key_counts[itr.key()]; }
  }

  std::size_t key_use_count(std::string_view key) const
  {
    const auto it = key_counts.find(std::string(key));
    return it == key_counts.end() ? 0 : it->second;
  }

  std::size_t layout_use_count(const nlohmann::ordered_json &value) const
  {
    const auto it = layout_counts.find(make_layout_signature(value));
    return it == layout_counts.end() ? 0 : it->second;
  }

  double estimate_inline_object_savings(const nlohmann::ordered_json &value) const
  {
    if (!value.is_object() || value.empty()) return 0.0;

    double key_savings = 0.0;
    for (auto itr = value.begin(); itr != value.end(); ++itr) {
      const auto key_count = static_cast<double>(key_use_count(itr.key()));
      if (key_count == 0.0) return -1.0;
      key_savings += 8.0 - (16.0 / key_count);
    }

    const auto layout_count = static_cast<double>(layout_counts.at(make_layout_signature(value)));
    const auto layout_savings = static_cast<double>(value.size()) * ((layout_count * 8.0) - 16.0);
    return std::max(key_savings, layout_savings);
  }

  std::string ensure_key_definition(const std::string &key, std::vector<std::string> &lines)
  {
    auto [it, _] = key_to_var.try_emplace(key, fmt::format("k{}", counter++));
    if (emitted_keys.insert(key).second) {
      lines.emplace_back(fmt::format("constexpr key_descriptor_t {}{{{}}};", it->second, format_json_string(key)));
    }
    return it->second;
  }

  std::size_t descriptor_count() const { return key_to_var.size(); }
};

struct TrackerSet
{
  KeyLayoutTracker key_tracker;
  DuplicateTracker object_tracker{ "o" };
  DuplicateTracker array_tracker{ "a" };
  ScalarTracker scalar_tracker{ "s" };
};

struct Mphf8Plan
{
  std::vector<std::uint8_t> displacements;
  std::vector<std::uint8_t> slots;
  std::uint8_t bucket_count = 0;
  std::uint8_t seed1 = 0;
  std::uint8_t seed2 = 0;
};

struct Mphf8TableInfo
{
  std::string name;
  Mphf8Plan utf8;
  Mphf8Plan utf16;
};

void analyze_json(const nlohmann::ordered_json &value,
  TrackerSet &trackers)
{
  if (value.is_object()) {
    trackers.object_tracker.track(value);
    trackers.key_tracker.count_layout(value);
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
    bool uses_compact_inline = false;
    bool uses_value_ref = false;
    bool uses_blob_ref = false;
    bool uses_mphf8_blob_ref = false;
    bool uses_indexed_mphf8_blob_ref = false;
    bool uses_scalar_pool = false;
  };

  std::size_t &node_count;
  std::vector<std::string> &lines;
  TrackerSet &trackers;
  LayoutUsage &layout_usage;
  std::unordered_map<std::string, Mphf8TableInfo> mphf8_tables;
  std::size_t mphf8_table_count = 0;
};

std::string emit_value(const nlohmann::ordered_json &value, EmitContext &ctx);
std::string emit_node_body(const nlohmann::ordered_json &value, EmitContext &ctx);

template<typename Tracker, typename ValueEmitter>
std::string ensure_emitted(Tracker &tracker,
  const nlohmann::ordered_json &value,
  std::vector<std::string> &lines,
  ValueEmitter emit_initializer)
{
  const auto &var_name = tracker.get_var_name(value);
  if (!tracker.is_processed(var_name)) {
    tracker.mark_as_processed(var_name);
    lines.emplace_back(fmt::format("constexpr auto {} = json{{{{ {} }}}};", var_name, emit_initializer()));
  }
  return var_name;
}

std::string emit_scalar_value(const nlohmann::ordered_json &value)
{
  if (value.is_number_float()) return fmt::format("double{{{}}}", value.get<double>());
  if (value.is_number_unsigned()) return fmt::format("std::uint64_t{{{}}}", value.get<std::uint64_t>());
  if (value.is_number_integer()) return fmt::format("std::int64_t{{{}}}", value.get<std::int64_t>());
  if (value.is_boolean()) return fmt::format("bool{{{}}}", value.get<bool>());
  if (value.is_string()) return format_json_string(value.get<std::string>());
  if (value.is_null()) return "std::nullptr_t{}";
  return "unhandled";
}

std::pair<std::uint32_t, std::uint32_t> value_hashes(const nlohmann::ordered_json &value)
{
  if (!value.is_string()) return { 0u, 0u };
  const auto str = value.get<std::string>();
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
  if (ctx.trackers.scalar_tracker.pooled_values.size() > 0xFFFFu) return false;
  std::size_t key_offset = 0;
  for (auto itr = value.begin(); itr != value.end(); ++itr) {
    if (!itr.value().is_string() || !ctx.trackers.scalar_tracker.is_shared(itr.value())) return false;
    if (ctx.trackers.scalar_tracker.get_pool_index(itr.value()) > 0xFFu) return false;
    if (itr.key().size() > 0xFFu) return false;
    key_offset += itr.key().size();
    if (key_offset > 0xFFFFu) return false;
  }
  return true;
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

bool try_build_mphf8_plan(const std::vector<std::uint32_t> &hashes,
  Mphf8Plan &plan,
  const std::uint8_t bucket_count,
  const std::uint8_t seed1,
  const std::uint8_t seed2)
{
  const auto size = static_cast<std::uint8_t>(hashes.size());
  std::vector<std::vector<std::uint8_t>> buckets(bucket_count);
  for (std::uint8_t i = 0; i < size; ++i) buckets[mphf_mix(hashes[i], seed1) % bucket_count].emplace_back(i);

  std::vector<std::uint8_t> order(bucket_count);
  for (std::uint8_t i = 0; i < bucket_count; ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](auto lhs, auto rhs) { return buckets[lhs].size() > buckets[rhs].size(); });

  std::vector<bool> used(size, false);
  plan.displacements.assign(bucket_count, 0);
  plan.slots.assign(size, 0xFFu);
  std::array<std::uint16_t, 256> trial_used{};
  std::uint16_t trial_generation = 1;
  for (const auto bucket_index : order) {
    const auto &bucket = buckets[bucket_index];
    if (bucket.empty()) continue;

    bool placed = false;
    for (std::uint16_t displacement = 0; displacement < size && !placed; ++displacement) {
      std::vector<std::uint8_t> trial;
      trial.reserve(bucket.size());
      bool collision = false;
      for (const auto key_index : bucket) {
        const auto slot = static_cast<std::uint8_t>((mphf_mix(hashes[key_index], seed2) + displacement) % size);
        if (used[slot] || trial_used[slot] == trial_generation) {
          collision = true;
          break;
        }
        trial_used[slot] = trial_generation;
        trial.emplace_back(slot);
      }
      ++trial_generation;
      if (collision) continue;

      plan.displacements[bucket_index] = static_cast<std::uint8_t>(displacement);
      for (std::size_t i = 0; i < bucket.size(); ++i) {
        used[trial[i]] = true;
        plan.slots[trial[i]] = bucket[i];
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

bool build_mphf8_plan(const nlohmann::ordered_json &value, const bool utf16, Mphf8Plan &plan)
{
  constexpr std::size_t min_mphf_size = 64;
  constexpr std::size_t max_mphf_attempts = 1'000'000;
  if (!value.is_object() || value.size() < min_mphf_size || value.size() > 0xFFu) return false;

  std::vector<std::uint32_t> hashes;
  hashes.reserve(value.size());
  for (auto itr = value.begin(); itr != value.end(); ++itr) {
    const auto hash = utf16 ? hash_utf16(itr.key()) : hash_utf8(itr.key());
    if (std::ranges::contains(hashes, hash)) return false;
    hashes.emplace_back(hash);
  }

  const auto size = static_cast<std::uint8_t>(value.size());
  const auto min_buckets = static_cast<std::uint8_t>((value.size() + 2u) / 3u);
  std::size_t attempts = 0;
  for (std::uint16_t bucket_count = min_buckets; bucket_count <= size; ++bucket_count)
    for (std::uint16_t seed1 = 0; seed1 <= 0xFFu; ++seed1)
      for (std::uint16_t seed2 = 0; seed2 <= 0xFFu; ++seed2) {
        if (++attempts > max_mphf_attempts) return false;
        if (try_build_mphf8_plan(hashes,
              plan,
              static_cast<std::uint8_t>(bucket_count),
              static_cast<std::uint8_t>(seed1),
              static_cast<std::uint8_t>(seed2)))
          return true;
      }
  return false;
}

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

void emit_mphf8_table_array(const std::string &table_name,
  const Mphf8Plan &plan,
  std::vector<std::string> &lines)
{
  std::vector<std::uint8_t> values;
  values.reserve(plan.displacements.size() + plan.slots.size());
  values.insert(values.end(), plan.displacements.begin(), plan.displacements.end());
  values.insert(values.end(), plan.slots.begin(), plan.slots.end());
  lines.emplace_back(fmt::format("constexpr std::uint8_t {}[] = {};", table_name, emit_uint8_array(values)));
}

void emit_mphf8_table_arrays(const std::string &table_name,
  const Mphf8Plan &utf8_plan,
  const Mphf8Plan &utf16_plan,
  std::vector<std::string> &lines)
{
  lines.emplace_back("#ifdef JSON2CPP_USE_UTF16");
  emit_mphf8_table_array(table_name, utf16_plan, lines);
  lines.emplace_back("#else");
  emit_mphf8_table_array(table_name, utf8_plan, lines);
  lines.emplace_back("#endif");
}

std::uint64_t make_mphf_prefix_mask(const nlohmann::ordered_json &value, const bool utf16)
{
  constexpr std::size_t linear_prefix = 16;
  std::uint64_t mask = 0;
  std::size_t index = 0;
  for (auto itr = value.begin(); itr != value.end() && index < linear_prefix; ++itr, ++index) {
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
  auto [it, inserted] = ctx.mphf8_tables.try_emplace(KeyLayoutTracker::make_layout_signature(value));
  if (inserted) {
    it->second.name = fmt::format("h{}", ctx.mphf8_table_count++);
    it->second.utf8 = utf8_plan;
    it->second.utf16 = utf16_plan;
    emit_mphf8_table_arrays(it->second.name, it->second.utf8, it->second.utf16, ctx.lines);
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
  lines.emplace_back(fmt::format("extern const blob_pair_t {}[];", node_name));
  lines.emplace_back("#ifdef JSON2CPP_USE_UTF16");
  lines.emplace_back(fmt::format("constexpr mphf8_blob_object_t {}_mphf{{{} + 2, {}, {}, {}, {}, {}, 0x{:016x}ull}};",
    node_name,
    node_name,
    table.name,
    size,
    table.utf16.bucket_count,
    table.utf16.seed1,
    table.utf16.seed2,
    utf16_prefix_mask));
  lines.emplace_back("#else");
  lines.emplace_back(fmt::format("constexpr mphf8_blob_object_t {}_mphf{{{} + 2, {}, {}, {}, {}, {}, 0x{:016x}ull}};",
    node_name,
    node_name,
    table.name,
    size,
    table.utf8.bucket_count,
    table.utf8.seed1,
    table.utf8.seed2,
    utf8_prefix_mask));
  lines.emplace_back("#endif");
}

void emit_indexed_mphf8_descriptor(const std::string &node_name,
  const std::size_t size,
  const std::uint64_t utf8_prefix_mask,
  const std::uint64_t utf16_prefix_mask,
  const Mphf8TableInfo &table,
  std::vector<std::string> &lines)
{
  lines.emplace_back("#ifdef JSON2CPP_USE_UTF16");
  lines.emplace_back(fmt::format("constexpr indexed_mphf8_blob_object_t {}_mphf{{{}.entries.data(), {}_keys, s, {}.value_hashes.data(), {}.prefix_hashes.data(), {}, {}, {}, {}, {}, 0x{:016x}ull}};",
    node_name,
    node_name,
    node_name,
    node_name,
    node_name,
    table.name,
    size,
    table.utf16.bucket_count,
    table.utf16.seed1,
    table.utf16.seed2,
    utf16_prefix_mask));
  lines.emplace_back("#else");
  lines.emplace_back(fmt::format("constexpr indexed_mphf8_blob_object_t {}_mphf{{{}.entries.data(), {}_keys, s, {}.value_hashes.data(), {}.prefix_hashes.data(), {}, {}, {}, {}, {}, 0x{:016x}ull}};",
    node_name,
    node_name,
    node_name,
    node_name,
    node_name,
    table.name,
    size,
    table.utf8.bucket_count,
    table.utf8.seed1,
    table.utf8.seed2,
    utf8_prefix_mask));
  lines.emplace_back("#endif");
}

ObjectLayout choose_object_layout(const nlohmann::ordered_json &value, const EmitContext &ctx)
{
  if (!value.is_object() || value.empty()) return ObjectLayout::Regular;
  if (value.size() <= 4 && ctx.trackers.key_tracker.layout_use_count(value) >= 12) return ObjectLayout::Regular;

  const auto inline_savings = ctx.trackers.key_tracker.estimate_inline_object_savings(value);
  double value_ref_savings = 0.0;
  bool value_ref_possible = true;
  for (auto itr = value.begin(); itr != value.end(); ++itr) {
    const auto value_entry_savings = estimate_value_ref_entry_savings(itr.value(), ctx);
    if (value_entry_savings < -1.0e17) {
      value_ref_possible = false;
    } else {
      value_ref_savings += value_entry_savings;
    }
  }

  const auto blob_ref_savings = value_ref_possible && can_use_blob_keys(value)
    ? value_ref_savings + (static_cast<double>(value.size()) * 8.0) - 16.0
    : -1.0e18;
  if (blob_ref_savings >= ctx.trackers.key_tracker.min_compact_savings
      && blob_ref_savings > value_ref_savings
      && blob_ref_savings > inline_savings)
    return ObjectLayout::BlobByReference;
  if (value_ref_possible && value_ref_savings >= ctx.trackers.key_tracker.min_compact_savings
      && value_ref_savings > inline_savings)
    return ObjectLayout::ValueByReference;
  if (inline_savings >= ctx.trackers.key_tracker.min_compact_savings) return ObjectLayout::CompactInline;
  return ObjectLayout::Regular;
}

std::string emit_value_reference(const nlohmann::ordered_json &value, EmitContext &ctx)
{
  if (value.is_object() && ctx.trackers.object_tracker.is_shared(value)) {
    return fmt::format("&{}",
      ensure_emitted(ctx.trackers.object_tracker, value, ctx.lines, [&] { return emit_node_body(value, ctx); }));
  }

  if (value.is_array() && ctx.trackers.array_tracker.is_shared(value)) {
    return fmt::format("&{}",
      ensure_emitted(ctx.trackers.array_tracker, value, ctx.lines, [&] { return emit_node_body(value, ctx); }));
  }

  ctx.layout_usage.uses_scalar_pool = true;
  return fmt::format("&s[{}]", ctx.trackers.scalar_tracker.get_pool_index(value));
}

std::string make_blob_literal(const nlohmann::ordered_json &value)
{
  std::string result = "J2C(";
  for (auto itr = value.begin(); itr != value.end(); ++itr) {
    result += fmt::format("\"{}\"", escape_string(itr.key()));
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

std::string join_strings(const std::vector<std::string> &values)
{
  std::string result;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) result += ", ";
    result += values[i];
  }
  return result;
}

std::string emit_uint8_std_array(const std::vector<std::uint8_t> &values)
{
  std::vector<std::string> parts;
  parts.reserve(values.size());
  for (const auto value : values) parts.emplace_back(std::to_string(value));
  return fmt::format("std::array<std::uint8_t, {}>{{{}}}", values.size(), join_strings(parts));
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
  return fmt::format("{}({}, {}, {}, {}, {}, {}, {}, {}, {}),",
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
  auto layout = choose_object_layout(value, ctx);
  Mphf8Plan utf8_mphf, utf16_mphf;
  const bool use_mphf = layout == ObjectLayout::BlobByReference
                     && build_mphf8_plan(value, false, utf8_mphf)
                     && build_mphf8_plan(value, true, utf16_mphf);
  if (use_mphf)
    layout = can_use_indexed_mphf_values(value, ctx) ? ObjectLayout::IndexedPerfectHashBlobByReference
                                                     : ObjectLayout::PerfectHashBlobByReference;

  if (layout == ObjectLayout::CompactInline) ctx.layout_usage.uses_compact_inline = true;
  if (layout == ObjectLayout::ValueByReference) ctx.layout_usage.uses_value_ref = true;
  if (layout == ObjectLayout::BlobByReference || layout == ObjectLayout::PerfectHashBlobByReference)
    ctx.layout_usage.uses_blob_ref = true;
  if (layout == ObjectLayout::PerfectHashBlobByReference) ctx.layout_usage.uses_mphf8_blob_ref = true;
  if (layout == ObjectLayout::IndexedPerfectHashBlobByReference) {
    ctx.layout_usage.uses_indexed_mphf8_blob_ref = true;
    ctx.layout_usage.uses_scalar_pool = true;
  }

  std::vector<std::string> entries;
  entries.reserve(value.size()
                + (layout == ObjectLayout::PerfectHashBlobByReference ? 2u
                   : layout == ObjectLayout::BlobByReference           ? 1u
                                                                        : 0u));

  if (layout == ObjectLayout::BlobByReference || layout == ObjectLayout::PerfectHashBlobByReference
      || layout == ObjectLayout::IndexedPerfectHashBlobByReference) {
    ctx.lines.emplace_back(fmt::format("constexpr basicType {}_keys[] = {};", node_name, make_blob_literal(value)));
    if (layout == ObjectLayout::PerfectHashBlobByReference) {
      emit_mphf8_descriptor(node_name,
        value.size(),
        make_mphf_prefix_mask(value, false),
        make_mphf_prefix_mask(value, true),
        ensure_mphf8_table(value, utf8_mphf, utf16_mphf, ctx),
        ctx.lines);
      entries.emplace_back(fmt::format("blob_pair_t{{&{}_mphf, blob_pair_t::header_t{{}}}},", node_name));
    }
    if (layout != ObjectLayout::IndexedPerfectHashBlobByReference) {
      entries.emplace_back(fmt::format("blob_pair_t{{{}_keys, blob_pair_t::header_t{{}}}},", node_name));
    }
  }

  std::size_t key_offset = 0;
  std::size_t utf16_key_offset = 0;
  std::vector<std::string> indexed_lengths;
  std::vector<std::uint8_t> indexed_value_indices;
  if (layout == ObjectLayout::IndexedPerfectHashBlobByReference) {
    indexed_lengths.reserve(value.size());
    indexed_value_indices.reserve(value.size());
  }
  for (auto itr = value.begin(); itr != value.end(); ++itr) {
    if (layout == ObjectLayout::CompactInline) {
      const auto value_repr = emit_value(itr.value(), ctx);
      const auto key_name = ctx.trackers.key_tracker.ensure_key_definition(itr.key(), ctx.lines);
      entries.emplace_back(fmt::format("compact_pair_t{{&{}, {}}},", key_name, value_repr));
    } else if (layout == ObjectLayout::ValueByReference) {
      entries.emplace_back(
        fmt::format("ref_pair_t{{{}, {}}},", format_json_string(itr.key()), emit_value_reference(itr.value(), ctx)));
    } else if (layout == ObjectLayout::IndexedPerfectHashBlobByReference) {
      const auto utf16_key_length = utf16_length(itr.key());
      indexed_lengths.emplace_back(fmt::format("J2D({}, {})", itr.key().size(), itr.key().size() - utf16_key_length));
      indexed_value_indices.emplace_back(
        static_cast<std::uint8_t>(ctx.trackers.scalar_tracker.get_pool_index(itr.value())));
      key_offset += itr.key().size();
      utf16_key_offset += utf16_key_length;
    } else if (layout == ObjectLayout::BlobByReference || layout == ObjectLayout::PerfectHashBlobByReference) {
      entries.emplace_back(emit_blob_entry(emit_value_reference(itr.value(), ctx),
        itr.value(),
        itr.key(),
        key_offset,
        utf16_key_offset));
      const auto utf16_key_length = utf16_length(itr.key());
      key_offset += itr.key().size();
      utf16_key_offset += utf16_key_length;
    } else {
      entries.emplace_back(fmt::format("pair_t{{{}, {}}},", format_json_string(itr.key()), emit_value(itr.value(), ctx)));
    }
  }

  if (layout == ObjectLayout::IndexedPerfectHashBlobByReference) {
    ctx.lines.emplace_back(fmt::format(
      "constexpr auto {} = json2cpp::detail::make_indexed_blob_storage({}_keys, std::array<std::uint16_t, {}>{{{}}}, {}, s);",
      node_name,
      node_name,
      indexed_lengths.size(),
      join_strings(indexed_lengths),
      emit_uint8_std_array(indexed_value_indices)));
    emit_indexed_mphf8_descriptor(node_name,
      value.size(),
      make_mphf_prefix_mask(value, false),
      make_mphf_prefix_mask(value, true),
      ensure_mphf8_table(value, utf8_mphf, utf16_mphf, ctx),
      ctx.lines);
    return fmt::format("&{}_mphf", node_name);
  }

  const auto entry_type = layout == ObjectLayout::CompactInline ? "compact_pair_t"
                        : layout == ObjectLayout::ValueByReference ? "ref_pair_t"
                        : layout == ObjectLayout::BlobByReference || layout == ObjectLayout::PerfectHashBlobByReference
                        ? "blob_pair_t"
                        : "pair_t";
  ctx.lines.emplace_back(fmt::format("constexpr {} {}[] = {{", entry_type, node_name));

  for (const auto &entry : entries) { ctx.lines.emplace_back(fmt::format("  {}", entry)); }
  ctx.lines.emplace_back("};");
  if (layout == ObjectLayout::PerfectHashBlobByReference) return fmt::format("&{}_mphf", node_name);
  const auto object_type = layout == ObjectLayout::CompactInline ? "compact_object_t"
                         : layout == ObjectLayout::ValueByReference ? "ref_value_object_t"
                         : layout == ObjectLayout::BlobByReference ? "blob_object_t"
                                                                   : "object_t";
  if (layout == ObjectLayout::BlobByReference) return fmt::format("{}{{{} + 1, {}}}", object_type, node_name, value.size());
  return fmt::format("{}{{{}}}", object_type, node_name);
}

std::string emit_array(const nlohmann::ordered_json &value, EmitContext &ctx, const std::string &node_name)
{
  std::vector<std::string> entries;
  entries.reserve(value.size());
  for (const auto &child : value) { entries.emplace_back(fmt::format("{},", emit_value(child, ctx))); }

  ctx.lines.emplace_back(fmt::format("constexpr json {}[] = {{", node_name));
  for (const auto &entry : entries) { ctx.lines.emplace_back(fmt::format("  {}", entry)); }
  ctx.lines.emplace_back("};");
  return fmt::format("array_t{{{}}}", node_name);
}

std::string emit_node_body(const nlohmann::ordered_json &value, EmitContext &ctx)
{
  const std::string node_name = fmt::format("d{}", ctx.node_count++);
  if (value.is_object()) return emit_object(value, ctx, node_name);
  if (value.is_array()) return emit_array(value, ctx, node_name);
  return {};
}

std::string emit_value(const nlohmann::ordered_json &value, EmitContext &ctx)
{
  if (value.is_object() && ctx.trackers.object_tracker.is_shared(value)) {
    return ensure_emitted(ctx.trackers.object_tracker, value, ctx.lines, [&] { return emit_node_body(value, ctx); });
  }

  if (value.is_array() && ctx.trackers.array_tracker.is_shared(value)) {
    return ensure_emitted(ctx.trackers.array_tracker, value, ctx.lines, [&] { return emit_node_body(value, ctx); });
  }

  if (value.is_object() || value.is_array()) return emit_node_body(value, ctx);
  return emit_scalar_value(value);
}

TrackerSet build_trackers(const nlohmann::ordered_json &json)
{
  TrackerSet trackers;
  analyze_json(json, trackers);
  trackers.object_tracker.prepare_variables();
  trackers.array_tracker.prepare_variables();
  trackers.scalar_tracker.prepare_variables();
  return trackers;
}

compile_results compile_impl(const std::string_view original_name, const nlohmann::ordered_json &json)
{
  const std::string document_name = sanitize_identifier(original_name);
  auto trackers = build_trackers(json);
  compile_results results;

  results.hpp.emplace_back(fmt::format("#ifndef {}_COMPILED_JSON", document_name));
  results.hpp.emplace_back(fmt::format("#define {}_COMPILED_JSON", document_name));
  results.hpp.emplace_back("#include <json2cpp/json2cpp.hpp>");
  results.hpp.emplace_back(fmt::format("namespace compiled_json::{} {{", document_name));
  results.hpp.emplace_back("  const json2cpp::json &get();");
  results.hpp.emplace_back("}");
  results.hpp.emplace_back("#endif");

  EmitContext::LayoutUsage layout_usage;
  std::vector<std::string> impl_body;
  results.impl.emplace_back(fmt::format("#ifndef {}_COMPILED_JSON_IMPL", document_name));
  results.impl.emplace_back(fmt::format("#define {}_COMPILED_JSON_IMPL", document_name));
  results.impl.emplace_back("#include <json2cpp/json2cpp.hpp>");
  results.impl.emplace_back(fmt::format(R"(
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
  EmitContext ctx{ node_count, impl_body, trackers, layout_usage };
  const auto root_repr = emit_value(json, ctx);

  if (trackers.key_tracker.descriptor_count() != 0) {
    results.impl.emplace_back("  using key_descriptor_t = json2cpp::basic_key_descriptor<basicType>;");
  }
  if (layout_usage.uses_compact_inline) {
    results.impl.emplace_back("  using compact_pair_t = json2cpp::basic_compact_value_pair_t<basicType>;");
    results.impl.emplace_back("  using compact_object_t = json2cpp::basic_compact_object_t<basicType>;");
  }
  if (layout_usage.uses_value_ref) {
    results.impl.emplace_back("  using ref_pair_t = json2cpp::basic_ref_value_pair_t<basicType>;");
    results.impl.emplace_back("  using ref_value_object_t = json2cpp::basic_ref_value_object_t<basicType>;");
  }
  if (layout_usage.uses_blob_ref || layout_usage.uses_indexed_mphf8_blob_ref) {
    results.impl.emplace_back(R"(  #ifdef JSON2CPP_USE_UTF16
  #define J2C(str) u"" str
  #define J2D(utf8_size, utf16_delta) (utf8_size - utf16_delta)
  #else
  #define J2C(str) str
  #define J2D(utf8_size, utf16_delta) utf8_size
    #endif)");
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
  }
  if (layout_usage.uses_indexed_mphf8_blob_ref) {
    results.impl.emplace_back(
      "  using indexed_mphf8_blob_object_t = json2cpp::detail::basic_indexed_mphf8_blob_ref_object_t<basicType>;");
  }
  if (layout_usage.uses_scalar_pool && !trackers.scalar_tracker.pooled_values.empty()) {
    results.impl.emplace_back("  constexpr json s[] = {");
    for (const auto &value : trackers.scalar_tracker.pooled_values) {
      results.impl.emplace_back(fmt::format("    json{{{{ {} }}}},", emit_scalar_value(value)));
    }
    results.impl.emplace_back("  };");
  }
  results.impl.insert(results.impl.end(), impl_body.begin(), impl_body.end());

  results.impl.emplace_back(fmt::format(R"(
  constexpr auto document = json{{{{ {} }}}};
}}
#endif)",
    root_repr));

  spdlog::info("{} JSON nodes emitted.", node_count);
  spdlog::info("{} compact key descriptors emitted.", trackers.key_tracker.descriptor_count());
  spdlog::info("{} duplicate arrays reused (min size: {}), saving {} references.",
    trackers.array_tracker.get_reused_count(),
    trackers.array_tracker.min_size,
    trackers.array_tracker.get_total_references_saved());
  spdlog::info("{} duplicate objects reused (min size: {}), saving {} references.",
    trackers.object_tracker.get_reused_count(),
    trackers.object_tracker.min_size,
    trackers.object_tracker.get_total_references_saved());
  spdlog::info("{} duplicate scalar values reused (min references: {}), saving {} references.",
    trackers.scalar_tracker.get_reused_count(),
    trackers.scalar_tracker.min_references,
    trackers.scalar_tracker.get_total_references_saved());

  return results;
}

}

std::string compile(const nlohmann::json &value, std::size_t &obj_count, std::vector<std::string> &lines)
{
  EmitContext::LayoutUsage layout_usage;
  const nlohmann::ordered_json ordered = value;
  auto trackers = build_trackers(ordered);

  EmitContext ctx{ obj_count, lines, trackers, layout_usage };
  return emit_value(ordered, ctx);
}

compile_results compile(const std::string_view document_name, const nlohmann::json &json)
{
  return compile_impl(document_name, nlohmann::ordered_json(json));
}

compile_results compile(const std::string_view document_name, const std::filesystem::path &filename)
{
  spdlog::info("Loading file: '{}'", filename.string());
  std::ifstream input(filename);
  nlohmann::ordered_json document;
  input >> document;
  spdlog::info("File loaded");
  return compile_impl(document_name, document);
}

void write_compilation([[maybe_unused]] std::string_view document_name,
  const compile_results &results,
  const std::filesystem::path &base_output)
{
  const std::string sanitized_name = sanitize_identifier(document_name);
  const auto append_extension = [](std::filesystem::path name, std::string_view ext) { return name += ext; };
  const auto hpp_name = append_extension(base_output, ".hpp");
  const auto cpp_name = append_extension(base_output, ".cpp");
  const auto impl_name = append_extension(base_output, "_impl.hpp");

  std::ofstream hpp(hpp_name);
  for (const auto &line : results.hpp) { hpp << line << '\n'; }

  std::ofstream impl(impl_name);
  for (const auto &line : results.impl) { impl << line << '\n'; }

  std::ofstream cpp(cpp_name);
  cpp << fmt::format("#include \"{}\"\n", impl_name.filename().string());
  cpp << fmt::format(
    "namespace compiled_json::{} {{\nconst json2cpp::json &get() {{ return compiled_json::{}::impl::document; }}\n}}\n",
    sanitized_name,
    sanitized_name);
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
