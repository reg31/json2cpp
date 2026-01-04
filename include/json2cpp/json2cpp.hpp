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
  struct Metadata
  {
    static constexpr uint32_t HashBits = 28;
    static constexpr uint64_t TypeMask = 0b111, LenMask = 0xFFFF'FFFF, HashMask = 0xFFF'FFFF;
    static constexpr size_t SortedShift = 3, LenShift = 4, HashShift = 36;
  };

  static constexpr size_t capacity = sizeof(uint64_t) / sizeof(CharType);

  uint64_t type_and_length_ = 0;
  union {
    const basic_json *array_value;
    const basic_value_pair_t<CharType> *object_value;
    const CharType *long_data;
    std::array<CharType, capacity> short_data;
    int64_t int_value;
    uint64_t uint_value;
    double float_value;
    bool boolean_value;
  };

  constexpr void set_metadata(Type t, size_t len, bool sorted, uint32_t hash) noexcept
  {
    type_and_length_ = (static_cast<uint64_t>(std::to_underlying(t)) & Metadata::TypeMask)
                       | ((static_cast<uint64_t>(sorted) & 1) << Metadata::SortedShift)
                       | ((static_cast<uint64_t>(len) & Metadata::LenMask) << Metadata::LenShift)
                       | ((static_cast<uint64_t>(hash) & Metadata::HashMask) << Metadata::HashShift);
  }

  constexpr bool eq_bool(bool other) const noexcept { return is_boolean() && boolean_value == other; }

  constexpr bool eq_int(int64_t other) const noexcept
  {
    return type() == Type::Integer
             ? int_value == other
             : (type() == Type::UInteger && other >= 0 && uint_value == static_cast<uint64_t>(other));
  }

  constexpr bool eq_uint(uint64_t other) const noexcept
  {
    return type() == Type::UInteger
             ? uint_value == other
             : (type() == Type::Integer && int_value >= 0 && static_cast<uint64_t>(int_value) == other);
  }

  constexpr bool eq_float(double other) const noexcept
  {
    switch (type()) {
    case Type::Float:
      return float_value == other;
    case Type::Integer:
      return static_cast<double>(int_value) == other;
    case Type::UInteger:
      return static_cast<double>(uint_value) == other;
    default:
      return false;
    }
  }

  constexpr bool eq_string(std::basic_string_view<CharType> other) const noexcept
  {
    return is_string() && size() == other.size() && getString() == other;
  }

