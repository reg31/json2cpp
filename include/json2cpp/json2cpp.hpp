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

#ifndef CONSTEXPR_JSON_HPP_INCLUDED
#define CONSTEXPR_JSON_HPP_INCLUDED

#include <algorithm>
#include <array>
#include <cstdint>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>

namespace json2cpp {

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
private:
  enum class Type : uint8_t { Null, Boolean, String, Integer, UInteger, Float, Array, Object };

  static constexpr size_t TYPE_SHIFT = sizeof(size_t) * 8 - 4;
  static constexpr size_t LENGTH_MASK = (1ULL << TYPE_SHIFT) - 1;
  static constexpr size_t capacity = sizeof(CharType *) / sizeof(CharType);

  size_t type_and_length_ = 0;

  union Data {
    const basic_json<CharType> *array_value;
    const basic_value_pair_t<CharType> *object_value;
    const CharType *long_data;
    std::array<CharType, capacity> short_data{};
    int64_t int_value;
    uint64_t uint_value;
    double float_value;
    bool boolean_value;
  } data_;

  constexpr void set_type_and_length(Type t, size_t len) noexcept
  {
    type_and_length_ = (static_cast<size_t>(t) << TYPE_SHIFT) | (len & LENGTH_MASK);
  }

  constexpr Type type() const noexcept { return static_cast<Type>(type_and_length_ >> TYPE_SHIFT); }

  constexpr const CharType *string_data() const noexcept
  {
    return size() <= capacity ? data_.short_data.data() : data_.long_data;
  }

  constexpr basic_array_t<CharType> array_data() const { return { data_.array_value, size() }; }

  constexpr basic_object_t<CharType> object_data() const { return { data_.object_value, size() }; }

  inline constexpr const basic_json *find_value(std::basic_string_view<CharType> key) const noexcept
  {
    if (is_object()) {
      const auto obj = object_data();
      const auto it = std::ranges::find_if(obj, [key](const auto &pair) { return pair.first.getString() == key; });
      return (it != obj.end()) ? &it->second : nullptr;
    } else if (is_array()) {
      const auto arr = array_data();
      const auto it = std::ranges::find_if(
        arr, [key](const auto &element) { return element.is_string() && element.getString() == key; });
      return (it != arr.end()) ? &(*it) : nullptr;
    }
    return nullptr;
  }

  inline constexpr void check_bounds(size_t index) const
  {
    if (index >= size()) throw std::runtime_error("Index out of range");
  }

public:
  constexpr basic_json() noexcept { set_type_and_length(Type::Null, 0); }
  constexpr basic_json(std::nullptr_t) noexcept { set_type_and_length(Type::Null, 0); }
  constexpr basic_json(bool v) noexcept : data_{ .boolean_value = v } { set_type_and_length(Type::Boolean, 0); }
  constexpr basic_json(basic_array_t<CharType> v) noexcept : data_{ .array_value = v.data() }
  {
    set_type_and_length(Type::Array, v.size());
  }
  constexpr basic_json(basic_object_t<CharType> v) noexcept : data_{ .object_value = v.data() }
  {
    set_type_and_length(Type::Object, v.size());
  }
  constexpr basic_json(int64_t v) noexcept : data_{ .int_value = v } { set_type_and_length(Type::Integer, 0); }
  constexpr basic_json(uint64_t v) noexcept : data_{ .uint_value = v } { set_type_and_length(Type::UInteger, 0); }
  constexpr basic_json(double v) noexcept : data_{ .float_value = v } { set_type_and_length(Type::Float, 0); }

  inline constexpr basic_json(std::basic_string_view<CharType> v) noexcept
  {
    const size_t len = v.size();
    set_type_and_length(Type::String, len);

    if (len <= capacity)
      std::ranges::copy(v, data_.short_data.begin());
    else
      data_.long_data = v.data();
  }

  constexpr size_t size() const noexcept { return type_and_length_ & LENGTH_MASK; }
  constexpr bool empty() const noexcept { return size() == 0; }

  constexpr bool is_object() const noexcept { return type() == Type::Object; }
  constexpr bool is_array() const noexcept { return type() == Type::Array; }
  constexpr bool is_string() const noexcept { return type() == Type::String; }
  constexpr bool is_number() const noexcept { return type() >= Type::Integer && type() <= Type::Float; }
  constexpr bool is_boolean() const noexcept { return type() == Type::Boolean; }
  constexpr bool is_null() const noexcept { return type() == Type::Null; }

  constexpr const basic_json &operator[](std::basic_string_view<CharType> key) const { return at(key); }

  constexpr const basic_json &operator[](size_t index) const { return at(index); }

  template<typename ValueType> inline constexpr bool operator==(const ValueType &other) const
  {
    return is_string() && size() == other.length() && std::equal(string_data(), string_data() + size(), other.data());
  }

  constexpr const basic_json &at(size_t index) const
  {
    check_bounds(index);
    return is_array() ? array_data()[index] : object_data()[index].second;
  }

  inline constexpr const basic_json &at(std::basic_string_view<CharType> key) const
  {
    const auto *result = find_value(key);
    if (!result) throw std::out_of_range("Key not found");
    return *result;
  }

  constexpr bool contains(std::basic_string_view<CharType> key) const noexcept { return find_value(key) != nullptr; }

  constexpr basic_object_t<CharType> items() const { return object_data(); }
  constexpr auto begin() const { return array_data().begin(); }
  constexpr auto end() const { return array_data().end(); }

  constexpr std::basic_string_view<CharType> getString() const { return { string_data(), size() }; }

  inline constexpr double getNumber() const
  {
    return type() == Type::UInteger  ? static_cast<double>(data_.uint_value)
           : type() == Type::Integer ? static_cast<double>(data_.int_value)
           : type() == Type::Float   ? data_.float_value
                                     : throw std::runtime_error("Not a number");
  }

  template<typename T> inline constexpr T get() const
  {
    if constexpr (std::is_same_v<T, std::basic_string_view<CharType>>) {
      return getString();
    } else if constexpr (std::is_same_v<T, bool>) {
      return data_.boolean_value;
    } else if constexpr (std::is_arithmetic_v<T>) {
      return static_cast<T>(getNumber());
    } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
      return nullptr;
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

#endi
