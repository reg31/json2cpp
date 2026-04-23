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

#ifndef CONSTEXPR_JSON_HPP_INCLUDED
#define CONSTEXPR_JSON_HPP_INCLUDED

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace json2cpp {

namespace detail {
  template<typename Exception>
  constexpr void throw_exception([[maybe_unused]] const char *msg)
  {
#ifndef NDEBUG
    if consteval { throw msg; }
    else { throw Exception(msg); }
#endif
  }

  template<typename CharType>
  constexpr uint32_t hash_key(std::basic_string_view<CharType> str) noexcept
  {
    uint32_t h = 0x811c9dc5u;
    for (auto c : str) {
      if constexpr (sizeof(CharType) == 1) {
        h ^= static_cast<uint8_t>(c);
        h *= 0x01000193u;
      } else {
        uint32_t val = static_cast<uint32_t>(c);
        for (size_t i = 0; i < sizeof(CharType); ++i) {
          h ^= static_cast<uint8_t>(val);
          h *= 0x01000193u;
          val >>= 8;
        }
      }
    }
    const uint32_t result = (h ^ (h >> 28)) & 0x0FFFFFFFu;
    return result != 0u ? result : 1u;
  }

  template<typename CharType, size_t N>
  struct CompileTimeKey
  {
    std::basic_string_view<CharType> value;
    uint32_t hash;

    constexpr CompileTimeKey(const CharType (&str)[N]) noexcept
      : value(str, N - 1), hash(hash_key(value)) {}

    constexpr operator std::basic_string_view<CharType>() const noexcept { return value; }
  };

  template<typename T, typename CharType>
  concept char_array_like =
    std::is_array_v<std::remove_reference_t<T>>
    && std::same_as<std::remove_cv_t<std::remove_extent_t<std::remove_reference_t<T>>>, CharType>;

  template<typename T, typename CharType>
  concept string_like =
    std::convertible_to<T, std::basic_string_view<CharType>> || char_array_like<T, CharType>
    || requires(const T &value) {
      { value.data() } -> std::convertible_to<const CharType *>;
      { value.size() } -> std::convertible_to<size_t>;
    };

  template<typename CharType, typename T>
  constexpr std::basic_string_view<CharType> make_string_view(const T &value) noexcept
  {
    if constexpr (std::convertible_to<T, std::basic_string_view<CharType>>) {
      return std::basic_string_view<CharType>(value);
    } else if constexpr (char_array_like<T, CharType>) {
      return { value, std::extent_v<std::remove_reference_t<T>> - 1 };
    } else {
      return { value.data(), static_cast<size_t>(value.size()) };
    }
  }

  template<typename CharType>
  struct find_entry_cache;
}

template<typename CharType> struct basic_json;
template<typename CharType> struct basic_items_t;
template<typename CharType> struct basic_key_descriptor;
template<typename CharType> struct basic_compact_value_pair_t;
template<typename CharType> struct basic_ref_value_pair_t;
template<typename CharType> struct basic_blob_ref_value_pair_t;
template<typename CharType> struct basic_blob_ref_object_t;
template<typename CharType> struct basic_item_key_t;
template<typename CharType> struct basic_item_view_t;

template<typename F, typename S>
struct pair { F first; S second; };

template<typename CharType> using basic_value_pair_t = pair<basic_json<CharType>, basic_json<CharType>>;
template<typename CharType> using basic_object_t = std::span<const basic_value_pair_t<CharType>>;
template<typename CharType> using basic_array_t = std::span<const basic_json<CharType>>;
template<typename CharType> using basic_compact_object_t = std::span<const basic_compact_value_pair_t<CharType>>;
template<typename CharType> using basic_ref_value_object_t = std::span<const basic_ref_value_pair_t<CharType>>;

template<typename CharType>
struct basic_json
{
  enum class Type : uint8_t { Null, Boolean, String, Integer, UInteger, Float, Array, Object };

private:
  friend struct basic_items_t<CharType>;
  friend struct basic_item_key_t<CharType>;
  friend struct basic_blob_ref_value_pair_t<CharType>;

  enum class ObjectLayout : uint32_t { Regular = 0, CompactInline = 1, ValueByReference = 2, BlobByReference = 3 };

  struct prehashed_t {};

  struct object_key_view { std::basic_string_view<CharType> value{}; uint32_t hash = 0; };

public:
  static constexpr uint32_t type_mask = 0b111u;
  static constexpr uint32_t sorted_mask = 0b1000u;
  static constexpr uint32_t object_layout_shift = 4;
  static constexpr uint32_t object_layout_mask = 0b111u << object_layout_shift;
  static constexpr size_t npos = static_cast<size_t>(-1);

private:
  static constexpr size_t capacity = sizeof(uint64_t) / sizeof(CharType);
  static constexpr uint64_t blob_key_hash_bits = 20, blob_value_hash_bits = 16, blob_length_bits = 12;
  static constexpr uint64_t blob_key_hash_mask = (uint64_t{1} << blob_key_hash_bits) - 1u,
                            blob_value_hash_mask = (uint64_t{1} << blob_value_hash_bits) - 1u,
                            blob_length_mask = (uint64_t{1} << blob_length_bits) - 1u;
  static constexpr uint64_t blob_value_hash_shift = blob_key_hash_bits;
  static constexpr uint64_t blob_length_shift = blob_key_hash_bits + blob_value_hash_bits;
  static constexpr uint64_t blob_offset_shift = blob_key_hash_bits + blob_value_hash_bits + blob_length_bits;

  uint32_t length_ = 0;
  uint32_t metadata_ = 0;
  union {
    const basic_json *array_value;
    const basic_value_pair_t<CharType> *object_value;
    const basic_compact_value_pair_t<CharType> *compact_object_value;
    const basic_ref_value_pair_t<CharType> *ref_value_object_value;
    const basic_blob_ref_value_pair_t<CharType> *blob_ref_object_value;
    const CharType *long_data;
    std::array<CharType, capacity> short_data;
    int64_t int_value;
    uint64_t uint_value;
    double float_value;
    bool boolean_value;
  } data_storage_;

  static constexpr uint32_t layout_bits(ObjectLayout layout) noexcept
  { return static_cast<uint32_t>(layout) << object_layout_shift; }

  static constexpr uint32_t blob_key_offset(uint64_t key_meta) noexcept
  { return static_cast<uint32_t>(key_meta >> blob_offset_shift); }
  static constexpr uint32_t blob_key_length(uint64_t key_meta) noexcept
  { return static_cast<uint32_t>((key_meta >> blob_length_shift) & blob_length_mask); }
  static constexpr uint32_t blob_key_hash(uint64_t key_meta) noexcept
  { return static_cast<uint32_t>(key_meta & blob_key_hash_mask); }
  static constexpr uint32_t blob_value_hash(uint64_t key_meta) noexcept
  { return static_cast<uint32_t>((key_meta >> blob_value_hash_shift) & blob_value_hash_mask); }
  static constexpr uint32_t blob_target_hash(uint32_t hash) noexcept
  { return hash & static_cast<uint32_t>(blob_key_hash_mask); }
  static constexpr uint32_t blob_target_value_hash(uint32_t hash) noexcept
  { return hash & static_cast<uint32_t>(blob_value_hash_mask); }
  static constexpr std::basic_string_view<CharType> blob_key_view(
    const basic_blob_ref_value_pair_t<CharType> *entries, const basic_blob_ref_value_pair_t<CharType> &entry) noexcept;

  constexpr void set_metadata(Type t, size_t len, bool sorted = false, uint32_t extra_bits = 0u) noexcept;
  constexpr void set_string_metadata(size_t len, uint32_t hash_val) noexcept;

  template<typename EntrySpan, typename GetKey>
  static constexpr bool is_sorted_entries(EntrySpan entries, GetKey get_key) noexcept;

  template<typename Entry>
  static constexpr void validate_object_keys(std::span<const Entry> entries) noexcept;

  template<typename Entry>
    requires requires(const Entry &entry) { entry.first.getString(); }
  static constexpr object_key_view get_entry_key(const Entry &entry) noexcept;
  static constexpr object_key_view get_entry_key(const basic_compact_value_pair_t<CharType> &entry) noexcept;

  template<typename Entry>
  constexpr void init_object(std::span<const Entry> entries, ObjectLayout layout) noexcept;

  [[nodiscard]] constexpr ObjectLayout object_layout() const noexcept;
  [[nodiscard]] constexpr object_key_view entry_key(size_t index) const noexcept;
  [[nodiscard]] constexpr const basic_json &entry_value(size_t index) const noexcept;
  [[nodiscard]] constexpr basic_value_pair_t<CharType> materialize_entry(size_t index) const noexcept;
  [[nodiscard]] constexpr size_t find_sorted_entry_index(std::basic_string_view<CharType> key) const noexcept;
  [[nodiscard]] constexpr size_t find_entry_index(std::basic_string_view<CharType> key) const noexcept;
  [[nodiscard]] constexpr size_t find_entry_index(std::basic_string_view<CharType> key, uint32_t target_hash) const noexcept;

  constexpr basic_json(std::basic_string_view<CharType> v, uint32_t hash_val, prehashed_t) noexcept;

public:
  static constexpr uint32_t calc_hash(std::basic_string_view<CharType> sv) noexcept
  { return detail::hash_key(sv); }

  static const basic_json &null_value() noexcept { static constexpr basic_json v{}; return v; }

  [[nodiscard]] constexpr const basic_value_pair_t<CharType> *find_entry(
    std::basic_string_view<CharType> key, uint32_t target_hash) const noexcept;

  [[nodiscard]] constexpr const basic_value_pair_t<CharType> *find_entry(
    std::basic_string_view<CharType> key) const noexcept
  { return find_entry(key, calc_hash(key)); }

  template<size_t N>
  [[nodiscard]] constexpr const basic_value_pair_t<CharType> *find_entry(
    const detail::CompileTimeKey<CharType, N> &key) const noexcept
  { return find_entry(key.value, key.hash); }

  [[nodiscard]] constexpr Type type() const noexcept
  { return static_cast<Type>(metadata_ & type_mask); }

  [[nodiscard]] constexpr size_t size() const noexcept { return length_; }
  [[nodiscard]] constexpr bool is_sorted_obj() const noexcept { return (metadata_ & sorted_mask) != 0u; }
  [[nodiscard]] constexpr uint32_t hash() const noexcept { return metadata_ >> 4; }

  constexpr basic_json() noexcept : length_(0), metadata_(0), data_storage_{ .short_data = {} } {}

  template<size_t N>
  constexpr basic_json(const CharType (&v)[N]) noexcept
    : basic_json(std::basic_string_view<CharType>(v, N - 1)) {}

  constexpr basic_json(const CharType *v) noexcept
    : basic_json(std::basic_string_view<CharType>(v)) {}

  constexpr basic_json(std::basic_string_view<CharType> v) noexcept : basic_json(v, calc_hash(v), prehashed_t{}) {}

  constexpr basic_json(std::nullptr_t) noexcept : data_storage_{ .short_data = {} } { set_metadata(Type::Null, 0); }
  constexpr basic_json(bool v) noexcept : data_storage_{ .boolean_value = v } { set_metadata(Type::Boolean, 0); }
  constexpr basic_json(std::signed_integral auto v) noexcept : data_storage_{ .int_value = v } { set_metadata(Type::Integer, 0); }
  constexpr basic_json(std::unsigned_integral auto v) noexcept : data_storage_{ .uint_value = v } { set_metadata(Type::UInteger, 0); }
  constexpr basic_json(std::floating_point auto v) noexcept : data_storage_{ .float_value = v } { set_metadata(Type::Float, 0); }
  constexpr basic_json(basic_array_t<CharType> v) noexcept : data_storage_{ .array_value = v.data() } { set_metadata(Type::Array, v.size()); }

  constexpr basic_json(basic_object_t<CharType> v) noexcept : data_storage_{ .object_value = v.data() }
  { init_object(v, ObjectLayout::Regular); }

  constexpr basic_json(basic_compact_object_t<CharType> v) noexcept : data_storage_{ .compact_object_value = v.data() }
  { init_object(v, ObjectLayout::CompactInline); }

  constexpr basic_json(basic_ref_value_object_t<CharType> v) noexcept : data_storage_{ .ref_value_object_value = v.data() }
  { init_object(v, ObjectLayout::ValueByReference); }

  constexpr basic_json(basic_blob_ref_object_t<CharType> v) noexcept;

  [[nodiscard]] constexpr bool is_object() const noexcept { return type() == Type::Object; }
  [[nodiscard]] constexpr bool is_array() const noexcept { return type() == Type::Array; }
  [[nodiscard]] constexpr bool is_string() const noexcept { return type() == Type::String; }
  [[nodiscard]] constexpr bool is_boolean() const noexcept { return type() == Type::Boolean; }
  [[nodiscard]] constexpr bool is_null() const noexcept { return type() == Type::Null; }
  [[nodiscard]] constexpr bool empty() const noexcept { return length_ == 0; }

  [[nodiscard]] constexpr bool is_number() const noexcept
  { const auto t = type(); return t == Type::Integer || t == Type::UInteger || t == Type::Float; }

  [[nodiscard]] constexpr const basic_json &operator[](std::integral auto index) const
  { return at(index); }

  template<size_t N>
  [[nodiscard]] constexpr const basic_json &operator[](const CharType (&key)[N]) const
  { return at(detail::CompileTimeKey<CharType, N>(key)); }

  template<typename Key>
  [[nodiscard]] constexpr const basic_json &operator[](const Key &key) const
    requires(detail::string_like<Key, CharType> && !detail::char_array_like<Key, CharType>)
  { return at(key); }

  constexpr bool operator==(const basic_json &other) const noexcept
  {
    const auto t = type();
    if (t != other.type()) return false;

    switch (t) {
    case Type::Null:
      return true;
    case Type::Boolean:
      return data_storage_.boolean_value == other.data_storage_.boolean_value;
    case Type::Integer:
      return data_storage_.int_value == other.data_storage_.int_value;
    case Type::UInteger:
      return data_storage_.uint_value == other.data_storage_.uint_value;
    case Type::Float:
      return data_storage_.float_value == other.data_storage_.float_value;
    case Type::String:
      return getString() == other.getString();
    case Type::Array:
      if (length_ != other.length_) return false;
      for (size_t i = 0; i < length_; ++i)
        if (!(data_storage_.array_value[i] == other.data_storage_.array_value[i])) return false;
      return true;
    case Type::Object:
      if (length_ != other.length_) return false;
      for (size_t i = 0; i < length_; ++i) {
        const auto lhs_key = entry_key(i);
        const auto rhs_key = other.entry_key(i);
        if (lhs_key.hash != rhs_key.hash || lhs_key.value != rhs_key.value) return false;
        if (!(entry_value(i) == other.entry_value(i))) return false;
      }
      return true;
    }
    return false;
  }

  constexpr bool operator==(std::basic_string_view<CharType> other) const noexcept
  { return is_string() && getString() == other; }

  template<typename T>
  constexpr bool operator==(const T &other) const noexcept
  {
    if constexpr (std::is_same_v<T, bool>)
      return is_boolean() && data_storage_.boolean_value == other;
    else if constexpr (std::is_integral_v<T>)
      if constexpr (std::is_signed_v<T>) {
        const auto t = type();
        return t == Type::Integer
          ? data_storage_.int_value == other
          : (t == Type::UInteger && other >= 0 && data_storage_.uint_value == static_cast<uint64_t>(other));
      } else {
        const auto t = type();
        return t == Type::UInteger
          ? data_storage_.uint_value == other
          : (t == Type::Integer && data_storage_.int_value >= 0
             && static_cast<uint64_t>(data_storage_.int_value) == other);
      }
    else if constexpr (std::is_floating_point_v<T>)
      switch (type()) {
      case Type::Float:
        return data_storage_.float_value == other;
      case Type::Integer:
        return static_cast<double>(data_storage_.int_value) == other;
      case Type::UInteger:
        return static_cast<double>(data_storage_.uint_value) == other;
      default:
        return false;
      }
    else if constexpr (detail::string_like<T, CharType>)
      return is_string() && getString() == detail::make_string_view<CharType>(other);
    else
      return false;
  }

  constexpr const basic_json &at(std::integral auto index) const;

  template<size_t N>
  [[nodiscard]] constexpr const basic_json &at(const CharType (&key)[N]) const
  { return at(detail::CompileTimeKey<CharType, N>(key)); }

  template<size_t N>
  [[nodiscard]] constexpr const basic_json &at(const detail::CompileTimeKey<CharType, N> &key) const
  {
    if (is_object() && object_layout() == ObjectLayout::Regular) [[likely]] {
      if (const auto *entry = find_regular_entry(key.value, key.hash)) [[likely]] return entry->second;
      detail::throw_exception<std::out_of_range>("Key not found");
      return null_value();
    }
    const auto index = find_entry_index(key.value, key.hash);
    if (index != npos) [[likely]] return entry_value(index);
    detail::throw_exception<std::out_of_range>("Key not found");
    return null_value();
  }

  [[nodiscard]] constexpr const basic_json &at(std::basic_string_view<CharType> view) const
  {
    if (is_object() && object_layout() == ObjectLayout::Regular) [[likely]] {
      if (const auto *entry = find_regular_entry(view, calc_hash(view))) [[likely]] return entry->second;
      detail::throw_exception<std::out_of_range>("Key not found");
      return null_value();
    }
    const auto index = find_entry_index(view);
    if (index != npos) [[likely]] return entry_value(index);
    detail::throw_exception<std::out_of_range>("Key not found");
    return null_value();
  }

  template<typename Key>
  [[nodiscard]] constexpr const basic_json &at(const Key &key) const
    requires(detail::string_like<Key, CharType> && !detail::char_array_like<Key, CharType>)
  { return at(detail::make_string_view<CharType>(key)); }

  [[nodiscard]] constexpr bool contains(std::basic_string_view<CharType> key) const noexcept
  { return find_entry_index(key) != npos; }

  template<size_t N>
  [[nodiscard]] constexpr bool contains(const CharType (&key)[N]) const noexcept
  { const detail::CompileTimeKey<CharType, N> lookup(key); return find_entry_index(lookup.value, lookup.hash) != npos; }

  [[nodiscard]] constexpr size_t index(std::basic_string_view<CharType> view) const noexcept
  {
    const auto target_hash = calc_hash(view);

    if (is_array()) {
      for (size_t i = 0; i < length_; ++i) {
        const auto &current = data_storage_.array_value[i];
        if (current.is_string() && current.hash() == target_hash && current.getString() == view) return i;
      }
      return npos;
    }

    if (!is_object()) return npos;
    const auto layout = object_layout();
    if (layout == ObjectLayout::Regular) {
      const auto entries = std::span(data_storage_.object_value, length_);
      for (size_t i = 0; i < entries.size(); ++i) {
        const auto &current = entries[i].second;
        if (current.is_string() && current.hash() == target_hash && current.getString() == view) return i;
      }
      return npos;
    }

    if (layout == ObjectLayout::CompactInline) {
      const auto entries = std::span(data_storage_.compact_object_value, length_);
      for (size_t i = 0; i < entries.size(); ++i) {
        const auto &current = entries[i].value;
        if (current.is_string() && current.hash() == target_hash && current.getString() == view) return i;
      }
      return npos;
    }

    if (layout == ObjectLayout::BlobByReference) {
      const auto entries = std::span(data_storage_.blob_ref_object_value, length_);
      const auto value_hash = blob_target_value_hash(target_hash);
      for (size_t i = 0; i < entries.size(); ++i) {
        if (blob_value_hash(entries[i].key_meta) != value_hash) continue;
        const auto &current = *entries[i].value;
        if (current.is_string() && current.hash() == target_hash && current.getString() == view) return i;
      }
      return npos;
    }

    const auto entries = std::span(data_storage_.ref_value_object_value, length_);
    for (size_t i = 0; i < entries.size(); ++i) {
      const auto &current = *entries[i].second;
      if (current.is_string() && current.hash() == target_hash && current.getString() == view) return i;
    }
    return npos;
  }

  template<typename T>
  [[nodiscard]] constexpr size_t index(const T &value) const noexcept
  {
    if constexpr (detail::string_like<T, CharType>) {
      return index(detail::make_string_view<CharType>(value));
    } else {
      if (is_array()) {
        for (size_t i = 0; i < length_; ++i)
          if (data_storage_.array_value[i] == value) return i;
        return npos;
      }

      if (!is_object()) return npos;
      const auto layout = object_layout();
      if (layout == ObjectLayout::Regular) {
        const auto entries = std::span(data_storage_.object_value, length_);
        for (size_t i = 0; i < entries.size(); ++i)
          if (entries[i].second == value) return i;
        return npos;
      }

      if (layout == ObjectLayout::CompactInline) {
        const auto entries = std::span(data_storage_.compact_object_value, length_);
        for (size_t i = 0; i < entries.size(); ++i)
          if (entries[i].value == value) return i;
        return npos;
      }

      if (layout == ObjectLayout::BlobByReference) {
        const auto entries = std::span(data_storage_.blob_ref_object_value, length_);
        for (size_t i = 0; i < entries.size(); ++i)
          if (*entries[i].value == value) return i;
        return npos;
      }

      const auto entries = std::span(data_storage_.ref_value_object_value, length_);
      for (size_t i = 0; i < entries.size(); ++i)
        if (*entries[i].second == value) return i;
      return npos;
    }
  }

  [[nodiscard]] constexpr basic_items_t<CharType> items() const;

  [[nodiscard]] constexpr const basic_json *begin() const noexcept
  { return is_array() ? data_storage_.array_value : nullptr; }

  [[nodiscard]] constexpr const basic_json *end() const noexcept
  { return is_array() ? data_storage_.array_value + length_ : nullptr; }

  constexpr operator std::span<const basic_json>() const noexcept
  {
    if (!is_array()) return {};
    return { data_storage_.array_value, length_ };
  }

  [[nodiscard]] constexpr const CharType *data() const noexcept
  { return length_ <= capacity ? data_storage_.short_data.data() : data_storage_.long_data; }

  [[nodiscard]] constexpr std::basic_string_view<CharType> getString() const noexcept
  { return { data(), length_ }; }

  [[nodiscard]] constexpr double getNumber() const;

  template<typename T>
  [[nodiscard]] constexpr T get() const
  {
    if constexpr (std::is_same_v<T, std::basic_string_view<CharType>>) {
      if (!is_string()) [[unlikely]] {
        detail::throw_exception<std::domain_error>("JSON value is not a string");
        return {};
      }
      return getString();
    } else if constexpr (std::is_same_v<T, bool>) {
      if (!is_boolean()) [[unlikely]] {
        detail::throw_exception<std::domain_error>("JSON value is not a boolean");
        return false;
      }
      return data_storage_.boolean_value;
    } else if constexpr (std::is_floating_point_v<T>) {
      return static_cast<T>(getNumber());
    } else if constexpr (std::is_integral_v<T>) {
      const auto t = type();
      if (t != Type::Integer && t != Type::UInteger && t != Type::Float) [[unlikely]] {
        detail::throw_exception<std::domain_error>("JSON value is not a number");
        return T{};
      }
      if (t == Type::Integer) return static_cast<T>(data_storage_.int_value);
      if (t == Type::UInteger) return static_cast<T>(data_storage_.uint_value);
      return static_cast<T>(data_storage_.float_value);
    } else {
      static_assert(false, "Unsupported type for get<T>()");
    }
  }

private:
  [[nodiscard]] constexpr const basic_value_pair_t<CharType> *find_regular_entry(
    std::basic_string_view<CharType> key, uint32_t target_hash) const noexcept;
};

template<typename CharType>
struct basic_key_descriptor
{
  const CharType *data = nullptr;
  uint32_t length = 0;
  uint32_t hash = 0;

  constexpr basic_key_descriptor() noexcept = default;
  constexpr explicit basic_key_descriptor(std::basic_string_view<CharType> sv) noexcept
    : data(sv.data()), length(static_cast<uint32_t>(sv.size())), hash(detail::hash_key(sv)) {}

  template<size_t N>
  constexpr basic_key_descriptor(const CharType (&str)[N]) noexcept : basic_key_descriptor(std::basic_string_view<CharType>(str, N - 1)) {}

  [[nodiscard]] constexpr std::basic_string_view<CharType> view() const noexcept { return { data, length }; }
};

template<typename CharType>
struct basic_compact_value_pair_t { const basic_key_descriptor<CharType> *key = nullptr; basic_json<CharType> value; };

template<typename CharType>
struct basic_ref_value_pair_t { basic_json<CharType> first; const basic_json<CharType> *second = nullptr; };

template<typename CharType>
struct basic_blob_ref_value_pair_t
{
  struct header_t {};

  union {
    const basic_json<CharType> *value = nullptr;
    const CharType *keys;
  };
  uint64_t key_meta = 0;

  constexpr basic_blob_ref_value_pair_t() noexcept = default;
  constexpr basic_blob_ref_value_pair_t(const CharType *k, header_t) noexcept : keys(k), key_meta(0) {}
  constexpr basic_blob_ref_value_pair_t(
    const basic_json<CharType> *v, uint32_t offset, uint32_t length, uint32_t hash, uint32_t value_hash = 0) noexcept
    : value(v),
      key_meta((uint64_t{offset} << basic_json<CharType>::blob_offset_shift)
             | (uint64_t{length} << basic_json<CharType>::blob_length_shift)
             | (uint64_t{basic_json<CharType>::blob_target_value_hash(value_hash)}
                << basic_json<CharType>::blob_value_hash_shift)
             | basic_json<CharType>::blob_target_hash(hash))
  {}
};

template<typename CharType>
struct basic_blob_ref_object_t { const basic_blob_ref_value_pair_t<CharType> *entries = nullptr; size_t size = 0; };

template<typename CharType>
struct basic_item_key_t
{
  const basic_json<CharType> *owner = nullptr;
  size_t index = 0;

  [[nodiscard]] constexpr std::basic_string_view<CharType> getString() const noexcept
  { return owner == nullptr ? std::basic_string_view<CharType>{} : owner->entry_key(index).value; }

  [[nodiscard]] constexpr uint32_t hash() const noexcept
  { return owner == nullptr ? 0u : owner->entry_key(index).hash; }

  constexpr operator std::basic_string_view<CharType>() const noexcept { return getString(); }

  constexpr operator basic_json<CharType>() const noexcept
  {
    if (owner == nullptr) return {};
    const auto key = owner->entry_key(index);
    return basic_json<CharType>(key.value, key.hash, typename basic_json<CharType>::prehashed_t{});
  }

  template<typename T>
  constexpr bool operator==(const T &other) const noexcept
    requires(detail::string_like<T, CharType>)
  { return getString() == detail::make_string_view<CharType>(other); }
};

template<typename CharType>
struct basic_item_view_t { basic_item_key_t<CharType> first; const basic_json<CharType> &second; };

template<typename CharType>
struct basic_items_t
{
  struct iterator
  {
    const basic_json<CharType> *owner = nullptr;
    const void *entry = nullptr;
    const basic_json<CharType> *value = nullptr;
    size_t index = 0;
    size_t stride = 0;
    uint8_t layout = 0;

    using value_type = basic_item_view_t<CharType>;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::input_iterator_tag;

    [[nodiscard]] constexpr value_type operator*() const noexcept { return { { owner, index }, *value }; }
    constexpr iterator &operator++() noexcept
    {
      ++index;
      if (owner == nullptr || index == owner->size()) {
        entry = nullptr;
        value = nullptr;
        return *this;
      }
      entry = static_cast<const std::byte *>(entry) + stride;
      value = value_from_entry(entry, layout);
      return *this;
    }
    constexpr void operator++(int) noexcept { ++(*this); }

    constexpr bool operator==(const iterator &other) const noexcept
    { return owner == other.owner && index == other.index; }

  private:
    static constexpr const basic_json<CharType> *value_from_entry(const void *current, uint8_t layout) noexcept
    {
      if (layout == 0u) return &static_cast<const basic_value_pair_t<CharType> *>(current)->second;
      if (layout == 1u) return &static_cast<const basic_compact_value_pair_t<CharType> *>(current)->value;
      if (layout == 2u) return static_cast<const basic_ref_value_pair_t<CharType> *>(current)->second;
      return static_cast<const basic_blob_ref_value_pair_t<CharType> *>(current)->value;
    }
  };

  const basic_json<CharType> *owner = nullptr;
  const void *entries = nullptr;
  const basic_json<CharType> *first_value = nullptr;
  size_t stride = 0;
  uint8_t layout = 0;

  [[nodiscard]] constexpr size_t size() const noexcept { return owner == nullptr ? 0u : owner->size(); }
  [[nodiscard]] constexpr bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] constexpr iterator begin() const noexcept { return { owner, entries, first_value, 0, stride, layout }; }
  [[nodiscard]] constexpr iterator end() const noexcept { return { owner, nullptr, nullptr, size(), stride, layout }; }
};

