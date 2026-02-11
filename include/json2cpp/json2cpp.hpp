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
#include <bit>
#include <concepts>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace json2cpp {

namespace detail {
  [[noreturn]] constexpr void throw_out_of_range(const char *msg) { throw std::out_of_range(msg); }
  [[noreturn]] constexpr void throw_domain_error(const char *msg) { throw std::domain_error(msg); }
  [[noreturn]] constexpr void throw_invalid_arg(const char *msg) { throw std::invalid_argument(msg); }

  template<typename CharType> constexpr uint32_t hash_key(std::basic_string_view<CharType> str) noexcept
  {
    uint32_t hash = 0x811c9dc5;

    for (auto character : str) {
      hash ^= static_cast<uint8_t>(character);
      hash *= 0x01000193;
    }

    return (hash ^ (hash >> 28)) & 0x0FFFFFFF;
  }

  template<typename CharType, size_t N> struct CompileTimeKey
  {
    std::basic_string_view<CharType> value;
    uint32_t hash;

    constexpr CompileTimeKey(const CharType (&str)[N]) noexcept : value(str, N - 1), hash(hash_key(value)) {}

    constexpr operator std::basic_string_view<CharType>() const noexcept { return value; }
  };
}// namespace detail

template<typename CharType> struct basic_json;
template<typename F, typename S> struct pair
{
  F first;
  [[no_unique_address]] S second;
};
template<typename CharType> using basic_value_pair_t = pair<basic_json<CharType>, basic_json<CharType>>;
template<typename CharType> using basic_object_t = std::span<const basic_value_pair_t<CharType>>;
template<typename CharType> using basic_array_t = std::span<const basic_json<CharType>>;

