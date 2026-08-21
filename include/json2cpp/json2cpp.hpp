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
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include <stdexcept>

#if defined(_MSC_VER)
#define JSON2CPP_FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define JSON2CPP_FORCE_INLINE __attribute__((always_inline))
#else
#define JSON2CPP_FORCE_INLINE inline
#endif

namespace json2cpp {

template<typename CharType> struct basic_json;

namespace detail {
  template<typename Exception> [[noreturn]] constexpr void throw_exception(const char *msg)
  {
    if consteval {
      throw msg;
    } else {
      throw Exception(msg);
    }
  }

  [[noreturn]] constexpr void throw_out_of_range(const char *msg) { throw_exception<std::out_of_range>(msg); }

  [[noreturn]] constexpr void throw_domain_error(const char *msg) { throw_exception<std::domain_error>(msg); }

  template<typename CharType> constexpr uint32_t hash_key(std::basic_string_view<CharType> str) noexcept
  {
    uint32_t h = 0x811c9dc5;
    for (auto c : str) {
      uint32_t val = static_cast<uint32_t>(c);
      for (size_t i = 0; i < sizeof(CharType); ++i) {
        h ^= static_cast<uint8_t>(val & 0xFF);
        h *= 0x01000193;
        val >>= 8;
      }
    }
    const uint32_t result = (h ^ (h >> 28)) & 0x0FFFFFFF;
    return result != 0 ? result : 1;
  }

  template<typename T, typename CharType>
  concept string_like = !std::same_as<std::remove_cvref_t<T>, basic_json<CharType>>
                        && (std::convertible_to<T, std::basic_string_view<CharType>> || requires(const T &value) {
                             { value.data() } -> std::convertible_to<const CharType *>;
                             { value.size() } -> std::convertible_to<size_t>;
                           });

  template<typename CharType, typename T>
  JSON2CPP_FORCE_INLINE constexpr std::basic_string_view<CharType> make_string_view(const T &value) noexcept
  {
    if constexpr (std::convertible_to<T, std::basic_string_view<CharType>>)
      return std::basic_string_view<CharType>(value);
    else
      return { value.data(), static_cast<size_t>(value.size()) };
  }

  struct indexed_key_header
  {
    const uint8_t *data;
    uint16_t mask;
    uint32_t prefix;
  };
  struct indexed_value_header
  {
    const uint8_t *data;
  };

