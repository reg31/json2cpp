/*
MIT License

Copyright (c) 2025 Jason Turner, Regis Duflaut-Averty

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
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <nlohmann/json.hpp>
#include <ranges>
#include <set>
#include <stdexcept>
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

struct StringDuplicateTracker
{
  std::unordered_map<std::string, int> string_counts;
  std::unordered_map<std::string, std::string> string_to_var;
  std::vector<std::string> definitions;
  std::size_t counter = 0;
  std::size_t min_string_length = 4;

  void count_string(const std::string &str, bool force = false)
  {
    if (force || str.length() >= min_string_length) { string_counts[str]++; }
  }

  void generate_definitions()
  {
    for (const auto &[str, count] : string_counts) {
      if (count > 1) {
        std::string var_name = std::format("s{}", counter++);
        string_to_var[str] = var_name;
        definitions.emplace_back(std::format("constexpr auto {} = {};", var_name, format_json_string(str)));
      }
    }
    if (!definitions.empty()) definitions.emplace_back();
  }

  std::string get_string_representation(const std::string &str)
  {
    auto it = string_to_var.find(str);
    if (it != string_to_var.end()) {
      return it->second;
    } else {
      return format_json_string(str);
    }
  }

  const std::vector<std::string> &get_definitions() const { return definitions; }
  std::size_t get_reused_count() const { return string_to_var.size(); }
  int32_t get_total_references_saved() const
  {
    int32_t count = 0;
    for (const auto &[str, _] : string_to_var) { count += string_counts.at(str) - 1; }
    return count;
  }
};

struct DuplicateTracker
{
  std::unordered_map<nlohmann::ordered_json, int, JsonHasher, JsonEqual> counts;
  std::unordered_map<nlohmann::ordered_json, std::string, JsonHasher, JsonEqual> value_to_var;
  std::set<std::string> processed_vars;
  std::size_t counter = 0;
  const std::string prefix;
  std::size_t min_size = 3;

  DuplicateTracker(std::string p) : prefix(std::move(p)) {}

  void track(const nlohmann::ordered_json &value)
  {
    if (value.is_object() && value.size() >= min_size) {
      counts[value]++;
    } else if (value.is_array() && value.size() >= min_size) {
      counts[value]++;
    }
  }

  void prepare_variables()
  {
    for (const auto &[value, count] : counts) {
      if (count > 1) { value_to_var[value] = std::format("{}{}", prefix, counter++); }
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
    for (const auto &[val, var] : value_to_var) { total += counts.at(val) - 1; }
    return total;
  }
};

void analyze_for_duplicates(const nlohmann::ordered_json &value,
  StringDuplicateTracker &string_tracker,
  DuplicateTracker &object_tracker,
  DuplicateTracker &array_tracker,
  DuplicateTracker &pair_tracker)
{
  if (value.is_object()) {
    object_tracker.track(value);
    for (auto itr = value.begin(); itr != value.end(); ++itr) {
      string_tracker.count_string(itr.key(), true);
      nlohmann::ordered_json pair_rep;
      pair_rep[itr.key()] = *itr;
      pair_tracker.track(pair_rep);
      analyze_for_duplicates(*itr, string_tracker, object_tracker, array_tracker, pair_tracker);
    }
  } else if (value.is_array()) {
    array_tracker.track(value);
    for (const auto &item : value) {
      analyze_for_duplicates(item, string_tracker, object_tracker, array_tracker, pair_tracker);
    }
  } else if (value.is_string()) {
    string_tracker.count_string(value.get<std::string>());
  }
}

std::string compile_dispatch(const nlohmann::ordered_json &value,
  std::size_t &obj_count,
  std::vector<std::string> &lines,
  StringDuplicateTracker &string_tracker,
  DuplicateTracker &object_tracker,
  DuplicateTracker &array_tracker,
  DuplicateTracker &pair_tracker);

bool is_indexed_object(const nlohmann::ordered_json &value)
{ return value.is_object() && value.size() >= 64u && value.size() <= 0xFFu; }

bool is_dense_object(const nlohmann::ordered_json &value)
{
  if (!value.is_object() || value.size() < 4u) return false;
  std::size_t index = 0;
  for (auto item = value.begin(); item != value.end(); ++item, ++index)
    if (item.key() != std::to_string(index)) return false;
  return true;
}

std::string generate_node_body(const nlohmann::ordered_json &value,
  std::size_t &obj_count,
  std::vector<std::string> &lines,
  StringDuplicateTracker &string_tracker,
  DuplicateTracker &object_tracker,
  DuplicateTracker &array_tracker,
  DuplicateTracker &pair_tracker)
{
  const auto current_object_number = obj_count++;
  const std::string obj_name = std::format("d{}", current_object_number);

  if (value.is_object()) {
    std::vector<std::string> pairs;
    for (auto itr = value.begin(); itr != value.end(); ++itr) {
      nlohmann::ordered_json pair_rep;
      pair_rep[itr.key()] = *itr;

      if (pair_tracker.is_shared(pair_rep)) {
        auto var_name = pair_tracker.get_var_name(pair_rep);
        if (!pair_tracker.is_processed(var_name)) {
          pair_tracker.mark_as_processed(var_name);
          const auto key_repr = string_tracker.get_string_representation(itr.key());
          const auto val_repr =
            compile_dispatch(*itr, obj_count, lines, string_tracker, object_tracker, array_tracker, pair_tracker);
          lines.emplace_back(std::format("constexpr auto {} = value_pair_t{{{}, {}}};", var_name, key_repr, val_repr));
        }
        pairs.emplace_back(std::format("{},", var_name));
      } else {
        const auto key_repr = string_tracker.get_string_representation(itr.key());
        pairs.emplace_back(std::format("{{{}, {}}},",
          key_repr,
          compile_dispatch(*itr, obj_count, lines, string_tracker, object_tracker, array_tracker, pair_tracker)));
      }
    }

    lines.emplace_back(std::format("constexpr std::array<value_pair_t, {}> {} = {{{{", pairs.size(), obj_name));
    for (const auto &pair : pairs) { lines.emplace_back(std::format("  {}", pair)); }
    lines.emplace_back("}};");
    if (is_dense_object(value))
      return std::format("dense_object_t{{object_t{{{}}}}}", obj_name);
    if (is_indexed_object(value)) {
      lines.emplace_back(std::format(
        "constexpr auto {}_key_index = json2cpp::detail::make_key_index({}, 8);", obj_name, obj_name));
      lines.emplace_back(std::format(
        "constexpr auto {}_value_hashes = json2cpp::detail::make_value_hashes({});", obj_name, obj_name));
      lines.emplace_back(std::format(
        "constexpr auto {}_indexed = json2cpp::detail::make_indexed_entries({}, {}_key_index, {}_value_hashes);",
        obj_name, obj_name, obj_name, obj_name));
      return std::format("indexed_object_ref_t{{{}_indexed.data() + 1u, {}}}", obj_name, value.size());
    }
    return std::format("object_t{{{}}}", obj_name);

  } else if (value.is_array()) {
    std::vector<std::string> entries;
    for (const auto &child : value) {
      entries.emplace_back(std::format("{{{}}},",
        compile_dispatch(child, obj_count, lines, string_tracker, object_tracker, array_tracker, pair_tracker)));
    }

    lines.emplace_back(std::format("constexpr std::array<json, {}> {} = {{{{", entries.size(), obj_name));
    for (const auto &entry : entries) { lines.emplace_back(std::format("  {}", entry)); }
    lines.emplace_back("}};");
    return std::format("array_t{{{}}}", obj_name);
  }

  return "";
}


std::string compile_dispatch(const nlohmann::ordered_json &value,
  std::size_t &obj_count,
  std::vector<std::string> &lines,
  StringDuplicateTracker &string_tracker,
  DuplicateTracker &object_tracker,
  DuplicateTracker &array_tracker,
  DuplicateTracker &pair_tracker)
{
  if (value.is_object() && object_tracker.is_shared(value)) {
    auto var_name = object_tracker.get_var_name(value);
    if (object_tracker.is_processed(var_name)) { return var_name; }
    object_tracker.mark_as_processed(var_name);
    auto body =
      generate_node_body(value, obj_count, lines, string_tracker, object_tracker, array_tracker, pair_tracker);
    lines.emplace_back(std::format("constexpr auto {} = json{{{{ {} }}}};", var_name, body));
    return var_name;
  }

  if (value.is_array() && array_tracker.is_shared(value)) {
    auto var_name = array_tracker.get_var_name(value);
    if (array_tracker.is_processed(var_name)) { return var_name; }
    array_tracker.mark_as_processed(var_name);
    auto body =
      generate_node_body(value, obj_count, lines, string_tracker, object_tracker, array_tracker, pair_tracker);
    lines.emplace_back(std::format("constexpr auto {} = json{{{{ {} }}}};", var_name, body));
    return var_name;
  }

  if (value.is_object() || value.is_array()) {
    return generate_node_body(value, obj_count, lines, string_tracker, object_tracker, array_tracker, pair_tracker);
  } else if (value.is_number_float()) {
    return std::format("double{{{}}}", value.get<double>());
  } else if (value.is_number_unsigned()) {
    return std::format("std::uint64_t{{{}}}", value.get<std::uint64_t>());
  } else if (value.is_number()) {
    return std::format("std::int64_t{{{}}}", value.get<std::int64_t>());
  } else if (value.is_boolean()) {
    return std::format("bool{{{}}}", value.get<bool>());
  } else if (value.is_string()) {
    return string_tracker.get_string_representation(value.get<std::string>());
  } else if (value.is_null()) {
    return "std::nullptr_t{}";
  }

  return "unhandled";
}

compile_results compile_impl(const std::string_view original_name, const nlohmann::ordered_json &json)
{
  const std::string document_name = sanitize_identifier(original_name);
  StringDuplicateTracker string_tracker;
  DuplicateTracker object_tracker("o");
  DuplicateTracker array_tracker("a");
  DuplicateTracker pair_tracker("p");
  compile_results results;

  analyze_for_duplicates(json, string_tracker, object_tracker, array_tracker, pair_tracker);
  string_tracker.generate_definitions();
  object_tracker.prepare_variables();
  array_tracker.prepare_variables();
  pair_tracker.prepare_variables();

  results.hpp.emplace_back(std::format("#ifndef {}_COMPILED_JSON", document_name));
  results.hpp.emplace_back(std::format("#define {}_COMPILED_JSON", document_name));
  results.hpp.emplace_back("#include <json2cpp/json2cpp.hpp>");
  results.hpp.emplace_back(std::format("namespace compiled_json::{} {{", document_name));
  results.hpp.emplace_back("  const json2cpp::json &get();");
  results.hpp.emplace_back("}");
  results.hpp.emplace_back("#endif");

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
  using dense_object_t = json2cpp::basic_dense_object_t<basicType>;
  using indexed_object_ref_t = json2cpp::basic_indexed_object_ref_t<basicType>;
  using value_pair_t = json2cpp::basic_value_pair_t<basicType>;
  )",
    document_name));

  const auto &string_defs = string_tracker.get_definitions();
  results.impl.insert(results.impl.end(), string_defs.begin(), string_defs.end());

  std::size_t obj_count = 0;
  const auto last_obj_name =
    compile_dispatch(json, obj_count, results.impl, string_tracker, object_tracker, array_tracker, pair_tracker);

  results.impl.emplace_back(std::format(R"(
  constexpr auto document = json{{{{ {} }}}};
  #undef RAW_PREFIX
}}
#endif)",
    last_obj_name));

  std::cout << obj_count << " JSON nodes emitted.\n";
  const auto report_reuse = [](std::string_view kind, std::string_view threshold,
                               const auto &tracker, auto minimum) {
    if (const auto reused = tracker.get_reused_count(); reused != 0u)
      std::cout << reused << " duplicate " << kind << " reused (" << threshold << ": " << minimum
                << "), saving " << tracker.get_total_references_saved() << " references.\n";
  };
  report_reuse("strings", "min length", string_tracker, string_tracker.min_string_length);
  report_reuse("arrays", "min size", array_tracker, array_tracker.min_size);
  report_reuse("objects", "min size", object_tracker, object_tracker.min_size);
  report_reuse("key-value pairs", "min size", pair_tracker, pair_tracker.min_size);

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