template<typename CharType> struct basic_json
{
  enum class Type : uint8_t { Null, Boolean, String, Integer, UInteger, Float, Array, Object };

private:
  static constexpr size_t capacity = sizeof(uint64_t) / sizeof(CharType);

  uint32_t length_ = 0;
  uint32_t metadata_ = 0;
  union {
    const basic_json *array_value;
    const basic_value_pair_t<CharType> *object_value;
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
    metadata_ = (static_cast<uint32_t>(std::to_underlying(t)) & 0b111) | ((static_cast<uint32_t>(sorted) & 0b1) << 3)
                | ((hash_val & 0x0FFFFFFF) << 4);
  }

  constexpr bool eq_bool(bool other) const noexcept { return is_boolean() && data_storage_.boolean_value == other; }

  constexpr bool eq_int(int64_t other) const noexcept
  {
    return type() == Type::Integer
             ? data_storage_.int_value == other
             : (type() == Type::UInteger && other >= 0 && data_storage_.uint_value == static_cast<uint64_t>(other));
  }

  constexpr bool eq_uint(uint64_t other) const noexcept
  {
    return type() == Type::UInteger ? data_storage_.uint_value == other
                                    : (type() == Type::Integer && data_storage_.int_value >= 0
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

  constexpr bool eq_string(std::basic_string_view<CharType> other) const noexcept
  {
    return is_string() && length_ == other.size() && getString() == other;
  }

public:
  static constexpr uint32_t calc_hash(std::basic_string_view<CharType> input_string) noexcept
  {
    return detail::hash_key(input_string);
  }

  [[nodiscard]] constexpr const basic_value_pair_t<CharType> *find_entry(std::basic_string_view<CharType> key,
    uint32_t target_hash) const noexcept
  {
    if (!is_object()) return nullptr;
    size_t count = length_;

    if (is_sorted_obj()) {
      size_t left = 0, right = count;
      while (left < right) {
        size_t mid = left + (right - left) / 2;
        if (data_storage_.object_value[mid].first.getString() < key)
          left = mid + 1;
        else
          right = mid;
      }
      if (left < count) {
        const auto &found = data_storage_.object_value[left].first;
        if (found.hash() == target_hash && found.size() == key.size() && found.getString() == key)
          return &data_storage_.object_value[left];
      }
    } else {
      for (size_t i = 0; i < count; ++i) {
        const auto &k = data_storage_.object_value[i].first;
        if (k.hash() == target_hash && k.size() == key.size() && k.getString() == key)
          return &data_storage_.object_value[i];
      }
    }
    return nullptr;
  }

  [[nodiscard]] constexpr const basic_value_pair_t<CharType> *find_entry(
    std::basic_string_view<CharType> key) const noexcept
  {
    return find_entry(key, calc_hash(key));
  }

  template<size_t N>
  [[nodiscard]] constexpr const basic_value_pair_t<CharType> *find_entry(
    const detail::CompileTimeKey<CharType, N> &key) const noexcept
  {
    return find_entry(key.value, key.hash);
  }

  [[nodiscard]] constexpr Type type() const noexcept { return static_cast<Type>(metadata_ & 0b111); }

  [[nodiscard]] constexpr size_t size() const noexcept { return length_; }

  [[nodiscard]] constexpr bool is_sorted_obj() const noexcept { return (metadata_ >> 3) & 0b1; }

  [[nodiscard]] constexpr uint32_t hash() const noexcept
  {
    uint32_t stored_hash = (metadata_ >> 4) & 0x0FFFFFFF;
    return stored_hash != 0 ? stored_hash : calc_hash(getString());
  }

  constexpr basic_json() noexcept : length_(0), metadata_(0), data_storage_{ .short_data = {} } {}

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

  constexpr basic_json(basic_object_t<CharType> v) noexcept : data_storage_{ .object_value = v.data() }
  {
    bool sorted = true;
    if (v.size() > 1) {
      for (size_t i = 0; i < v.size() - 1; ++i) {
        if (v[i].first.getString() > v[i + 1].first.getString()) {
          sorted = false;
          break;
        }
      }
    }
    set_metadata(Type::Object, v.size(), sorted, 0);
  }

  constexpr basic_json(std::basic_string_view<CharType> v) noexcept
  {
    uint32_t hash_val = v.size() > capacity ? calc_hash(v) : 0;
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
    return t >= Type::Integer && t <= Type::Float;
  }
  [[nodiscard]] constexpr bool is_boolean() const noexcept { return type() == Type::Boolean; }
  [[nodiscard]] constexpr bool is_null() const noexcept { return type() == Type::Null; }

  [[nodiscard]] constexpr const basic_json &operator[](std::integral auto index) const { return at(index); }

  template<size_t N> [[nodiscard]] constexpr const basic_json &operator[](const CharType (&key)[N]) const
  {
    return at(detail::CompileTimeKey<CharType, N>(key));
  }

  [[nodiscard]] constexpr const basic_json &operator[](
    std::convertible_to<std::basic_string_view<CharType>> auto &&key) const
    requires(!std::is_array_v<std::remove_reference_t<decltype(key)>>)
  {
    return at(key);
  }

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
    if (index >= length_) [[unlikely]]
      detail::throw_out_of_range("Index out of range");
    return is_array() ? data_storage_.array_value[index] : data_storage_.object_value[index].second;
  }

  template<size_t N> [[nodiscard]] constexpr const basic_json &at(const CharType (&key)[N]) const
  {
    return at(detail::CompileTimeKey<CharType, N>(key));
  }

  template<size_t N> [[nodiscard]] constexpr const basic_json &at(const detail::CompileTimeKey<CharType, N> &key) const
  {
    if (auto *ptr = find_entry(key)) [[likely]]
      return ptr->second;
    detail::throw_out_of_range("Key not found");
  }

  [[nodiscard]] constexpr const basic_json &at(std::convertible_to<std::basic_string_view<CharType>> auto &&key) const
    requires(!std::is_array_v<std::remove_reference_t<decltype(key)>>)
  {
    if (auto *ptr = find_entry(key)) [[likely]]
      return ptr->second;
    detail::throw_out_of_range("Key not found");
  }

  [[nodiscard]] constexpr bool contains(std::basic_string_view<CharType> key) const noexcept
  {
    return find_entry(key) != nullptr;
  }

  template<size_t N> [[nodiscard]] constexpr bool contains(const CharType (&key)[N]) const noexcept
  {
    return find_entry(detail::CompileTimeKey<CharType, N>(key)) != nullptr;
  }

  [[nodiscard]] constexpr basic_object_t<CharType> items() const { return { data_storage_.object_value, length_ }; }

  [[nodiscard]] constexpr auto begin() const noexcept { return is_array() ? data_storage_.array_value : nullptr; }

  [[nodiscard]] constexpr auto end() const noexcept
  {
    return is_array() ? data_storage_.array_value + length_ : nullptr;
  }

  constexpr operator std::span<const basic_json>() const { return { begin(), end() }; }

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
    } else if constexpr (std::is_integral_v<T>) {
      if (!is_number()) [[unlikely]]
        detail::throw_domain_error("JSON value is not a number");
      if (type() == Type::Integer) return static_cast<T>(data_storage_.int_value);
      if (type() == Type::UInteger) return static_cast<T>(data_storage_.uint_value);
      return static_cast<T>(data_storage_.float_value);
    } else {
      detail::throw_invalid_arg("Unsupported type for get()");
    }
  }
};

#ifdef JSON2CPP_USE_UTF16
using basicType = char16_t;
#else
using basicType = char;
#endif

using json = basic_json<basicType>;
using array_t = basic_array_t<basicType>;
using object_t = basic_object_t<basicType>;
using value_pair_t = basic_value_pair_t<basicType>;

}// namespace json2cpp

#endif