namespace detail {
  template<typename CharType>
  struct find_entry_cache
  {
    struct cache_slot
    {
      const basic_json<CharType> *owner = nullptr;
      size_t index = 0;
      const basic_value_pair_t<CharType> *value = nullptr;
    };

    static constexpr size_t slot_count = 256;

    static size_t slot_index(const basic_json<CharType> *owner, size_t index) noexcept
    {
      auto hash = static_cast<size_t>(reinterpret_cast<std::uintptr_t>(owner) >> 4);
      hash ^= index + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
      return hash & (slot_count - 1);
    }

    static const basic_value_pair_t<CharType> *store(
      const basic_json<CharType> *owner, size_t index, basic_value_pair_t<CharType> entry) noexcept
    {
      static thread_local std::array<cache_slot, slot_count> slots{};
      static thread_local std::deque<basic_value_pair_t<CharType>> cache;
      const auto slot = slot_index(owner, index);
      const auto &existing = slots[slot];
      if (existing.value != nullptr && existing.owner == owner && existing.index == index) return existing.value;

      cache.emplace_back(std::move(entry));
      auto *stored = &cache.back();
      slots[slot] = cache_slot{ owner, index, stored };
      return stored;
    }
  };
}

template<typename CharType>
constexpr void basic_json<CharType>::set_metadata(Type t, size_t len, bool sorted, uint32_t extra_bits) noexcept
{
  length_ = static_cast<uint32_t>(len);
  metadata_ = (std::to_underlying(t) & type_mask)
            | ((static_cast<uint32_t>(sorted) & 0b1u) << 3)
            | extra_bits;
}

