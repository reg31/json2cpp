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
#include <stdexcept>
#include <string_view>

namespace json2cpp {

template<typename CharType> struct basic_json;

template<typename F, typename S> struct pair
{
  F first;
  [[no_unique_address]] S second;
};

template<typename T> struct span
{
  const T *data_{ nullptr };
  size_t count_{ 0 };

  template<std::size_t Size>
  constexpr explicit span(const std::array<T, Size> &input) noexcept : data_{ input.data() }, count_{ Size }
  {}

  constexpr span() noexcept = default;

  constexpr const T *begin() const noexcept { return data_; }
  constexpr const T *end() const noexcept { return data_ + count_; }
  constexpr const T &operator[](std::size_t index) const { return data_[index]; }
  constexpr std::size_t size() const noexcept { return count_; }
  constexpr const T *data() const noexcept { return data_; }
};

template<typename CharType> struct StringStorage
{
  static constexpr size_t capacity = sizeof(CharType *) / sizeof(CharType);

  union {
    std::array<CharType, capacity> short_data{};
    const CharType *long_data;
  };

  size_t length = 0;
  constexpr StringStorage() noexcept = default;
  constexpr StringStorage(std::basic_string_view<CharType> sv) noexcept : length{ sv.size() }
  {
    if (length <= capacity) {
      std::copy_n(sv.begin(), length, short_data.begin());
    } else {
      long_data = sv.data();
    }
  }

  constexpr std::basic_string_view<CharType> get() const noexcept
  {
    return std::basic_string_view<CharType>{ length <= capacity ? short_data.data() : long_data, length };
  }

  constexpr size_t size() const noexcept { return length; }
};

template<typename CharType> using basic_value_pair_t = pair<std::basic_string_view<CharType>, basic_json<CharType>>;
template<typename CharType> using basic_object_t = span<basic_value_pair_t<CharType>>;
template<typename CharType> using basic_array_t = span<basic_json<CharType>>;

template<typename CharType> struct basic_json
{
  struct iterator
  {
    enum class Kind : uint8_t { Array, Object };
    Kind kind;
    union {
      const basic_json<CharType> *arr_it;
      const basic_value_pair_t<CharType> *obj_it;
    };

    constexpr const basic_json<CharType> *operator->() const { return &(operator*()); }

    constexpr iterator(const basic_json<CharType> *it) noexcept : kind(Kind::Array), arr_it(it) {}

    constexpr iterator(const basic_value_pair_t<CharType> *it) noexcept : kind(Kind::Object), obj_it(it) {}

    constexpr const basic_json<CharType> &operator*() const { return kind == Kind::Array ? *arr_it : obj_it->second; }

    constexpr iterator &operator++() noexcept
    {
      if (kind == Kind::Array)
        ++arr_it;
      else
        ++obj_it;
      return *this;
    }

    constexpr bool operator==(const iterator &other) const noexcept
    {
      return kind == other.kind && (kind == Kind::Array ? arr_it == other.arr_it : obj_it == other.obj_it);
    }

    constexpr bool operator!=(const iterator &other) const noexcept { return !(*this == other); }

    constexpr std::basic_string_view<CharType> key() const
    {
      if (kind != Kind::Object) throw std::runtime_error("Not an object iterator");
      return obj_it->first;
    }
  };

private:
  enum class Type : uint8_t { Null, Boolean, Array, Object, Integer, UInteger, Float, String };

  Type value_type{ Type::Null };

  union {
    bool boolean_value;
    basic_array_t<CharType> array_value;
    basic_object_t<CharType> object_value;
    int64_t int_value;
    uint64_t uint_value;
    double float_value;
    StringStorage<CharType> string_value;
  };

public:
  constexpr basic_json() noexcept = default;
  constexpr basic_json(std::nullptr_t) noexcept : value_type(Type::Null) {}
  constexpr basic_json(bool v) noexcept : value_type(Type::Boolean), boolean_value{ v } {}
  constexpr basic_json(basic_array_t<CharType> v) noexcept : value_type(Type::Array), array_value{ v } {}
  constexpr basic_json(basic_object_t<CharType> v) noexcept : value_type(Type::Object), object_value{ v } {}
  constexpr basic_json(int64_t v) noexcept : value_type(Type::Integer), int_value{ v } {}
  constexpr basic_json(uint64_t v) noexcept : value_type(Type::UInteger), uint_value{ v } {}
  constexpr basic_json(double v) noexcept : value_type(Type::Float), float_value{ v } {}
  constexpr basic_json(std::basic_string_view<CharType> v) noexcept : value_type(Type::String), string_value(v) {}

  constexpr bool is_object() const noexcept { return value_type == Type::Object; }
  constexpr bool is_array() const noexcept { return value_type == Type::Array; }
  constexpr bool is_string() const noexcept { return value_type == Type::String; }
  constexpr bool is_uinteger() const noexcept { return value_type == Type::UInteger; }
  constexpr bool is_integer() const noexcept { return value_type == Type::Integer; }
  constexpr bool is_float() const noexcept { return value_type == Type::Float; }
  constexpr bool is_boolean() const noexcept { return value_type == Type::Boolean; }
  constexpr bool is_null() const noexcept { return value_type == Type::Null; }
  constexpr bool is_number_integer() const noexcept { return is_integer() || is_uinteger(); }
  constexpr bool is_number_float() const noexcept { return is_float(); }
  constexpr bool is_number() const noexcept { return is_number_integer() || is_number_float(); }
  constexpr bool is_structured() const noexcept { return is_object() || is_array(); }
  constexpr bool is_primitive() const noexcept { return !is_structured(); }

  constexpr size_t size() const noexcept
  {
    switch (value_type) {
    case Type::Object:
      return object_value.size();
    case Type::Array:
      return array_value.size();
    case Type::String:
      return string_value.size();
    case Type::Null:
      return 0;
    default:
      return 1;
    }
  }

  constexpr bool empty() const noexcept { return size() == 0; }

  constexpr iterator begin() const noexcept
  {
    if (is_array()) return iterator{ array_value.begin() };
    if (is_object()) return iterator{ object_value.begin() };
    return iterator{ static_cast<const basic_json<CharType> *>(nullptr) };
  }

  constexpr iterator end() const noexcept
  {
    if (is_array()) return iterator{ array_value.end() };
    if (is_object()) return iterator{ object_value.end() };
    return iterator{ static_cast<const basic_json<CharType> *>(nullptr) };
  }

  constexpr iterator find(std::basic_string_view<CharType> key) const noexcept
  {
    if (!is_object()) return end();

    for (auto it = object_value.begin(); it != object_value.end(); ++it) {
      if (it->first == key) return iterator{ it };
    }
    return end();
  }

  constexpr const basic_json &operator[](size_t idx) const
  {
    if (!is_array()) throw std::runtime_error("Not an array");
    return array_value[idx];
  }

  constexpr const basic_json &at(std::basic_string_view<CharType> key) const
  {
    auto it = find(key);
    if (it == end()) throw std::out_of_range("Key not found");
    return *it;
  }

  constexpr const basic_json &operator[](std::basic_string_view<CharType> key) const { return at(key); }

  constexpr bool contains(std::basic_string_view<CharType> key) const noexcept
  {
    if (!is_object()) return false;
    return find(key) != end();
  }

  constexpr const basic_array_t<CharType> &array_data() const
  {
    if (!is_array()) throw std::runtime_error("Not an array");
    return array_value;
  }

  constexpr const basic_object_t<CharType> &object_data() const
  {
    if (!is_object()) throw std::runtime_error("Not an object");
    return object_value;
  }

  constexpr std::basic_string_view<CharType> getString() const
  {
    if (!is_string()) throw std::runtime_error("Not a string");
    return string_value.get();
  }

  template<typename ValueType> constexpr ValueType get() const
  {
    if constexpr (std::is_same_v<ValueType, bool>) {
      if (is_boolean()) return boolean_value;
      throw std::runtime_error("JSON value is not a boolean");
    } else if constexpr (std::is_arithmetic_v<ValueType>) {
      if (is_uinteger()) return static_cast<ValueType>(uint_value);
      if (is_integer()) return static_cast<ValueType>(int_value);
      if (is_float()) return static_cast<ValueType>(float_value);
      throw std::runtime_error("JSON value is not a number");
    } else if constexpr (std::is_same_v<ValueType, std::basic_string_view<CharType>>) {
      return getString();
    } else if constexpr (std::is_same_v<ValueType, std::nullptr_t>) {
      if (is_null()) return nullptr;
      throw std::runtime_error("JSON value is not null");
    } else if constexpr (std::is_same_v<ValueType, basic_array_t<CharType>>) {
      return array_data();
    } else if constexpr (std::is_same_v<ValueType, basic_object_t<CharType>>) {
      return object_data();
    } else {
      static_assert(std::is_same_v<ValueType, void>, "Unsupported type requested");
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
