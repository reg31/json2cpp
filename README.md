# json2cpp

[![CI](https://github.com/reg31/json2cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/reg31/json2cpp/actions/workflows/ci.yml)

`json2cpp` compiles JSON documents into static C++23 data. Loading requires no parsing,
filesystem access, heap allocation, or runtime initialization. The generated values can
be queried at compile time or runtime through a small, read-only JSON API.

## Highlights

- Static, immutable JSON with a 16-byte `json2cpp::json` value on 64-bit targets.
- `constexpr` lookup, iteration, comparison, and conversion.
- Prehashed keys and generated compact, reference, blob, and perfect-hash layouts.
- Zero-copy object views from `find_entry()` and `items()`.
- UTF-8 (`char`) by default and optional UTF-16 (`char16_t`).
- A generated `.cpp` firewall keeps large resources from being compiled in every
  translation unit.

`json2cpp` intentionally implements a focused subset of the nlohmann JSON API. It is
read-only and does not parse, allocate, mutate, or own JSON data at runtime.

## Requirements

- A C++23 compiler.
- CMake when building the generator.

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

The arguments are the generated namespace name, input JSON file, and output base path.
This creates:

```text
generated/config.hpp
generated/config.cpp
generated/config_impl.hpp
```

Compile `config.cpp` once, add the generated directory to the include path, and include
`config.hpp` where the document is used:

```cpp
#include "generated/config.hpp"

const json2cpp::json &config = compiled_json::config::get();
```

The generated accessor returns a reference to immutable static data. It performs no
runtime loading or allocation.

## Quick start

```cpp
#include "generated/config.hpp"

#include <string_view>

using namespace std::literals;

const auto &config = compiled_json::config::get();

const auto name = config.at("name").get<std::string_view>();
const bool enabled = config["enabled"].get<bool>();
const int retries = config["retries"].get<int>();

for (const auto &tag : config["tags"]) {
  // tag is const json2cpp::json&
}

for (const auto &[key, value] : config.items()) {
  const std::string_view key_view = key.getString();
  (void)value;
}
```

## Breaking change: `find_entry()`

`find_entry()` returns `json2cpp::entry_view_t` by value. It no longer returns
`const json2cpp::value_pair_t *` because compact generated layouts do not physically
store a `value_pair_t`. Synthesizing one required copying a JSON value, thread-local
caching, and possible allocation.

The new result is a nullable, zero-copy, pointer-like view:

```cpp
if (const auto entry = config.find_entry("name")) {
  const std::string_view key = entry->first.getString();
  const std::string_view value = entry->second->get<std::string_view>();
}
```

Migration:

```cpp
// Before
const json2cpp::value_pair_t *entry = config.find_entry("name");
const auto &value = entry->second;

// Now
const json2cpp::entry_view_t entry = config.find_entry("name");
const auto &value = *entry->second;
```

`entry_view_t` does not own data. It remains valid while the source document remains
valid; generated documents have static lifetime.

Its pointer-like operations are provided for concise migration:

```cpp
const auto entry = config.find_entry("name");
if (static_cast<bool>(entry)) {          // operator bool()
  const auto *view = entry.operator->(); // Same object, not a heap pointer.
  const auto &same = entry.operator*();
  (void)view;
  (void)same;
}
```

## API reference

The examples below use the generated `config` document shown above.

### Types and aliases

| Alias | Meaning |
|---|---|
| `json2cpp::json` | `basic_json<char>` by default, or `basic_json<char16_t>` in UTF-16 mode. |
| `json2cpp::array_t` | `std::span<const json>` used to construct arrays. |
| `json2cpp::object_t` | `std::span<const value_pair_t>` used to construct regular objects. |
| `json2cpp::value_pair_t` | A stored `{json key, json value}` pair. |
| `json2cpp::items_t` | Zero-copy object range returned by `items()`. |
| `json2cpp::item_key_t` | Zero-copy key proxy yielded by `items()`. |
| `json2cpp::item_view_t` | `{item_key_t first, const json &second}` yielded by `items()`. |
| `json2cpp::entry_view_t` | Nullable zero-copy result returned by `find_entry()`. |
| `json2cpp::key_descriptor_t` | Generated compact-key descriptor with `view()`. |
| `json2cpp::compact_value_pair_t` / `compact_object_t` | Generated compact-key storage. |
| `json2cpp::ref_value_pair_t` / `ref_value_object_t` | Generated referenced-value storage. |
| `json2cpp::blob_ref_value_pair_t` / `blob_ref_object_t` | Generated packed-key storage. |

The compact, reference, blob, and perfect-hash storage types are public so generated
code can construct them. They are implementation-facing; application code should use
`json`, `array_t`, `object_t`, and the generated accessor.

### Value type and state

`json::Type` contains `Null`, `Boolean`, `String`, `Integer`, `UInteger`, `Float`,
`Array`, and `Object`.

```cpp
const auto &name = config["name"];

const json2cpp::json::Type type = name.type();
const std::size_t length = name.size();       // 4 for "Melo"
const bool no_elements = name.empty();       // false

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
| `size()` | Returns string length or array/object element count; scalar values use `0`. |
| `empty()` | Equivalent to `size() == 0`. |
| `is_object()` | Tests for `Type::Object`. |
| `is_array()` | Tests for `Type::Array`. |
| `is_string()` | Tests for `Type::String`. |
| `is_boolean()` | Tests for `Type::Boolean`. |
| `is_number()` | Tests for signed integer, unsigned integer, or floating point. |
| `is_null()` | Tests for `Type::Null`. |
| `is_sorted_obj()` | Reports whether an object's keys are stored in lexical order. Only meaningful for objects. |
| `hash()` | Returns the cached string hash. Only meaningful for strings and key values. |

```cpp
if (config.is_sorted_obj()) {
  // Some lookups can use the generated sorted layout.
}

const std::uint32_t name_hash = config["name"].hash();
```

### Object lookup

#### `at()`

`at()` returns a `const json &`. String literals are hashed at compile time; generic
string-like keys are accepted through `std::string_view` conversion.

```cpp
const auto &name = config.at("name");

const std::string_view key = "settings";
const auto &settings = config.at(key);

constexpr json2cpp::detail::CompileTimeKey locale_key{"locale"};
const auto &locale = settings.at(locale_key);
```

`at(index)` accesses an array element or an object value by stored order:

```cpp
const auto &first_tag = config["tags"].at(0);
const auto &first_object_value = config.at(0);
```

Missing keys and invalid indexes throw `std::out_of_range`. Calling indexed `at()` on
a scalar throws `std::domain_error`.

#### `operator[]`

`operator[]` forwards to `at()` and has the same checked behavior:

```cpp
const auto &theme = config["settings"]["theme"];
const auto &second_tag = config["tags"][1];
```

#### `contains()`

`contains()` tests object keys without throwing. Literal keys use a compile-time hash.
It returns `false` for non-object values.

```cpp
if (config.contains("threshold")) {
  const double threshold = config["threshold"].get<double>();
}

const std::string_view dynamic_key = "missing";
const bool found = config.contains(dynamic_key); // false
```

#### `find_entry()`

`find_entry()` returns a nullable `entry_view_t`. `first` is a zero-copy key proxy and
`second` is a pointer to the stored value.

```cpp
const auto entry = config.find_entry("name");
if (entry) {
  const auto key = entry->first.getString();
  const auto value = entry->second->get<std::string_view>();
}

const auto missing = config.find_entry("missing");
if (!missing) {
  // No entry was found.
}
```

The prehashed overload avoids hashing a dynamic key when the caller already has the
matching hash:

```cpp
const std::string_view key = "name";
const std::uint32_t hash = json2cpp::json::calc_hash(key);
const auto entry = config.find_entry(key, hash);

constexpr json2cpp::detail::CompileTimeKey name_key{"name"};
const auto literal_entry = config.find_entry(name_key);
```

The supplied hash must have been computed from the same key.

#### `calc_hash()` and `null_value()`

```cpp
const auto hash = json2cpp::json::calc_hash("name");
const json2cpp::json &null = json2cpp::json::null_value();
```

`calc_hash()` uses the same hash as generated keys. `null_value()` returns a reference
to a shared immutable null value.

### Find a value index

`index(value)` searches array elements or object values in stored order. It does not
search object keys. The string overload uses cached hashes before comparing text.

```cpp
const auto tag_index = config["tags"].index("translation"); // 1
const auto theme_index = config["settings"].index("dark"); // 0

const auto &settings = config["settings"];
const auto locale_index = settings.index(settings["locale"]);

if (tag_index == json2cpp::json::npos) {
  // No equal value was found.
}
```

The template overload supports booleans, numbers, strings, and `json` values:

```cpp
const auto retry_index = config.index(3);
const auto enabled_index = config.index(true);
```

### Object iteration with `items()`

`items()` returns an allocation-free `items_t` range over any generated object layout.
Each item has a key proxy named `first` and a value reference named `second`.

```cpp
for (const auto &[key, value] : config.items()) {
  const std::string_view text = key.getString();
  const std::uint32_t hash = key.hash();
  (void)text;
  (void)hash;
  (void)value;
}
```

`item_key_t` converts to `std::string_view` or `json` and compares directly with
string-like values:

```cpp
for (const auto &[key, value] : config.items()) {
  if (key == "name") {
    const std::string_view key_view = key;
    const json2cpp::json key_json = key;
    (void)key_view;
    (void)key_json;
    (void)value;
  }
}
```

Store the range before using a ranges algorithm whose result must be retained:

```cpp
#include <algorithm>
#include <ranges>

const auto items = config.items();
const auto it = std::ranges::find_if(items, [](const auto &item) {
  return item.second == "Melo";
});

if (it != items.end()) {
  const auto key = (*it).first.getString();
}
```

`items_t::size()`, `empty()`, `begin()`, and `end()` provide the normal range
operations. Its iterator supports dereference, pre/post-increment, and equality as a
single-pass input iterator:

```cpp
const auto items = config.items();
const std::size_t count = items.size();
const bool empty = items.empty();
const auto first = items.begin();
const auto last = items.end();
```

Calling `items()` on a non-object throws `std::domain_error`.

### Array iteration and spans

Arrays expose `begin()` and `end()` directly:

```cpp
const auto &tags = config["tags"];

for (const json2cpp::json &tag : tags) {
  const auto text = tag.get<std::string_view>();
  (void)text;
}

const json2cpp::json *first = tags.begin();
const json2cpp::json *last = tags.end();
```

They also convert to `std::span<const json>`:

```cpp
#include <span>

const std::span<const json2cpp::json> tag_span = config["tags"];
```

For non-array values, `begin()` and `end()` return `nullptr`, and span conversion
returns an empty span.

### Read values

#### `get<T>()`

`get<T>()` supports `std::string_view`, `bool`, integral types, and floating-point
types:

```cpp
const auto name = config["name"].get<std::string_view>();
const bool enabled = config["enabled"].get<bool>();
const int retries = config["retries"].get<int>();
const float threshold = config["threshold"].get<float>();
```

Numeric conversions use normal C++ casts. They do not perform range or precision
validation. Unsupported `T` values fail at compile time. A JSON type mismatch throws
`std::domain_error`.

#### `getString()`, `data()`, and `getNumber()`

```cpp
const auto &name_value = config["name"];
const std::string_view name = name_value.getString();
const char *characters = name_value.data();
const double retries = config["retries"].getNumber();
```

`getString()` and `data()` are unchecked low-level string accessors. Check
`is_string()` first or use `get<std::string_view>()` when the input type is uncertain.
`data()` is not guaranteed to be null-terminated. `getNumber()` accepts any numeric
JSON type and throws `std::domain_error` otherwise.

### Equality

JSON values compare recursively:

```cpp
const bool same_document = config == config;
const bool same_settings = config["settings"] == config["settings"];
const bool same_tags = config["tags"] == config["tags"];
```

Values also compare directly with supported C++ scalars and strings:

```cpp
const bool name_matches = config["name"] == "Melo";
const bool enabled_matches = config["enabled"] == true;
const bool retries_match = config["retries"] == 3;
const bool threshold_matches = config["threshold"] == 0.75;
```

Object equality is order-sensitive because generated objects preserve JSON property
order. Floating-point equality is exact. Arrays compare elements recursively, so
arrays containing objects use the same object equality behavior.

### Construct values manually

`json` is non-owning. Array and object backing storage must outlive the `json` value;
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
constexpr json string_view{std::string_view{"Melo"}};
```

Constructing a regular object with a non-string key throws `std::domain_error` or
causes constant evaluation to fail.

Generated code also constructs `compact_object_t`, `ref_value_object_t`, and
`blob_ref_object_t` values. These constructors select an internal storage layout while
preserving the same `json` methods. They are not intended for application code.

`key_descriptor_t` is the only generated storage helper with an application-visible
method:

```cpp
constexpr json2cpp::key_descriptor_t key{"name"};
static_assert(key.view() == "name");
```

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

During constant evaluation, invalid operations fail compilation rather than producing
a runtime exception.

## UTF-16

Define `JSON2CPP_USE_UTF16` consistently before including json2cpp or any generated
header in every translation unit:

```cpp
#define JSON2CPP_USE_UTF16
#include "generated/config.hpp"

#include <string_view>

const auto &config = compiled_json::config::get();
const std::u16string_view name = config.at(u"name").get<std::u16string_view>();
```

This changes `json2cpp::basicType` to `char16_t` and the convenience aliases to their
UTF-16 forms. Do not mix UTF-8 and UTF-16 modes across translation units.

## Constexpr use

The generated implementation owns a `constexpr` document internally. Public generated
accessors use a `.cpp` firewall and are intended for runtime access without runtime
loading. Code that directly includes generated implementation data can use the same API
during constant evaluation:

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

## License

MIT. See [LICENSE](LICENSE).