template<typename CharType>
constexpr void basic_json<CharType>::set_string_metadata(size_t len, uint32_t hash_val) noexcept
{
  set_metadata(Type::String, len, false, (hash_val & 0x0FFFFFFFu) << 4);
}

template<typename CharType>
constexpr std::basic_string_view<CharType> basic_json<CharType>::blob_key_view(
  const basic_blob_ref_value_pair_t<CharType> *entries, const basic_blob_ref_value_pair_t<CharType> &entry) noexcept
{
  return { entries[-1].keys + blob_key_offset(entry.key_meta), blob_key_length(entry.key_meta) };
}

template<typename CharType>
template<typename EntrySpan, typename GetKey>
constexpr bool basic_json<CharType>::is_sorted_entries(EntrySpan entries, GetKey get_key) noexcept
{
  if (entries.size() < 2) return true;

  auto previous = get_key(entries[0]);
  for (size_t i = 1; i < entries.size(); ++i) {
    const auto current = get_key(entries[i]);
    if (current < previous) return false;
    previous = current;
  }
  return true;
}

template<typename CharType>
template<typename Entry>
constexpr void basic_json<CharType>::validate_object_keys(std::span<const Entry> entries) noexcept
{
  if constexpr (requires(const Entry &entry) { entry.first; }) {
    for (const auto &entry : entries)
      if (!entry.first.is_string()) [[unlikely]]
        detail::throw_exception<std::domain_error>("JSON object keys must be strings");
  }
}