  template<typename CharType> struct CompileTimeKey
  {
    std::basic_string_view<CharType> value;
    uint32_t hash;

    template<size_t N>
    consteval CompileTimeKey(const CharType (&str)[N]) noexcept : value(str, N - 1), hash(hash_key(value))
    {}

    constexpr operator std::basic_string_view<CharType>() const noexcept { return value; }
  };
}// namespace detail

template<typename F, typename S> struct pair
{
  F first;
  S second;
};
template<typename CharType> using basic_value_pair_t = pair<basic_json<CharType>, basic_json<CharType>>;
template<typename CharType> using basic_object_t = std::span<const basic_value_pair_t<CharType>>;
template<typename CharType> struct basic_dense_object_t
{
  basic_object_t<CharType> value;
};
template<typename CharType> struct basic_indexed_object_ref_t
{
  const basic_value_pair_t<CharType> *entries;
  uint16_t size;
};
template<typename CharType> using basic_indexed_object_t = basic_indexed_object_ref_t<CharType>;
template<typename CharType> using basic_array_t = std::span<const basic_json<CharType>>;
template<typename CharType> using basic_items_t = basic_object_t<CharType>;
template<typename CharType> using basic_item_key_t = basic_json<CharType>;
template<typename CharType> using basic_item_view_t = basic_value_pair_t<CharType>;
template<typename CharType> using basic_entry_view_t = const basic_value_pair_t<CharType> *;

template<typename CharType> struct basic_json
{
  enum class Type : uint8_t { Null, Boolean, String, Integer, UInteger, Float, Array, Object };

private:
  static constexpr size_t capacity = sizeof(uint64_t) / sizeof(CharType);
  static constexpr uint32_t dense_object_mask = 1u << 4u;
  static constexpr uint32_t indexed_object_mask = 1u << 5u;

  uint32_t length_ = 0;
  uint32_t metadata_ = 0;
  union {
    const basic_json *array_value;
    const basic_value_pair_t<CharType> *object_value;
    const uint8_t *byte_data;
    const CharType *long_data;
    std::array<CharType, capacity> short_data;
    int64_t int_value;
    uint64_t uint_value;
    double float_value;
    bool boolean_value;
  } data_storage_;

  constexpr void set_metadata(Type t, size_t len, bool sorted, uint32_t hash_val) noexcept
  {
    length_ = static_cast<uint32_t>(len);
    metadata_ = (static_cast<uint32_t>(t) & 0b111) | ((static_cast<uint32_t>(sorted) & 0b1) << 3)
                | ((hash_val & 0x0FFFFFFF) << 4);
  }

  [[nodiscard]] constexpr bool is_dense_object() const noexcept
  {
    return is_object() && (metadata_ & dense_object_mask) != 0u;
  }

  [[nodiscard]] constexpr bool is_indexed_object() const noexcept
  {
    return is_object() && (metadata_ & indexed_object_mask) != 0u;
  }

  [[nodiscard]] constexpr const basic_value_pair_t<CharType> *object_entries() const noexcept
  {
    return data_storage_.object_value;
  }

  [[nodiscard]] constexpr size_t dense_index(std::basic_string_view<CharType> key) const noexcept
  {
    if (key.empty() || (key.size() > 1u && key.front() == static_cast<CharType>('0'))) return npos;
    size_t index = 0;
    for (const auto character : key) {
      if (character < static_cast<CharType>('0') || character > static_cast<CharType>('9')) return npos;
      const auto digit = static_cast<size_t>(character - static_cast<CharType>('0'));
      if (index > (std::numeric_limits<size_t>::max() - digit) / 10u) return npos;
      index = index * 10u + digit;
      if (index >= length_) return npos;
    }
    return index;
  }

  constexpr bool eq_bool(bool other) const noexcept { return is_boolean() && data_storage_.boolean_value == other; }

  constexpr bool eq_int(int64_t other) const noexcept
  {
    const auto t = type();
    return t == Type::Integer
             ? data_storage_.int_value == other
             : (t == Type::UInteger && other >= 0 && data_storage_.uint_value == static_cast<uint64_t>(other));
  }

  constexpr bool eq_uint(uint64_t other) const noexcept
  {
    const auto t = type();
    return t == Type::UInteger ? data_storage_.uint_value == other
                               : (t == Type::Integer && data_storage_.int_value >= 0
                                   && static_cast<uint64_t>(data_storage_.int_value) == other);
  }

  constexpr bool eq_float(double other) const noexcept
  {
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
  }

  static constexpr bool equal_string_storage(const basic_json &lhs, const basic_json &rhs) noexcept
  {
    if (lhs.length_ != rhs.length_ || lhs.metadata_ != rhs.metadata_) return false;
    return lhs.length_ <= capacity ? lhs.data_storage_.short_data == rhs.data_storage_.short_data
                                   : lhs.getString() == rhs.getString();
  }

  constexpr bool eq_string(std::basic_string_view<CharType> other) const noexcept
  {
    return is_string() && length_ == other.size() && getString() == other;
  }

  constexpr bool eq_json(const basic_json &other) const noexcept
  {
    const auto t = type();
    if (this == &other) return true;
    if (t != other.type() || length_ != other.length_) return false;
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
      return equal_string_storage(*this, other);
    case Type::Array:
      if (data_storage_.array_value == other.data_storage_.array_value) return true;
      for (size_t i = 0; i < length_; ++i)
        if (!(data_storage_.array_value[i] == other.data_storage_.array_value[i])) return false;
      return true;
    case Type::Object: {
      const auto lhs = object_entries();
      const auto rhs = other.object_entries();
      if (lhs == rhs) return true;
      if (is_dense_object() && other.is_dense_object()) {
        for (size_t i = 0; i < length_; ++i)
          if (!(lhs[i].second == rhs[i].second)) return false;
        return true;
      }
      for (size_t i = 0; i < length_; ++i) {
        const auto &a = lhs[i];
        const auto &b = rhs[i];
        if (!equal_string_storage(a.first, b.first) || !(a.second == b.second)) return false;
      }
      return true;
    }
    }
    return false;
  }

