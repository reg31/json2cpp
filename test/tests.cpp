#include "test_json.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Can read object size")
{
  const auto &document = compiled_json::test_json::get();

  REQUIRE(document.size() == 1);
}


TEST_CASE("Can iterate object")
{
  const auto &document = compiled_json::test_json::get();

  std::size_t elements = 0;
  for (const auto &[key, value] : document.items()) {
    REQUIRE(key.is_string());
    REQUIRE(!value.is_null());
    ++elements;
  }

  REQUIRE(elements == document.size());
}

TEST_CASE("Long strings compare by content at runtime")
{
  const auto &title = compiled_json::test_json::get()["glossary"]["title"];
  REQUIRE(title == "example glossary");
  REQUIRE_FALSE(title == "example glossarz");
  REQUIRE(compiled_json::test_json::get()["glossary"].contains(std::string{ "GlossDiv" }));
}

TEST_CASE("Can read iterator key")
{
  const auto &document = compiled_json::test_json::get();
  REQUIRE(document.items().front().first == "glossary");
}