template<typename CharType>
template<typename Entry>
  requires requires(const Entry &entry) { entry.first.getString(); }
constexpr auto basic_json<CharType>::get_entry_key(const Entry &entry) noexcept -> object_key_view
{ return { entry.first.getString(), entry.first.hash() }; }

template<typename CharType>
constexpr auto basic_json<CharType>::get_entry_key(const basic_compact_value_pair_t<CharType> &entry) noexcept
  -> object_key_view
{ return { entry.key->view(), entry.key->hash }; }

template<typename CharType>
template<typename Entry>
constexpr void basic_json<CharType>::init_object(std::span<const Entry> entries, ObjectLayout layout) noexcept
{
  validate_object_keys(entries);
  const bool sorted = is_sorted_entries(entries, [](const Entry &entry) { return get_entry_key(entry).value; });
  set_metadata(Type::Object, entries.size(), sorted, layout_bits(layout));
}

template<typename CharType>
constexpr basic_json<CharType>::basic_json(basic_blob_ref_object_t<CharType> v) noexcept
  : data_storage_{ .blob_ref_object_value = v.entries }
{
  const auto entries = std::span(v.entries, v.size);
  const bool sorted = is_sorted_entries(
    entries, [all_entries = v.entries](const auto &entry) { return blob_key_view(all_entries, entry); });
  set_metadata(Type::Object, v.size, sorted, layout_bits(ObjectLayout::BlobByReference));
}