  [[nodiscard]] JSON2CPP_FORCE_INLINE constexpr const basic_value_pair_t<CharType> *
    find_regular_entry(std::basic_string_view<CharType> key, uint32_t target_hash) const noexcept
  {
    const auto entries = data_storage_.object_value;
    if (is_sorted_obj()) {
      size_t left = 0, right = length_;
      while (left < right) {
        const auto middle = left + (right - left) / 2u;
        if (entries[middle].first.getString() < key)
          left = middle + 1u;
        else
          right = middle;
      }
      if (left < length_) {
        const auto &found = entries[left].first;
        if (found.size() == key.size() && found.getString() == key) return entries + left;
      }
      return nullptr;
    }
    for (size_t i = 0; i < length_; ++i) {
      const auto &found = entries[i].first;
      if (found.hash() == target_hash && found.size() == key.size() && found.getString() == key) return entries + i;
    }
    return nullptr;
  }

  [[nodiscard]] JSON2CPP_FORCE_INLINE constexpr const basic_value_pair_t<CharType> *
    find_indexed_entry(std::basic_string_view<CharType> key, uint32_t target_hash) const noexcept
  {
    const auto entries = data_storage_.object_value;
    const auto &header = entries[-1];
    if ((header.first.metadata_ & (uint32_t{ 1 } << (target_hash & 31u))) != 0u) {
      for (size_t i = 1; i < length_ && i < 8u; ++i) {
        const auto &found = entries[i].first;
        if (found.hash() == target_hash && found.size() == key.size() && found.getString() == key) return entries + i;
      }
    }
    const auto key_index = header.first.data_storage_.byte_data;
    const auto mask = static_cast<uint16_t>(header.first.length_);
    auto slot = static_cast<uint16_t>(static_cast<uint16_t>(target_hash) & mask);
    for (;;) {
      const auto encoded = key_index[slot];
      if (encoded == 0u) return nullptr;
      const auto index = static_cast<size_t>(encoded - 1u);
      const auto &found = entries[index].first;
      if (found.hash() == target_hash && found.size() == key.size() && found.getString() == key) return entries + index;
      slot = static_cast<uint16_t>((slot + 1u) & mask);
    }
  }

public:
  static constexpr size_t npos = static_cast<size_t>(-1);
  static constexpr uint32_t calc_hash(std::basic_string_view<CharType> sv) noexcept { return detail::hash_key(sv); }
  static const basic_json &null_value() noexcept
  {
    static constexpr basic_json value{};
    return value;
  }

  [[nodiscard]] JSON2CPP_FORCE_INLINE constexpr const basic_value_pair_t<CharType> *
    find_entry(std::basic_string_view<CharType> key, uint32_t target_hash) const noexcept
  {
    constexpr auto object_mask = 0b111u | dense_object_mask | indexed_object_mask;
    constexpr auto object_tag = static_cast<uint32_t>(Type::Object);
    const auto kind = metadata_ & object_mask;
    if (kind == (object_tag | indexed_object_mask)) {
      const auto entries = data_storage_.object_value;
      if (length_ != 0u) {
        const auto &found = entries[0].first;
        if (found.hash() == target_hash && found.size() == key.size() && found.getString() == key) return entries;
      }
      return find_indexed_entry(key, target_hash);
    }
    if (kind == object_tag) [[likely]]
      return find_regular_entry(key, target_hash);
    if (kind == (object_tag | dense_object_mask)) {
      const auto index = dense_index(key);
      return index == npos ? nullptr : data_storage_.object_value + index;
    }
    return nullptr;
  }

  [[nodiscard]] JSON2CPP_FORCE_INLINE constexpr const basic_value_pair_t<CharType> *find_entry(
    std::basic_string_view<CharType> key) const noexcept
  {
    constexpr auto object_mask = 0b111u | dense_object_mask | indexed_object_mask;
    constexpr auto object_tag = static_cast<uint32_t>(Type::Object);
    constexpr auto indexed_tag = object_tag | indexed_object_mask;
    const auto kind = metadata_ & object_mask;
    if (length_ != 0u && kind == indexed_tag) {
      const auto entries = data_storage_.object_value;
      const auto &found = entries[0].first;
      if (found.size() == key.size() && found.getString() == key) return entries;
    }
    const auto target_hash = calc_hash(key);
    if (kind == object_tag) return find_regular_entry(key, target_hash);
    return find_entry(key, target_hash);
  }

  template<size_t N>
  [[nodiscard]] JSON2CPP_FORCE_INLINE constexpr const basic_value_pair_t<CharType> *find_entry(
    const CharType (&key)[N]) const noexcept
  {
    const std::basic_string_view<CharType> view(key, N - 1u);
    return find_entry(view, detail::hash_key(view));
  }