public:
  static constexpr uint32_t calc_hash(std::basic_string_view<CharType> input_string) noexcept
  {
    uint32_t hash_value = 2166136261u;
    for (auto character : input_string) {
      hash_value ^= static_cast<uint32_t>(character);
      hash_value *= 16777619u;
    }
    return (hash_value ^ (hash_value >> Metadata::HashBits)) & Metadata::HashMask;
  }

  [[nodiscard]] constexpr const basic_value_pair_t<CharType> *find_entry(
    std::basic_string_view<CharType> key) const noexcept
  {
    if (!is_object()) return nullptr;
    uint32_t target_hash = calc_hash(key);
    size_t count = size();

    if (is_sorted_obj()) {
      size_t left = 0, right = count;
      while (left < right) {
        size_t mid = left + (right - left) / 2;
        if (object_value[mid].first.getString() < key)
          left = mid + 1;
        else
          right = mid;
      }
      if (left < count) {
        const auto &found = object_value[left].first;
        if (found.hash() == target_hash && found.size() == key.size() && found.getString() == key)
          return &object_value[left];
      }
    } else {
      for (size_t i = 0; i < count; ++i) {
        const auto &k = object_value[i].first;
        if (k.hash() == target_hash && k.size() == key.size() && k.getString() == key) return &object_value[i];
      }
    }
    return nullptr;
  }

  [[nodiscard]] constexpr Type type() const noexcept
  {
    return static_cast<Type>(type_and_length_ & Metadata::TypeMask);
  }

  [[nodiscard]] constexpr size_t size() const noexcept
  {
    return static_cast<size_t>((type_and_length_ >> Metadata::LenShift) & Metadata::LenMask);
  }

  [[nodiscard]] constexpr bool is_sorted_obj() const noexcept
  {
    return (type_and_length_ >> Metadata::SortedShift) & 1;
  }

  [[nodiscard]] constexpr uint32_t hash() const noexcept
  {
    return static_cast<uint32_t>((type_and_length_ >> Metadata::HashShift) & Metadata::HashMask);
  }

  constexpr basic_json() noexcept : short_data{} {}

  constexpr basic_json(bool v) noexcept : boolean_value(v) { set_metadata(Type::Boolean, 0, false, 0); }

  constexpr basic_json(std::signed_integral auto v) noexcept : int_value(v)
  {
    set_metadata(Type::Integer, 0, false, 0);
  }

  constexpr basic_json(std::unsigned_integral auto v) noexcept : uint_value(v)
  {
    set_metadata(Type::UInteger, 0, false, 0);
  }

  constexpr basic_json(std::floating_point auto v) noexcept : float_value(v) { set_metadata(Type::Float, 0, false, 0); }

  constexpr basic_json(basic_array_t<CharType> v) noexcept : array_value(v.data())
  {
    set_metadata(Type::Array, v.size(), false, 0);
  }

  constexpr basic_json(basic_object_t<CharType> v) noexcept : object_value(v.data())
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

  constexpr basic_json(std::basic_string_view<CharType> v) noexcept : short_data{}
  {
    set_metadata(Type::String, v.size(), false, calc_hash(v));
    if (v.size() <= capacity) {
      for (size_t i = 0; i < v.size(); ++i) short_data[i] = v[i];
    } else {
      long_data = v.data();
    }
  }

  [[nodiscard]] constexpr bool empty() const noexcept { return size() == 0; }
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

  [[nodiscard]] constexpr const basic_json &operator[](
    std::convertible_to<std::basic_string_view<CharType>> auto &&key) const
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
    if (index >= size()) [[unlikely]]
      detail::throw_out_of_range("Index out of range");
    return is_array() ? array_value[index] : object_value[index].second;
  }

  constexpr const basic_json &at(std::convertible_to<std::basic_string_view<CharType>> auto &&key) const
  {
    if (auto *ptr = find_entry(key)) [[likely]]
      return ptr->second;
    detail::throw_out_of_range("Key not found");
  }

  [[nodiscard]] constexpr bool contains(std::basic_string_view<CharType> key) const noexcept
  {
    return find_entry(key) != nullptr;
  }

  [[nodiscard]] constexpr basic_object_t<CharType> items() const { return { object_value, size() }; }

  [[nodiscard]] constexpr auto begin() const noexcept { return is_array() ? array_value : nullptr; }

  [[nodiscard]] constexpr auto end() const noexcept { return is_array() ? array_value + size() : nullptr; }

  constexpr operator std::span<const basic_json>() const { return { begin(), end() }; }

  [[nodiscard]] constexpr const CharType *data() const noexcept
  {
    return size() <= capacity ? short_data.data() : long_data;
  }

  [[nodiscard]] constexpr std::basic_string_view<CharType> getString() const noexcept { return { data(), size() }; }

  [[nodiscard]] constexpr double getNumber() const
  {
    switch (type()) {
    case Type::UInteger:
      return static_cast<double>(uint_value);
    case Type::Integer:
      return static_cast<double>(int_value);
    case Type::Float:
      return float_value;
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
      return boolean_value;
    } else if constexpr (std::is_floating_point_v<T>) {
      return static_cast<T>(getNumber());
    } else if constexpr (std::is_integral_v<T>) {
      if (!is_number()) [[unlikely]]
        detail::throw_domain_error("JSON value is not a number");
      if (type() == Type::Integer) return static_cast<T>(int_value);
      if (type() == Type::UInteger) return static_cast<T>(uint_value);
      return static_cast<T>(float_value);
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