template<typename CharType>
constexpr auto basic_json<CharType>::object_layout() const noexcept -> ObjectLayout
{
  if (type() != Type::Object) return ObjectLayout::Regular;
  return static_cast<ObjectLayout>((metadata_ & object_layout_mask) >> object_layout_shift);
}

template<typename CharType>
constexpr basic_json<CharType>::basic_json(
  std::basic_string_view<CharType> v, uint32_t hash_val, prehashed_t) noexcept
  : data_storage_{ .short_data = {} }
{
  set_string_metadata(v.size(), hash_val);
  if (v.size() <= capacity) {
    for (size_t i = 0; i < v.size(); ++i) data_storage_.short_data[i] = v[i];
  } else {
    data_storage_.long_data = v.data();
  }
}

template<typename CharType>
constexpr auto basic_json<CharType>::entry_key(size_t index) const noexcept -> object_key_view
{
  const auto layout = object_layout();
  if (layout == ObjectLayout::Regular) {
    const auto &entry = data_storage_.object_value[index];
    return { entry.first.getString(), entry.first.hash() };
  }
  if (layout == ObjectLayout::CompactInline) {
    const auto &entry = data_storage_.compact_object_value[index];
    return { entry.key->view(), entry.key->hash };
  }
  if (layout == ObjectLayout::BlobByReference) {
    const auto entries = data_storage_.blob_ref_object_value;
    const auto &entry = entries[index];
    return { blob_key_view(data_storage_.blob_ref_object_value, entry), blob_key_hash(entry.key_meta) };
  }

  const auto &entry = data_storage_.ref_value_object_value[index];
  return { entry.first.getString(), entry.first.hash() };
}