  template<typename Key>
  [[nodiscard]] JSON2CPP_FORCE_INLINE constexpr const basic_value_pair_t<CharType> *find_entry(
    const Key &key) const noexcept
    requires(detail::string_like<Key, CharType> && !std::convertible_to<Key, std::basic_string_view<CharType>>)
  {
    return find_entry(detail::make_string_view<CharType>(key));
  }

  [[nodiscard]] constexpr Type type() const noexcept { return static_cast<Type>(metadata_ & 0b111); }

  [[nodiscard]] constexpr size_t size() const noexcept { return length_; }

  [[nodiscard]] constexpr bool is_sorted_obj() const noexcept { return (metadata_ >> 3) & 0b1; }

  [[nodiscard]] constexpr uint32_t hash() const noexcept { return (metadata_ >> 4) & 0x0FFFFFFF; }

  constexpr basic_json() noexcept : length_(0), metadata_(0), data_storage_{ .short_data = {} } {}

  constexpr basic_json(std::nullptr_t) noexcept : data_storage_{ .short_data = {} }
  {
    set_metadata(Type::Null, 0, false, 0);
  }

  template<size_t N>
  constexpr basic_json(const CharType (&v)[N]) noexcept : basic_json(std::basic_string_view<CharType>(v, N - 1))
  {}

  constexpr basic_json(const CharType *v) noexcept : basic_json(std::basic_string_view<CharType>(v)) {}

  constexpr basic_json(bool v) noexcept : data_storage_{ .boolean_value = v }
  {
    set_metadata(Type::Boolean, 0, false, 0);
  }

  constexpr basic_json(std::signed_integral auto v) noexcept : data_storage_{ .int_value = v }
  {
    set_metadata(Type::Integer, 0, false, 0);
  }

  constexpr basic_json(std::unsigned_integral auto v) noexcept : data_storage_{ .uint_value = v }
  {
    set_metadata(Type::UInteger, 0, false, 0);
  }

  constexpr basic_json(std::floating_point auto v) noexcept : data_storage_{ .float_value = v }
  {
    set_metadata(Type::Float, 0, false, 0);
  }

  constexpr basic_json(basic_array_t<CharType> v) noexcept : data_storage_{ .array_value = v.data() }
  {
    set_metadata(Type::Array, v.size(), false, 0);
  }

  constexpr basic_json(basic_object_t<CharType> v) : data_storage_{ .object_value = v.data() }
  {
    bool sorted = true;
    for (size_t i = 0; i < v.size(); ++i) {
      if (!v[i].first.is_string()) [[unlikely]] { detail::throw_domain_error("JSON object keys must be strings"); }
      if (sorted && i < v.size() - 1 && v[i].first.getString() > v[i + 1].first.getString()) { sorted = false; }
    }
    set_metadata(Type::Object, v.size(), sorted, 0);
  }

  constexpr basic_json(basic_dense_object_t<CharType> v) : basic_json(v.value) { metadata_ |= dense_object_mask; }

  constexpr basic_json(basic_indexed_object_ref_t<CharType> v) noexcept : data_storage_{ .object_value = v.entries }
  {
    set_metadata(Type::Object, v.size, false, 0);
    metadata_ |= indexed_object_mask;
  }

  constexpr basic_json(detail::indexed_key_header v) noexcept
    : length_(v.mask), metadata_(v.prefix), data_storage_{ .byte_data = v.data }
  {}

  constexpr basic_json(detail::indexed_value_header v) noexcept : data_storage_{ .byte_data = v.data } {}

  constexpr basic_json(std::basic_string_view<CharType> v) noexcept
  {
    uint32_t hash_val = calc_hash(v);
    set_metadata(Type::String, v.size(), false, hash_val);

    if (v.size() <= capacity) {
      std::array<CharType, capacity> temp_short_data{};
      for (size_t i = 0; i < v.size(); ++i) temp_short_data[i] = v[i];
      data_storage_.short_data = temp_short_data;
    } else {
      data_storage_.long_data = v.data();
    }
  }

