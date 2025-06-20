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

#include "json2cpp.hpp"
#include <algorithm>
#include <fstream>
#include <unordered_map>

std::string format_json_string(const std::string &str)
{
  bool needs_raw_string = str.find('"') != std::string::npos || str.find('\\') != std::string::npos
                          || str.find('\n') != std::string::npos || str.find('\r') != std::string::npos
                          || str.find('\t') != std::string::npos;

  if (needs_raw_string) {
    return fmt::format("RAW_PREFIX(R\"string({})string\")", str);
  } else {
    return fmt::format("RAW_PREFIX(\"{}\")", str);
  }
}

struct StringDuplicateTracker
{
  std::unordered_map<std::string, int> string_counts;
  std::unordered_map<std::string, std::string> string_to_var;
  std::vector<std::string> definitions;
  std::size_t counter = 0;

  void count_string(const std::string &str) { string_counts[str]++; }

  void generate_definitions()
  {
    for (const auto &[str, count] : string_counts) {
      if (count > 1) {
        std::string var_name = fmt::format("shared_str_{}", counter++);
        string_to_var[str] = var_name;
        definitions.emplace_back(fmt::format("inline constexpr auto {} = {};", var_name, format_json_string(str)));
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

void count_strings(const nlohmann::ordered_json &value, StringDuplicateTracker &string_tracker)
{
  if (value.is_object()) {
    for (auto itr = value.begin(); itr != value.end(); ++itr) {
      string_tracker.count_string(itr.key());
      count_strings(*itr, string_tracker);
    }
  } else if (value.is_array()) {
    for (const auto &item : value) { count_strings(item, string_tracker); }
  } else if (value.is_string()) {
    string_tracker.count_string(value.get<std::string>());
  }
}

std::string compile(const nlohmann::ordered_json &value,
  std::size_t &obj_count,
  std::vector<std::string> &lines,
  StringDuplicateTracker &string_tracker)
{
  const auto current_object_number = obj_count++;

  if (value.is_object()) {
    std::vector<std::string> pairs;
    for (auto itr = value.begin(); itr != value.end(); ++itr) {
      const auto key_repr = string_tracker.get_string_representation(itr.key());
      pairs.emplace_back(
        fmt::format("value_pair_t{{{}, {}}},", key_repr, compile(*itr, obj_count, lines, string_tracker)));
    }

    lines.emplace_back(fmt::format(
      "inline constexpr std::array<value_pair_t, {}> object_data_{} = {{", pairs.size(), current_object_number));

    std::transform(pairs.begin(), pairs.end(), std::back_inserter(lines), [](const auto &pair) {
      return fmt::format("  {}", pair);
    });

    lines.emplace_back("};");

    return fmt::format("object_t{{object_data_{}}}", current_object_number);
  } else if (value.is_array()) {
    std::vector<std::string> entries;
    std::transform(value.begin(), value.end(), std::back_inserter(entries), [&](const auto &child) {
      return fmt::format("{{{}}},", compile(child, obj_count, lines, string_tracker));
    });

    lines.emplace_back(fmt::format(
      "inline constexpr std::array<json, {}> object_data_{} = {{{{", entries.size(), current_object_number));

    std::transform(entries.begin(), entries.end(), std::back_inserter(lines), [](const auto &entry) {
      return fmt::format("  {}", entry);
    });

    lines.emplace_back("}};");
    return fmt::format("array_t{{object_data_{}}}", current_object_number);

  } else if (value.is_number_float()) {
    return fmt::format("double{{{}}}", value.get<double>());
  } else if (value.is_number_unsigned()) {
    return fmt::format("std::uint64_t{{{}}}", value.get<std::uint64_t>());
  } else if (value.is_number() && !value.is_number_unsigned()) {
    return fmt::format("std::int64_t{{{}}}", value.get<std::int64_t>());
  } else if (value.is_boolean()) {
    return fmt::format("bool{{{}}}", value.get<bool>());
  } else if (value.is_string()) {
    const auto value_repr = string_tracker.get_string_representation(value.get<std::string>());
    return value_repr;
  } else if (value.is_null()) {
    return "std::nullptr_t{}";
  }

  return "unhandled";
}

compile_results compile(const std::string_view document_name, const nlohmann::ordered_json &json)
{
  StringDuplicateTracker string_tracker;
  compile_results results;

  results.hpp.emplace_back(fmt::format("#ifndef {}_COMPILED_JSON", document_name));
  results.hpp.emplace_back(fmt::format("#define {}_COMPILED_JSON", document_name));
  results.hpp.emplace_back("#include <json2cpp/json2cpp.hpp>");

  results.hpp.emplace_back("using namespace std::literals::string_view_literals;");
  results.hpp.emplace_back(fmt::format("namespace compiled_json::{} {{", document_name));
  results.hpp.emplace_back(fmt::format("  const json2cpp::json &get();", document_name));
  results.hpp.emplace_back("}");

  results.hpp.emplace_back("#endif");

  results.impl.emplace_back(fmt::format(
    "// Just in case the user wants to use the entire document in a constexpr context, it can be included safely"));
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

  count_strings(json, string_tracker);
  string_tracker.generate_definitions();
  const auto &string_defs = string_tracker.get_definitions();
  results.impl.insert(results.impl.end(), string_defs.begin(), string_defs.end());

  std::size_t obj_count = 0;
  const auto last_obj_name = compile(json, obj_count, results.impl, string_tracker);

  results.impl.emplace_back(fmt::format(R"(
  inline constexpr auto document = json{{{{{}}}}};

}}

#endif)",
    last_obj_name));

  spdlog::info("{} JSON objects processed.", obj_count);
  spdlog::info("{} duplicate strings reused, saving {} string definitions.",
    string_tracker.get_reused_count(),
    string_tracker.get_total_references_saved());
  return results;
}

compile_results compile(const std::string_view document_name, const std::filesystem::path &filename)
{
  spdlog::info("Loading file: '{}'", filename.string());

  std::ifstream input(filename);
  nlohmann::ordered_json document;
  input >> document;

  spdlog::info("File loaded");

  return compile(document_name, document);
}

void write_compilation([[maybe_unused]] std::string_view document_name,
  const compile_results &results,
  const std::filesystem::path &base_output)
{
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
    document_name,
    document_name);
}

void compile_to(const std::string_view document_name,
  const nlohmann::ordered_json &json,
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
