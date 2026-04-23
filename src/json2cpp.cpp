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

#include "json2cpp_generator.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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

  bool is_shared(const nlohmann::ordered_json &value) const { return value_to_var.count(value) > 0; }
  bool is_processed(const std::string &var_name) const { return processed_vars.count(var_name) > 0; }
  void mark_as_processed(const std::string &var_name) { processed_vars.insert(var_name); }
  std::string get_var_name(const nlohmann::ordered_json &value) const { return value_to_var.at(value); }
  std::size_t get_reused_count() const { return value_to_var.size(); }

  int32_t get_total_references_saved() const
  {
    int32_t total = 0;
    for (const auto &[value, var_name] : value_to_var) {
      (void)var_name;
      total += counts.at(value) - 1;
    }
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

  using ReuseTrackerBase::ReuseTrackerBase;

  void track(const nlohmann::ordered_json &value)
  {
    if (value.is_string()) ++counts[value];
  }

  void prepare_variables() { prepare_reuse_variables([this](int count) { return count >= min_references; }); }

  int use_count(const nlohmann::ordered_json &value) const
  {
    if (const auto it = counts.find(value); it != counts.end()) return it->second;
    return 0;
  }
};

enum class ObjectLayout
{
  Regular,
  CompactInline,
  ValueByReference,
  BlobByReference,
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
  };

  std::size_t &node_count;
  std::vector<std::string> &lines;
  TrackerSet &trackers;
  LayoutUsage &layout_usage;
};

std::string emit_value(const nlohmann::ordered_json &value, EmitContext &ctx);
std::string emit_node_body(const nlohmann::ordered_json &value, EmitContext &ctx);