  [[nodiscard]] constexpr bool empty() const noexcept { return length_ == 0; }
  [[nodiscard]] constexpr bool is_object() const noexcept { return type() == Type::Object; }
  [[nodiscard]] constexpr bool is_array() const noexcept { return type() == Type::Array; }
  [[nodiscard]] constexpr bool is_string() const noexcept { return type() == Type::String; }
  [[nodiscard]] constexpr bool is_number() const noexcept
  {
    auto t = type();
    return t == Type::Integer || t == Type::UInteger || t == Type::Float;
  }
  [[nodiscard]] constexpr bool is_boolean() const noexcept { return type() == Type::Boolean; }
  [[nodiscard]] constexpr bool is_null() const noexcept { return type() == Type::Null; }

  [[nodiscard]] constexpr const basic_json &operator[](std::integral auto index) const { return at(index); }

  [[nodiscard]] JSON2CPP_FORCE_INLINE constexpr const basic_json &operator[](detail::CompileTimeKey<CharType> key) const
  {
    return at(key);
  }

  template<size_t N> [[nodiscard]] constexpr const basic_json &operator[](CharType (&key)[N]) const
  {
    return at(std::basic_string_view<CharType>(key, N - 1));
  }

  template<typename Key>
  [[nodiscard]] JSON2CPP_FORCE_INLINE constexpr const basic_json &operator[](const Key &key) const
    requires(detail::string_like<Key, CharType> && !std::is_array_v<std::remove_reference_t<Key>>)
  {
    return at(key);
  }

  constexpr bool operator==(const basic_json &other) const noexcept { return eq_json(other); }

  template<typename T> constexpr bool operator==(const T &other) const noexcept
  {
    if constexpr (std::is_same_v<T, bool>) {
      return eq_bool(other);
    } else if constexpr (std::is_integral_v<T>) {
      return std::is_signed_v<T> ? eq_int(other) : eq_uint(other);
    } else if constexpr (std::is_floating_point_v<T>) {
      return eq_float(other);
    } else if constexpr (std::convertible_to<T, std::basic_string_view<CharType>>) {
      return eq_string(std::basic_string_view<CharType>(other));
    } else {
      return false;
    }
  }

  constexpr const basic_json &at(std::integral auto index) const
  {
    const auto t = type();
    if (t != Type::Array && t != Type::Object) [[unlikely]]
      detail::throw_domain_error("JSON value is not an array or object");
    if (static_cast<size_t>(index) >= length_) [[unlikely]]
      detail::throw_out_of_range("Index out of range");
    return t == Type::Array ? data_storage_.array_value[index] : object_entries()[index].second;
  }

  template<size_t N> [[nodiscard]] constexpr const basic_json &at(CharType (&key)[N]) const
  {
    return at(std::basic_string_view<CharType>(key, N - 1));
  }

  [[nodiscard]] JSON2CPP_FORCE_INLINE constexpr const basic_json &at(detail::CompileTimeKey<CharType> key) const
  {
    if (auto *ptr = find_entry(key.value, key.hash)) [[likely]]
      return ptr->second;
    detail::throw_out_of_range("Key not found");
  }

  template<typename Key>
  [[nodiscard]] JSON2CPP_FORCE_INLINE constexpr const basic_json &at(const Key &key) const
    requires(detail::string_like<Key, CharType> && !std::is_array_v<std::remove_reference_t<Key>>)
  {
    if (auto *ptr = find_entry(detail::make_string_view<CharType>(key))) [[likely]]
      return ptr->second;
    detail::throw_out_of_range("Key not found");
  }

  template<typename Key>
  [[nodiscard]] JSON2CPP_FORCE_INLINE constexpr bool contains(Key &&key) const noexcept
    requires std::same_as<std::remove_cvref_t<Key>, std::basic_string_view<CharType>>
  {
    return find_entry(key) != nullptr;
  }

  template<typename Key>
  [[nodiscard]] JSON2CPP_FORCE_INLINE constexpr bool contains(const Key &key) const noexcept
    requires(detail::string_like<Key, CharType> && !std::convertible_to<Key, std::basic_string_view<CharType>>)
  {
    return find_entry(key) != nullptr;
  }

  [[nodiscard]] JSON2CPP_FORCE_INLINE constexpr bool contains(detail::CompileTimeKey<CharType> key) const noexcept
  {
    return find_entry(key.value, key.hash) != nullptr;
  }

  template<size_t N> [[nodiscard]] constexpr bool contains(CharType (&key)[N]) const noexcept
  {
    return contains(std::basic_string_view<CharType>(key, N - 1));
  }