template<typename CharType>
constexpr const basic_json<CharType> &basic_json<CharType>::entry_value(size_t index) const noexcept
{
  const auto layout = object_layout();
  if (layout == ObjectLayout::Regular) return data_storage_.object_value[index].second;
  if (layout == ObjectLayout::CompactInline) return data_storage_.compact_object_value[index].value;
  if (layout == ObjectLayout::BlobByReference) return *data_storage_.blob_ref_object_value[index].value;
  return *data_storage_.ref_value_object_value[index].second;
}

template<typename CharType>
constexpr basic_value_pair_t<CharType> basic_json<CharType>::materialize_entry(size_t index) const noexcept
{
  const auto key = entry_key(index);
  return { basic_json(key.value, key.hash, prehashed_t{}), entry_value(index) };
}

template<typename CharType>
constexpr size_t basic_json<CharType>::find_sorted_entry_index(std::basic_string_view<CharType> key) const noexcept
{
  const auto layout = object_layout();
  if (layout == ObjectLayout::Regular) {
    const auto entries = std::span(data_storage_.object_value, length_);
    size_t first = 0;
    size_t count = entries.size();

    while (count > 0) {
      const size_t step = count / 2;
      const size_t index = first + step;
      if (entries[index].first.getString() < key) {
        first = index + 1;
        count -= step + 1;
      } else {
        count = step;
      }
    }

    return first < entries.size() && entries[first].first.getString() == key ? first : npos;
  }

  if (layout == ObjectLayout::CompactInline) {
    const auto entries = std::span(data_storage_.compact_object_value, length_);
    size_t first = 0;
    size_t count = entries.size();

    while (count > 0) {
      const size_t step = count / 2;
      const size_t index = first + step;
      if (entries[index].key->view() < key) {
        first = index + 1;
        count -= step + 1;
      } else {
        count = step;
      }
    }

    return first < entries.size() && entries[first].key->view() == key ? first : npos;
  }

  if (layout == ObjectLayout::BlobByReference) {
    const auto entries_ptr = data_storage_.blob_ref_object_value;
    const auto entries = std::span(entries_ptr, length_);
    size_t first = 0;
    size_t count = entries.size();

    while (count > 0) {
      const size_t step = count / 2;
      const size_t index = first + step;
      if (blob_key_view(entries_ptr, entries[index]) < key) {
        first = index + 1;
        count -= step + 1;
      } else {
        count = step;
      }
    }

    return first < entries.size() && blob_key_view(entries_ptr, entries[first]) == key ? first : npos;
  }

  const auto entries = std::span(data_storage_.ref_value_object_value, length_);
  size_t first = 0;
  size_t count = entries.size();

  while (count > 0) {
    const size_t step = count / 2;
    const size_t index = first + step;
    if (entries[index].first.getString() < key) {
      first = index + 1;
      count -= step + 1;
    } else {
      count = step;
    }
  }

  return first < entries.size() && entries[first].first.getString() == key ? first : npos;
}

