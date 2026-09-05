# json2cpp

[![CI](https://github.com/reg31/json2cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/reg31/json2cpp/actions/workflows/ci.yml)

`json2cpp` compiles JSON documents into immutable C++23 data. Generated documents
require no parsing, filesystem access, heap allocation, or runtime initialization.
They expose a small, read-only JSON API that works at runtime and during constant
evaluation.

## Highlights

- A `json2cpp::json` value occupies 16 bytes on 64-bit targets.
- Strings, arrays, objects, keys, and values are non-owning and allocation-free.
- Objects use contiguous 32-byte key/value pairs, so `items()` is a zero-copy
  random-access span.
- String literals passed to `at()`, `operator[]`, and `contains()` are prehashed with
  `consteval`.
- Dense objects keyed `"0"`, `"1"`, ... use direct numeric lookup.
- Objects with 64 to 255 keys receive a compile-time byte-sized side index while
  preserving their contiguous pair storage and source order.
- Naturally sorted regular objects use binary search; other regular objects use
  hash-first linear search.
- UTF-8 (`char`) is the default; UTF-16 (`char16_t`) is optional.
- A generated `.cpp` firewall keeps large resources out of consumer translation units.

The library intentionally implements a focused subset of the nlohmann JSON API. It
does not parse, mutate, allocate, or own JSON data at runtime.

## Requirements

- A C++23 compiler.
- CMake 3.28 or newer when building the generator.

Object, array, and string lengths are stored as 32-bit values. Generated documents
must therefore contain fewer than `UINT32_MAX` elements in any single container or
string. The byte side index is used only when an object has at most 255 keys; larger
objects automatically remain regular objects.

## Installation

Applications need the generator at build time and `json2cpp.hpp` at compile time.
There is no runtime library to link.

### Download a release

Download and extract the package for your platform from
[GitHub Releases](https://github.com/reg31/json2cpp/releases). It contains the
`json2cpp` executable, `json2cpp.hpp`, and the optional ValiJSON adapter.

Add the generator to `PATH`, or invoke it using its full path.

### Build with CMake

```console
git clone https://github.com/reg31/json2cpp.git
cmake -S json2cpp -B json2cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build json2cpp/build --config Release --target json2cpp
ctest --test-dir json2cpp/build --output-on-failure
```

### Add the header to an application

Preserve the include path expected by generated files:

```text
your-project/
  include/
    json2cpp/
      json2cpp.hpp
  generated/
    config.hpp
    config.cpp
    config_impl.hpp
```

```cmake
target_sources(my_app PRIVATE generated/config.cpp)
target_include_directories(my_app PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/generated"
)
target_compile_features(my_app PRIVATE cxx_std_23)
```

Use the header and generator from the same release. Regenerate documents after an
incompatible storage-layout update.

### ValiJSON adapter

Consumers that use ValiJSON can include `json2cpp_adapter.hpp`:

```cpp
#include <json2cpp/json2cpp_adapter.hpp>

const valijson::adapters::json2cppJsonAdapter adapter{
  compiled_json::config::get()
};
```

ValiJSON is not required by other consumers.

## Generate a document

Given `config.json`:

```json
{
  "name": "Melo",
  "enabled": true,
  "retries": 3,
  "threshold": 0.75,
  "tags": ["speech", "translation"],
  "settings": {
    "theme": "dark",
    "locale": "en"
  }
}
```

Run:

```console
json2cpp config config.json generated/config
```

The arguments are the generated namespace name, input file, and output base path. The
command creates `config.hpp`, `config.cpp`, and `config_impl.hpp`. Compile `config.cpp`
once and include `config.hpp` wherever the document is used:

```cpp
#include "generated/config.hpp"

const json2cpp::json &config = compiled_json::config::get();
```

The accessor returns immutable static data and performs no runtime loading.

## Quick start

```cpp
#include "generated/config.hpp"

#include <string_view>

const auto &config = compiled_json::config::get();

const auto name = config.at("name").get<std::string_view>();
const bool enabled = config["enabled"].get<bool>();
const int retries = config["retries"].get<int>();

for (const auto &tag : config["tags"]) {
  // tag is const json2cpp::json&
}

for (const auto &[key, value] : config.items()) {
  const std::string_view key_text = key.getString();
  (void)key_text;
  (void)value;
}
```

## V5 breaking changes

V5 replaces the multi-layout proxy architecture with contiguous stored pairs and
optional side indexes. Public method names and behavior remain the same, but two exact
return types changed:

- `find_entry()` returns `const json2cpp::value_pair_t *`.
- `items()` returns `json2cpp::items_t`, an alias of the contiguous `object_t` span.

Migration for `find_entry()`:

```cpp
// Previous proxy result
const auto entry = config.find_entry("name");
const auto value = entry->second->get<std::string_view>();

// V5 stored-pair pointer
const json2cpp::value_pair_t *entry = config.find_entry("name");
const auto value = entry->second.get<std::string_view>();
```

Structured binding and ranges code over `items()` remains unchanged:

```cpp
for (const auto &[key, value] : config.items()) {
  // key and value are const json2cpp::json&
}
```

Generated files from earlier releases must be regenerated.

## API reference

### Types

| Alias | Meaning |
|---|---|
| `json2cpp::json` | `basic_json<char>` or `basic_json<char16_t>`. |
| `json2cpp::array_t` | `std::span<const json>` used to construct arrays. |
| `json2cpp::object_t` | `std::span<const value_pair_t>` used to construct objects. |
| `json2cpp::value_pair_t` | A stored `{json key, json value}` pair. |
| `json2cpp::items_t` | Contiguous object range returned by `items()`. |
| `json2cpp::entry_view_t` | Compatibility alias for `const value_pair_t *`. |

`dense_object_t`, `indexed_object_t`, and `indexed_object_ref_t` are generator-facing
storage helpers. Application code should normally use `json`, `array_t`, `object_t`,
and generated accessors.

### Type and state

`json::Type` contains `Null`, `Boolean`, `String`, `Integer`, `UInteger`, `Float`,
`Array`, and `Object`.

```cpp
const auto &name = config["name"];

const json2cpp::json::Type type = name.type();
const std::size_t length = name.size();
const bool no_elements = name.empty();

const bool object = config.is_object();
const bool array = config["tags"].is_array();
const bool string = name.is_string();
const bool boolean = config["enabled"].is_boolean();
const bool number = config["retries"].is_number();
const bool null = json2cpp::json{}.is_null();
```

| Method | Behavior |
|---|---|
| `type()` | Returns the exact `json::Type`. |
| `size()` | Returns string length or array/object element count; other scalars use `0`. |
| `empty()` | Equivalent to `size() == 0`. |
| `is_object()` | Tests for `Type::Object`. |
| `is_array()` | Tests for `Type::Array`. |
| `is_string()` | Tests for `Type::String`. |
| `is_boolean()` | Tests for `Type::Boolean`. |
| `is_number()` | Tests for any numeric type. |
| `is_null()` | Tests for `Type::Null`. |
| `is_sorted_obj()` | Reports whether a regular object's keys are lexically sorted. |
| `hash()` | Returns a string or key's cached hash. |

### Object lookup

> [!TIP]
> Prefer `at("name")`, `config["name"]`, and `contains("name")` when the key is a
> literal. These overloads guarantee compile-time hashing. `"name"sv` selects the
> `std::string_view` overload; a compiler may fold its hash, but the API does not
> require that optimization.

#### `at()` and `operator[]`

Both return `const json &` and perform checked access. `operator[]` forwards to
`at()`.

```cpp
const auto &name = config.at("name");
const auto &theme = config["settings"]["theme"];

const std::string_view runtime_key = "settings";
const auto &settings = config.at(runtime_key);

const auto &first_tag = config["tags"].at(0);
const auto &first_value = config.at(0); // Object values use stored order.
```

Named mutable character arrays should be passed as `std::string_view`; their contents
cannot be prehashed at compile time.

#### `contains()`

`contains()` checks object keys without throwing and returns `false` for non-objects:

```cpp
if (config.contains("threshold")) {
  const double threshold = config["threshold"].get<double>();
}

const std::string_view missing = "missing";
const bool found = config.contains(missing); // false

const std::string key = "threshold";
const bool owned_key_found = config.contains(key);
const bool pointer_key_found = config.contains(key.c_str());
```

#### `find_entry()`

`find_entry()` returns a pointer to the stored pair or `nullptr`:

```cpp
if (const auto *entry = config.find_entry("name")) {
  const std::string_view key = entry->first.getString();
  const std::string_view value = entry->second.get<std::string_view>();
}
```

Use the prehashed overload when repeatedly searching with a runtime key:

```cpp
const std::string_view key = "name";
const std::uint32_t hash = json2cpp::json::calc_hash(key);
const auto *entry = config.find_entry(key, hash);
```

The supplied hash must have been calculated from the same key.

Generated lookup tables omit keys already covered by the fast prefix scan.
Regenerate your JSON sources with the updated generator and header to use this
optimization; existing generated sources remain compatible with the new header.

#### `calc_hash()` and `null_value()`

```cpp
const auto hash = json2cpp::json::calc_hash("name");
const json2cpp::json &null = json2cpp::json::null_value();
```

`calc_hash()` uses the same hash as generated keys. `null_value()` returns a shared,
immutable null value.

### Find a value index

`index(value)` searches array elements or object values in stored order; it does not
search object keys. It returns `json::npos` when no value compares equal.

```cpp
const auto tag_index = config["tags"].index("translation");
const auto theme_index = config["settings"].index("dark");

const auto &settings = config["settings"];
const auto locale_index = settings.index(settings["locale"]);

const auto retry_index = config.index(3);
const auto enabled_index = config.index(true);
```

String searches use cached value fingerprints on indexed objects. A `json` reference
to a stored element returns that element's own index, even if an earlier value compares
equal. A detached value returns the first equal index. This behavior is the same at
runtime and during constant evaluation.

### Object iteration with `items()`

`items()` returns an allocation-free, contiguous random-access span:

```cpp
const auto items = config.items();

for (const auto &[key, value] : items) {
  const std::string_view key_text = key.getString();
  const std::uint32_t key_hash = key.hash();
  (void)key_text;
  (void)key_hash;
  (void)value;
}
```

It works directly with ranges algorithms:

```cpp
#include <algorithm>
#include <ranges>

const auto items = config.items();
const auto found = std::ranges::find_if(items, [](const auto &item) {
  return item.second == "Melo";
});
```

`items_t` provides `size()`, `empty()`, `front()`, `back()`, indexing, `begin()`, and
`end()` through `std::span`. Calling `items()` on a non-object throws
`std::domain_error`.

### Array iteration and spans

Arrays expose pointer iterators and convert to `std::span<const json>`:

```cpp
const auto &tags = config["tags"];

for (const json2cpp::json &tag : tags) {
  const auto text = tag.get<std::string_view>();
  (void)text;
}

const json2cpp::json *first = tags.begin();
const json2cpp::json *last = tags.end();
const std::span<const json2cpp::json> span = tags;
```

For non-arrays, `begin()` and `end()` return `nullptr`, and span conversion returns an
empty span.

### Read scalar values

`get<T>()` supports `std::string_view`, `bool`, integral types, and floating-point
types:

```cpp
const auto name = config["name"].get<std::string_view>();
const bool enabled = config["enabled"].get<bool>();
const int retries = config["retries"].get<int>();
const float threshold = config["threshold"].get<float>();
```

Numeric conversions use normal C++ casts and do not add range or precision checks.
Unsupported types fail compilation.

Low-level accessors are also available:

```cpp
const auto &name_value = config["name"];
const std::string_view name = name_value.getString();
const char *characters = name_value.data();
const double retries = config["retries"].getNumber();
```

`getString()` and `data()` are unchecked; `data()` is not guaranteed to be
null-terminated. `getNumber()` accepts any numeric JSON type.

### Equality

JSON values compare recursively and allocation-free:

```cpp
const bool same_document = config == config;
const bool same_settings = config["settings"] == config["settings"];
const bool same_tags = config["tags"] == config["tags"];

const bool name_matches = config["name"] == "Melo";
const bool enabled_matches = config["enabled"] == true;
const bool retries_match = config["retries"] == 3;
```

Object equality is order-sensitive because source property order is preserved.
Floating-point equality is exact. Dense objects compare values without rechecking
their implied numeric keys.

### Construct values manually

`json` is non-owning. Array and object backing storage must outlive the JSON value;
`constexpr` or static storage is the normal choice.

```cpp
using json2cpp::array_t;
using json2cpp::json;
using json2cpp::object_t;
using json2cpp::value_pair_t;

constexpr json array_values[] = {1, 2, 3};
constexpr json numbers{array_t{array_values}};

constexpr value_pair_t object_values[] = {
  {"name", "Melo"},
  {"enabled", true},
};
constexpr json object{object_t{object_values}};

constexpr json null_default{};
constexpr json null_explicit{nullptr};
constexpr json boolean{true};
constexpr json signed_integer{-1};
constexpr json unsigned_integer{1u};
constexpr json floating_point{0.75};
constexpr json string_literal{"Melo"};
```

Constructing an object with a non-string key throws `std::domain_error` or fails
constant evaluation.

## Error behavior

| Operation | Error |
|---|---|
| `at(key)` with a missing key | `std::out_of_range` |
| `at(index)` outside an array/object | `std::out_of_range` |
| `at(index)` on a scalar | `std::domain_error` |
| `items()` on a non-object | `std::domain_error` |
| `get<T>()` with the wrong JSON type | `std::domain_error` |
| `getNumber()` on a non-number | `std::domain_error` |
| Object construction with a non-string key | `std::domain_error` |

During constant evaluation, invalid operations fail compilation.

## UTF-16

Define `JSON2CPP_USE_UTF16` consistently before including generated headers:

```cpp
#define JSON2CPP_USE_UTF16
#include "generated/config.hpp"

const auto &config = compiled_json::config::get();
const std::u16string_view name = config.at(u"name").get<std::u16string_view>();
```

Do not mix UTF-8 and UTF-16 modes across translation units.

## Constexpr use

The generated `.cpp` owns a `constexpr` document internally. Manually constructed
documents can use the complete API in constant evaluation:

```cpp
constexpr json2cpp::value_pair_t values[] = {
  {"enabled", true},
  {"retries", 3},
};
constexpr json2cpp::json document{json2cpp::object_t{values}};

static_assert(document.contains("enabled"));
static_assert(document["enabled"].get<bool>());
static_assert(document["retries"] == 3);
```

## Performance

The project benchmark compares every public read API across `allLabels`,
`allLanguages`, `labels`, `shadows`, and `voices`, using seven alternating runs per
case. Compared with the previous layout engine, median improvements include:

- `at(runtime)`: 39.6%
- `contains()` hits/misses: 35.6% / 17.7%
- `find_entry()` hits/misses: 25.8% / 9.1%
- Full `items()` traversal: 2.4%
- `index(string)` hits/misses: 14.6% / 6.4%
- Equal-object comparison: 16.0%

The representative stripped executable is 2.75% smaller, generated source across the
five documents is 30.63% smaller, and `json2cpp.hpp` is about 65.3% smaller by bytes.
Exact results depend on document shape, key position, compiler, and target CPU.

## License

MIT. See [LICENSE](LICENSE).