template<typename Tracker, typename ValueEmitter>
std::string ensure_emitted(Tracker &tracker,
  const nlohmann::ordered_json &value,
  std::vector<std::string> &lines,
  ValueEmitter emit_initializer)
{
  const auto var_name = tracker.get_var_name(value);
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

std::string emit_value_hash(const nlohmann::ordered_json &value)
{
  if (!value.is_string()) return "0u";
  return fmt::format("json::calc_hash({})", format_json_string(value.get<std::string>()));
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

  return fmt::format("&{}",
    ensure_emitted(ctx.trackers.scalar_tracker, value, ctx.lines, [&] { return emit_scalar_value(value); }));
}

std::string make_blob_literal(const nlohmann::ordered_json &value)
{
  std::string result;
  for (auto itr = value.begin(); itr != value.end(); ++itr) {
    result += fmt::format("RAW_CHARS(\"{}\")", escape_string(itr.key()));
  }
  return result.empty() ? "RAW_CHARS(\"\")" : result;
}

std::string emit_object(const nlohmann::ordered_json &value, EmitContext &ctx, const std::string &node_name)
{
  const auto layout = choose_object_layout(value, ctx);
  if (layout == ObjectLayout::CompactInline) ctx.layout_usage.uses_compact_inline = true;
  if (layout == ObjectLayout::ValueByReference) ctx.layout_usage.uses_value_ref = true;
  if (layout == ObjectLayout::BlobByReference) ctx.layout_usage.uses_blob_ref = true;

  std::vector<std::string> entries;
  entries.reserve(value.size() + (layout == ObjectLayout::BlobByReference ? 1u : 0u));

  if (layout == ObjectLayout::BlobByReference) {
    ctx.lines.emplace_back(fmt::format("constexpr basicType {}_keys[] = {};", node_name, make_blob_literal(value)));
    entries.emplace_back(fmt::format("blob_ref_value_pair_t{{{}_keys, blob_ref_value_pair_t::header_t{{}}}},", node_name));
  }

  std::size_t key_offset = 0;
  for (auto itr = value.begin(); itr != value.end(); ++itr) {
    if (layout == ObjectLayout::CompactInline) {
      const auto value_repr = emit_value(itr.value(), ctx);
      const auto key_name = ctx.trackers.key_tracker.ensure_key_definition(itr.key(), ctx.lines);
      entries.emplace_back(fmt::format("compact_value_pair_t{{&{}, {}}},", key_name, value_repr));
    } else if (layout == ObjectLayout::ValueByReference) {
      entries.emplace_back(
        fmt::format("ref_value_pair_t{{{}, {}}},", format_json_string(itr.key()), emit_value_reference(itr.value(), ctx)));
    } else if (layout == ObjectLayout::BlobByReference) {
      entries.emplace_back(fmt::format("blob_ref_value_pair_t{{{}, {}, {}, json::calc_hash({}), {}}},",
        emit_value_reference(itr.value(), ctx),
        key_offset,
        itr.key().size(),
        format_json_string(itr.key()),
        emit_value_hash(itr.value())));
      key_offset += itr.key().size();
    } else {
      entries.emplace_back(fmt::format("value_pair_t{{{}, {}}},", format_json_string(itr.key()), emit_value(itr.value(), ctx)));
    }
  }

  if (layout == ObjectLayout::CompactInline) {
    ctx.lines.emplace_back(fmt::format("constexpr compact_value_pair_t {}[] = {{", node_name));
  } else if (layout == ObjectLayout::ValueByReference) {
    ctx.lines.emplace_back(fmt::format("constexpr ref_value_pair_t {}[] = {{", node_name));
  } else if (layout == ObjectLayout::BlobByReference) {
    ctx.lines.emplace_back(fmt::format("constexpr blob_ref_value_pair_t {}[] = {{", node_name));
  } else {
    ctx.lines.emplace_back(fmt::format("constexpr value_pair_t {}[] = {{", node_name));
  }

  for (const auto &entry : entries) { ctx.lines.emplace_back(fmt::format("  {}", entry)); }
  ctx.lines.emplace_back("};");
  if (layout == ObjectLayout::CompactInline) return fmt::format("compact_object_t{{{}}}", node_name);
  if (layout == ObjectLayout::ValueByReference) return fmt::format("ref_value_object_t{{{}}}", node_name);
  if (layout == ObjectLayout::BlobByReference) return fmt::format("blob_ref_object_t{{{} + 1, {}}}", node_name, value.size());
  return fmt::format("object_t{{{}}}", node_name);
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
  using value_pair_t = json2cpp::basic_value_pair_t<basicType>;
  )",
    document_name));

  std::size_t node_count = 0;
  EmitContext ctx{ node_count, impl_body, trackers, layout_usage };
  const auto root_repr = emit_value(json, ctx);

  if (trackers.key_tracker.descriptor_count() != 0) {
    results.impl.emplace_back("  using key_descriptor_t = json2cpp::basic_key_descriptor<basicType>;");
  }
  if (layout_usage.uses_compact_inline) {
    results.impl.emplace_back("  using compact_value_pair_t = json2cpp::basic_compact_value_pair_t<basicType>;");
    results.impl.emplace_back("  using compact_object_t = json2cpp::basic_compact_object_t<basicType>;");
  }
  if (layout_usage.uses_value_ref) {
    results.impl.emplace_back("  using ref_value_pair_t = json2cpp::basic_ref_value_pair_t<basicType>;");
    results.impl.emplace_back("  using ref_value_object_t = json2cpp::basic_ref_value_object_t<basicType>;");
  }
  if (layout_usage.uses_blob_ref) {
    results.impl.emplace_back(R"(  #ifdef JSON2CPP_USE_UTF16
  #define RAW_CHARS(str) u"" str
  #else
  #define RAW_CHARS(str) str
    #endif)");
    results.impl.emplace_back("  using blob_ref_value_pair_t = json2cpp::basic_blob_ref_value_pair_t<basicType>;");
    results.impl.emplace_back("  using blob_ref_object_t = json2cpp::basic_blob_ref_object_t<basicType>;");
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
