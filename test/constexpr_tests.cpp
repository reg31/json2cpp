#include "edge_values_impl.hpp"
#include "layouts_impl.hpp"
#include "test_json_impl.hpp"
#include <algorithm>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <iterator>
#include <limits>
#include <ranges>
#include <string_view>

namespace {
constexpr const auto &layouts = compiled_json::layouts::impl::document;
}

TEST_CASE("Indexed objects find every key, 8-bit and 16-bit hash slots")
{
  STATIC_REQUIRE((layouts["wide"].size() == 300 && layouts["narrow"].size() == 100));
  STATIC_REQUIRE([] {
    for (int i = 0; i < 300; ++i) {
      const char key[] = { 'w',
        static_cast<char>('0' + i / 100),
        static_cast<char>('0' + i / 10 % 10),
        static_cast<char>('0' + i % 10),
        '\0' };
      if (layouts["wide"].at(std::string_view{ key }) != i) return false;
    }
    return true;
  }());
  STATIC_REQUIRE((layouts["narrow"]["n99"] == 99 && layouts["narrow"]["n00"] == 0));
  STATIC_REQUIRE((!layouts["wide"].contains("w300") && !layouts["narrow"].contains("n100")));
  STATIC_REQUIRE(layouts["wide"][299] == 299);
}

TEST_CASE("Deduplicated, dense and empty objects keep their own values")
{
  STATIC_REQUIRE((layouts["points"][0]["x"] == 1 && layouts["points"][2]["y"] == 6));
  STATIC_REQUIRE(
    (layouts["points"][1].items().front().first == "x" && layouts["points"][1].items().back().second == 4));
  STATIC_REQUIRE((layouts["dup_a"] == layouts["dup_b"] && !(layouts["dup_a"] == layouts["points"][1])));
  STATIC_REQUIRE((layouts["unique"]["once"] == true && layouts["unique"]["only"] == "here"));
  STATIC_REQUIRE((layouts["dense"]["3"] == "d" && !layouts["dense"].contains("03")));
  STATIC_REQUIRE((layouts["empty"].items().empty() && !layouts["empty"].contains("x") && layouts["empty_arr"].empty()));
}

TEST_CASE("find_entry, index and items() keep their public types")
{
  STATIC_REQUIRE(std::same_as<decltype(layouts.find_entry("x")), const json2cpp::value_pair_t *>);
  STATIC_REQUIRE(std::same_as<decltype(layouts.items()), json2cpp::object_t>);
  STATIC_REQUIRE(std::same_as<json2cpp::items_t, json2cpp::object_t>);
  constexpr const json2cpp::value_pair_t *entry = layouts["unique"].find_entry("only");
  STATIC_REQUIRE((entry && entry->first == "only" && entry->second == "here"));
  STATIC_REQUIRE(layouts["unique"].find_entry("missing") == nullptr);
  STATIC_REQUIRE(layouts["unique"].index(layouts["unique"]["once"]) == 1);
  STATIC_REQUIRE(layouts["unique"].index("here") == 0);
  STATIC_REQUIRE((std::forward_iterator<json2cpp::items_t::iterator> && std::ranges::forward_range<json2cpp::items_t>));
  STATIC_REQUIRE([] {
    const auto items = layouts["narrow"].items();
    const auto found = std::ranges::find_if(items, [](const auto &item) { return item.second == 42; });
    return found != items.end() && (*found).first == "n42";
  }());
}

TEST_CASE("is_sorted_obj() keeps v5's meaning")
{
  STATIC_REQUIRE(layouts["dense"].is_sorted_obj());
  STATIC_REQUIRE(layouts["points"][0].is_sorted_obj());
  STATIC_REQUIRE(!layouts["unique"].is_sorted_obj());
  STATIC_REQUIRE(!layouts["wide"].is_sorted_obj());
}

TEST_CASE("Hand-built objects and arrays accept every construction form")
{
  static constexpr json2cpp::value_pair_t members[] = { { "a", 1 }, { "b", "two" } };
  static constexpr std::array<json2cpp::json, 2> elements{ { 1, 2 } };
  constexpr json2cpp::json object{ json2cpp::object_t{ members } };
  constexpr json2cpp::json braced{ { json2cpp::object_t{ members } } };
  constexpr json2cpp::json array_copy_init = json2cpp::array_t{ elements };
  STATIC_REQUIRE((braced == object && array_copy_init.size() == 2 && array_copy_init[1] == 2));
  STATIC_REQUIRE((object["b"] == "two" && object[0] == 1 && object.size() == 2 && !object.contains("c")));
  STATIC_REQUIRE((object.items().front().first == "a" && object.index(object["b"]) == 1));
  STATIC_REQUIRE(object.find_entry("a")->second == 1);
}


TEST_CASE("Can read object size")
{
  constexpr auto &document = compiled_json::test_json::impl::document;// NOLINT No, I'm not going to mark this `const`

  STATIC_REQUIRE(document.size() == 1);
}

constexpr auto count_elements()
{
  constexpr auto &document = compiled_json::test_json::impl::document;// NOLINT No, I'm not going to mark this `const`

  std::size_t elements = 0;
  for (const auto &[key, value] : document.items()) {
    // count_if is not constexpr in C++17
    // cppcheck-suppress useStlAlgorithm
    if (key.is_string() && !value.is_null()) { ++elements; }
  }

  return elements;
}

TEST_CASE("Can iterate object") { STATIC_REQUIRE(count_elements() == compiled_json::test_json::impl::document.size()); }

TEST_CASE("Can read iterator key")
{
  constexpr auto &document = compiled_json::test_json::impl::document;// NOLINT No, I'm not going to mark this `const`

  STATIC_REQUIRE(document.items().front().first == "glossary");
}

TEST_CASE("Padded char buffer keys stop at the first NUL")
{
  STATIC_REQUIRE([] {
    char key[16] = "glossary";
    return compiled_json::test_json::impl::document.contains(key)
           && !compiled_json::test_json::impl::document[key].is_null();
  }());
}

TEST_CASE("Generator keeps edge values exact")
{
  constexpr const auto &document = compiled_json::edge_values::impl::document;

  STATIC_REQUIRE(std::bit_cast<std::uint64_t>(document["neg_zero"].get<double>()) == 0x8000000000000000u);
  STATIC_REQUIRE(document["int_min"].get<std::int64_t>() == std::numeric_limits<std::int64_t>::min());
  STATIC_REQUIRE(document["uint_max"].get<std::uint64_t>() == std::numeric_limits<std::uint64_t>::max());
  STATIC_REQUIRE(document["trigraphs"] == "?\?=?\?/");
}