  [[nodiscard]] constexpr size_t index(std::basic_string_view<CharType> value) const noexcept
  {
    const auto target_hash = calc_hash(value);
    if (is_array()) {
      for (size_t i = 0; i < length_; ++i) {
        const auto &current = data_storage_.array_value[i];
        if (current.is_string() && current.hash() == target_hash && current.getString() == value) return i;
      }
      return npos;
    }
    if (is_object()) {
      const auto entries = object_entries();
      const auto matches = [&](size_t i) {
        const auto &current = entries[i].second;
        return current.is_string() && current.hash() == target_hash && current.getString() == value;
      };
      if (is_indexed_object()) {
        const auto hashes = data_storage_.object_value[-1].second.data_storage_.byte_data;
        const auto fingerprint = static_cast<uint8_t>(target_hash);
        if consteval {
          for (size_t i = 0; i < length_; ++i)
            if (hashes[i] == fingerprint && matches(i)) return i;
        } else {
          auto first = hashes;
          auto remaining = static_cast<size_t>(length_);
          while (remaining != 0u) {
            const auto found = static_cast<const uint8_t *>(std::memchr(first, fingerprint, remaining));
            if (found == nullptr) return npos;
            const auto index = static_cast<size_t>(found - hashes);
            if (matches(index)) return index;
            first = found + 1;
            remaining = static_cast<size_t>(hashes + length_ - first);
          }
        }
        return npos;
      }
      for (size_t i = 0; i < length_; ++i)
        if (matches(i)) return i;
    }
    return npos;
  }

  [[nodiscard]] constexpr size_t index(const basic_json &value) const noexcept
  {
    if (is_array()) {
      if (&value >= data_storage_.array_value && &value < data_storage_.array_value + length_)
        return static_cast<size_t>(&value - data_storage_.array_value);
      for (size_t i = 0; i < length_; ++i)
        if (data_storage_.array_value[i] == value) return i;
      return npos;
    }
    if (is_object()) {
      const auto entries = object_entries();
      if (length_ != 0u) {
        const auto address = reinterpret_cast<std::uintptr_t>(&value);
        const auto first = reinterpret_cast<std::uintptr_t>(&entries[0].second);
        if (address >= first) {
          const auto distance = address - first;
          if (distance % sizeof(basic_value_pair_t<CharType>) == 0u
              && distance / sizeof(basic_value_pair_t<CharType>) < length_)
            return distance / sizeof(basic_value_pair_t<CharType>);
        }
      }
      for (size_t i = 0; i < length_; ++i)
        if (entries[i].second == value) return i;
    }
    return npos;
  }

  template<typename T> [[nodiscard]] constexpr size_t index(const T &value) const noexcept
  {
    if constexpr (detail::string_like<T, CharType>) {
      return index(detail::make_string_view<CharType>(value));
    } else if (is_array()) {
      for (size_t i = 0; i < length_; ++i)
        if (data_storage_.array_value[i] == value) return i;
    } else if (is_object()) {
      const auto entries = object_entries();
      for (size_t i = 0; i < length_; ++i)
        if (entries[i].second == value) return i;
    }
    return npos;
  }

  [[nodiscard]] constexpr basic_object_t<CharType> items() const
  {
    if (!is_object()) [[unlikely]]
      detail::throw_domain_error("JSON value is not an object");
    return { object_entries(), length_ };
  }

  [[nodiscard]] constexpr const basic_json *begin() const noexcept
  {
    return is_array() ? data_storage_.array_value : nullptr;
  }

  [[nodiscard]] constexpr const basic_json *end() const noexcept
  {
    if (!is_array()) return nullptr;
    return length_ == 0u ? data_storage_.array_value : data_storage_.array_value + length_;
  }

  constexpr operator std::span<const basic_json>() const noexcept
  {
    if (!is_array()) return {};
    return { data_storage_.array_value, length_ };
  }

  [[nodiscard]] constexpr const CharType *data() const noexcept
  {
    return length_ <= capacity ? data_storage_.short_data.data() : data_storage_.long_data;
  }

  [[nodiscard]] constexpr std::basic_string_view<CharType> getString() const noexcept { return { data(), length_ }; }