template<typename CharType>
constexpr size_t basic_json<CharType>::find_entry_index(std::basic_string_view<CharType> key) const noexcept
{
  if (!is_object()) return npos;
  if (length_ == 0) return npos;
  if (object_layout() == ObjectLayout::BlobByReference) return find_entry_index(key, calc_hash(key));
  if (is_sorted_obj()) return find_sorted_entry_index(key);
  return find_entry_index(key, calc_hash(key));
}

template<typename CharType>
constexpr size_t basic_json<CharType>::find_entry_index(
  std::basic_string_view<CharType> key, uint32_t target_hash) const noexcept
{
  if (!is_object()) return npos;
  if (length_ == 0) return npos;

  const auto layout = object_layout();
  if (layout == ObjectLayout::Regular) {
    const auto entries = std::span(data_storage_.object_value, length_);
    for (size_t i = 0; i < entries.size(); ++i)
      if (entries[i].first.hash() == target_hash && entries[i].first.getString() == key) return i;
    return npos;
  }

  if (layout == ObjectLayout::CompactInline) {
    const auto entries = std::span(data_storage_.compact_object_value, length_);
    for (size_t i = 0; i < entries.size(); ++i)
      if (entries[i].key->hash == target_hash && entries[i].key->view() == key) return i;
    return npos;
  }

  if (layout == ObjectLayout::BlobByReference) {
    const auto entries_ptr = data_storage_.blob_ref_object_value;
    const auto entries = std::span(entries_ptr, length_);
    const auto packed_hash = blob_target_hash(target_hash);
    for (size_t i = 0; i < entries.size(); ++i)
      if (blob_key_hash(entries[i].key_meta) == packed_hash && blob_key_view(entries_ptr, entries[i]) == key) return i;
    return npos;
  }

  const auto entries = std::span(data_storage_.ref_value_object_value, length_);
  for (size_t i = 0; i < entries.size(); ++i)
    if (entries[i].first.hash() == target_hash && entries[i].first.getString() == key) return i;
  return npos;
}