  [[nodiscard]] constexpr double getNumber() const
  {
    switch (type()) {
    case Type::UInteger:
      return static_cast<double>(data_storage_.uint_value);
    case Type::Integer:
      return static_cast<double>(data_storage_.int_value);
    case Type::Float:
      return data_storage_.float_value;
    default:
      detail::throw_domain_error("JSON value is not a number");
    }
  }

  template<typename T> [[nodiscard]] constexpr T get() const
  {
    if constexpr (std::is_same_v<T, std::basic_string_view<CharType>>) {
      if (!is_string()) [[unlikely]]
        detail::throw_domain_error("JSON value is not a string");
      return getString();
    } else if constexpr (std::is_same_v<T, bool>) {
      if (!is_boolean()) [[unlikely]]
        detail::throw_domain_error("JSON value is not a boolean");
      return data_storage_.boolean_value;
    } else if constexpr (std::is_floating_point_v<T>) {
      return static_cast<T>(getNumber());
    } else if constexpr (std::is_integral_v<T>) {
      const auto t = type();
      if (t != Type::Integer && t != Type::UInteger && t != Type::Float) [[unlikely]]
        detail::throw_domain_error("JSON value is not a number");
      if (t == Type::Integer) return static_cast<T>(data_storage_.int_value);
      if (t == Type::UInteger) return static_cast<T>(data_storage_.uint_value);
      return static_cast<T>(data_storage_.float_value);
    } else {
      static_assert(sizeof(T) == 0, "Unsupported type for get<T>()");
    }
  }
};

namespace detail {
  template<typename CharType, size_t N>
  consteval auto make_key_index(const std::array<basic_value_pair_t<CharType>, N> &entries)
  {
    static_assert(N <= 0xFFu);
    constexpr auto capacity = [] {
      size_t value = 1;
      while (value < N * 2u) value *= 2u;
      return value;
    }();
    std::array<uint8_t, capacity> result{};
    for (size_t i = 0; i < N; ++i) {
      auto slot = entries[i].first.hash() & (capacity - 1u);
      while (result[slot] != 0u) slot = (slot + 1u) & (capacity - 1u);
      result[slot] = static_cast<uint8_t>(i + 1u);
    }
    return result;
  }

  template<typename CharType, size_t N>
  consteval uint32_t make_key_prefix_mask(const std::array<basic_value_pair_t<CharType>, N> &entries)
  {
    uint32_t result = 0;
    for (size_t i = 0; i < N && i < 8u; ++i) result |= uint32_t{ 1 } << (entries[i].first.hash() & 31u);
    return result;
  }

  template<typename CharType, size_t N, size_t KeySize>
  consteval auto make_indexed_entries(const std::array<basic_value_pair_t<CharType>, N> &entries,
    const std::array<uint8_t, KeySize> &key_index,
    const std::array<uint8_t, N> &value_hashes)
  {
    std::array<basic_value_pair_t<CharType>, N + 1u> result{};
    result[0] = { basic_json<CharType>{
                    indexed_key_header{ key_index.data(), KeySize - 1u, make_key_prefix_mask(entries) } },
      basic_json<CharType>{ indexed_value_header{ value_hashes.data() } } };
    for (size_t i = 0; i < N; ++i) result[i + 1u] = entries[i];
    return result;
  }

  template<typename CharType, size_t N>
  consteval auto make_value_hashes(const std::array<basic_value_pair_t<CharType>, N> &entries)
  {
    std::array<uint8_t, N> result{};
    for (size_t i = 0; i < N; ++i)
      result[i] = entries[i].second.is_string() ? static_cast<uint8_t>(entries[i].second.hash()) : 0u;
    return result;
  }
}// namespace detail

#ifdef JSON2CPP_USE_UTF16
using basicType = char16_t;
#else
using basicType = char;
#endif

using json = basic_json<basicType>;
using array_t = basic_array_t<basicType>;
using object_t = basic_object_t<basicType>;
using dense_object_t = basic_dense_object_t<basicType>;
using indexed_object_t = basic_indexed_object_t<basicType>;
using indexed_object_ref_t = basic_indexed_object_ref_t<basicType>;
using items_t = basic_items_t<basicType>;
using item_key_t = basic_item_key_t<basicType>;
using item_view_t = basic_item_view_t<basicType>;
using entry_view_t = basic_entry_view_t<basicType>;
using value_pair_t = basic_value_pair_t<basicType>;

#undef JSON2CPP_FORCE_INLINE

}// namespace json2cpp

#endif