template<typename CharType>
constexpr const basic_value_pair_t<CharType> *basic_json<CharType>::find_regular_entry(
  std::basic_string_view<CharType> key, uint32_t target_hash) const noexcept
{
  if (!is_object() || object_layout() != ObjectLayout::Regular || length_ == 0) return nullptr;

  if (is_sorted_obj()) {
    const auto index = find_sorted_entry_index(key);
    return index == npos ? nullptr : data_storage_.object_value + index;
  }

  const auto entries = std::span(data_storage_.object_value, length_);
  for (size_t i = 0; i < entries.size(); ++i)
    if (entries[i].first.hash() == target_hash && entries[i].first.getString() == key) return entries.data() + i;
  return nullptr;
}

template<typename CharType>
constexpr const basic_value_pair_t<CharType> *basic_json<CharType>::find_entry(
  std::basic_string_view<CharType> key, uint32_t target_hash) const noexcept
{
  const auto index = find_entry_index(key, target_hash);
  if (index == npos) return nullptr;

  if (object_layout() == ObjectLayout::Regular) return data_storage_.object_value + index;

  if consteval {
    detail::throw_exception<std::domain_error>("find_entry() is not constexpr for compact objects");
    return nullptr;
  }

  return detail::find_entry_cache<CharType>::store(this, index, materialize_entry(index));
}

template<typename CharType>
constexpr const basic_json<CharType> &basic_json<CharType>::at(std::integral auto index) const
{
  const auto t = type();
  if (t != Type::Array && t != Type::Object) [[unlikely]] {
    detail::throw_exception<std::domain_error>("JSON value is not an array or object");
    return null_value();
  }
  if (static_cast<size_t>(index) >= length_) [[unlikely]] {
    detail::throw_exception<std::out_of_range>("Index out of range");
    return null_value();
  }
  return t == Type::Array ? data_storage_.array_value[index] : entry_value(static_cast<size_t>(index));
}

template<typename CharType>
constexpr basic_items_t<CharType> basic_json<CharType>::items() const
{
  if (!is_object()) [[unlikely]] {
    detail::throw_exception<std::domain_error>("JSON value is not an object");
    return {};
  }
  const auto layout = object_layout();
  if (length_ == 0) return { this, nullptr, nullptr, 0, static_cast<uint8_t>(layout) };
  if (layout == ObjectLayout::Regular) {
    const auto entries = data_storage_.object_value;
    return { this, entries, &entries[0].second, sizeof(basic_value_pair_t<CharType>), static_cast<uint8_t>(layout) };
  }
  if (layout == ObjectLayout::CompactInline) {
    const auto entries = data_storage_.compact_object_value;
    return { this, entries, &entries[0].value, sizeof(basic_compact_value_pair_t<CharType>), static_cast<uint8_t>(layout) };
  }
  if (layout == ObjectLayout::BlobByReference) {
    const auto entries = data_storage_.blob_ref_object_value;
    return { this, entries, entries[0].value, sizeof(basic_blob_ref_value_pair_t<CharType>), static_cast<uint8_t>(layout) };
  }
  const auto entries = data_storage_.ref_value_object_value;
  return { this, entries, entries[0].second, sizeof(basic_ref_value_pair_t<CharType>), static_cast<uint8_t>(layout) };
}

template<typename CharType>
constexpr double basic_json<CharType>::getNumber() const
{
  switch (type()) {
  case Type::UInteger:
    return static_cast<double>(data_storage_.uint_value);
  case Type::Integer:
    return static_cast<double>(data_storage_.int_value);
  case Type::Float:
    return data_storage_.float_value;
  default:
    detail::throw_exception<std::domain_error>("JSON value is not a number");
    return 0.0;
  }
}

#ifdef JSON2CPP_USE_UTF16
using basicType = char16_t;
#else
using basicType = char;
#endif

using json = basic_json<basicType>;
using array_t = basic_array_t<basicType>;
using object_t = basic_object_t<basicType>;
using items_t = basic_items_t<basicType>;
using item_key_t = basic_item_key_t<basicType>;
using item_view_t = basic_item_view_t<basicType>;
using value_pair_t = basic_value_pair_t<basicType>;
using key_descriptor_t = basic_key_descriptor<basicType>;
using compact_value_pair_t = basic_compact_value_pair_t<basicType>;
using compact_object_t = basic_compact_object_t<basicType>;
using ref_value_pair_t = basic_ref_value_pair_t<basicType>;
using ref_value_object_t = basic_ref_value_object_t<basicType>;
using blob_ref_value_pair_t = basic_blob_ref_value_pair_t<basicType>;
using blob_ref_object_t = basic_blob_ref_object_t<basicType>;

}

#endif
