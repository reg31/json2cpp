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

template<typename T> struct span
{
private:
  const T *data_{ nullptr };
  size_t count_{ 0 };

public:
  template<std::size_t Size>
  constexpr explicit span(const std::array<T, Size> &input) noexcept : data_{ input.data() }, count_{ Size }
  {}

  constexpr span() noexcept = default;
  [[nodiscard]] constexpr const T *begin() const noexcept { return data_; }
  [[nodiscard]] constexpr const T *end() const noexcept { return data_ + count_; }
  [[nodiscard]] constexpr const T &operator[](std::size_t index) const { return data_[index]; }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return count_; }
  [[nodiscard]] constexpr const T *data() const noexcept { return data_; }
};

template<typename CharType> using basic_value_pair_t = pair<std::basic_string_view<CharType>, basic_json<CharType>>;
template<typename CharType> using basic_object_t = span<basic_value_pair_t<CharType>>;
template<typename CharType> using basic_array_t = span<basic_json<CharType>>;

template<typename CharType> struct basic_json
{
private:
  enum class Type : uint8_t { Null, Boolean, Array, Object, Integer, UInteger, Float, String };

  struct StringStorage
  {
    static constexpr uint8_t capacity = sizeof(std::basic_string_view<CharType>) / sizeof(CharType);

    union Storage {
      std::array<CharType, capacity> short_data;
      std::basic_string_view<CharType> long_data;

      constexpr Storage() noexcept : short_data{} {}
      constexpr Storage(std::basic_string_view<CharType> v) noexcept : long_data{ v } {}
    } storage;

    size_t length{ 0 };

    constexpr StringStorage() noexcept = default;
    constexpr StringStorage(std::basic_string_view<CharType> sv) noexcept : length{ sv.size() }
    {
      if (length <= capacity)
        std::copy_n(sv.begin(), length, storage.short_data.begin());
      else
        storage = Storage{ sv };
    }

    [[nodiscard]] inline constexpr size_t size() const noexcept { return length; }
    [[nodiscard]] inline constexpr std::basic_string_view<CharType> get() const noexcept
    {
      return length <= capacity ? std::basic_string_view<CharType>{ storage.short_data.begin(), length }
                                : storage.long_data;
    }
  };

  struct iterator
  {
  private:
    enum class Kind : uint8_t { None, Array, Object };

    Kind kind{ Kind::None };
    union Storage {
      constexpr Storage() noexcept : default_ptr{} {}
      constexpr Storage(const basic_json<CharType> *it) noexcept : arr_it{ it } {}
      constexpr Storage(const basic_value_pair_t<CharType> *it) noexcept : obj_it{ it } {}

      const basic_json<CharType> *arr_it;
      const basic_value_pair_t<CharType> *obj_it;
      std::nullptr_t default_ptr;
    } storage;

  public:
    using value_type = basic_json<CharType>;
    using pointer = const basic_json<CharType> *;
    using reference = const basic_json<CharType> &;

    constexpr iterator() noexcept = default;
    constexpr explicit iterator(const basic_json<CharType> *it) noexcept : kind(Kind::Array), storage(it) {}
    constexpr explicit iterator(const basic_value_pair_t<CharType> *it) noexcept : kind(Kind::Object), storage(it) {}

    constexpr reference operator*() const
    {
      if (kind == Kind::Array) return *storage.arr_it;
      if (kind == Kind::Object) return storage.obj_it->second;
      throw std::runtime_error("Invalid iterator");
    }

    constexpr pointer operator->() const { return &(operator*()); }

    constexpr iterator &operator++() noexcept
    {
      if (kind == Kind::Array) {
        ++storage.arr_it;
      } else if (kind == Kind::Object) {
        ++storage.obj_it;
      }
      return *this;
    }

    constexpr iterator operator++(int) noexcept
    {
      iterator tmp = *this;
      ++(*this);
      return tmp;
    }

    constexpr bool operator==(const iterator &other) const noexcept
    {
      if (kind != other.kind) return false;
      if (kind == Kind::Array) return storage.arr_it == other.storage.arr_it;
      if (kind == Kind::Object) return storage.obj_it == other.storage.obj_it;
      return true;
    }

    constexpr bool operator!=(const iterator &other) const noexcept { return !(*this == other); }

    constexpr std::basic_string_view<CharType> key() const
    {
      if (kind != Kind::Object) throw std::runtime_error("Iterator does not refer to an object element");
      return storage.obj_it->first;
    }
  };

  Type value_type{ Type::Null };

  union {
    bool boolean_value;
    basic_array_t<CharType> array_value;
    basic_object_t<CharType> object_value;
    int64_t int_value;
    uint64_t uint_value;
    double float_value;
    StringStorage string_value;
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

  [[nodiscard]] constexpr bool is_object() const noexcept { return value_type == Type::Object; }
  [[nodiscard]] constexpr bool is_array() const noexcept { return value_type == Type::Array; }
  [[nodiscard]] constexpr bool is_string() const noexcept { return value_type == Type::String; }
  [[nodiscard]] constexpr bool is_uinteger() const noexcept { return value_type == Type::UInteger; }
  [[nodiscard]] constexpr bool is_integer() const noexcept { return value_type == Type::Integer; }
  [[nodiscard]] constexpr bool is_float() const noexcept { return value_type == Type::Float; }
  [[nodiscard]] constexpr bool is_boolean() const noexcept { return value_type == Type::Boolean; }
  [[nodiscard]] constexpr bool is_null() const noexcept { return value_type == Type::Null; }
  [[nodiscard]] constexpr bool is_number_integer() const noexcept { return is_integer() || is_uinteger(); }
  [[nodiscard]] constexpr bool is_number_float() const noexcept { return is_float(); }
  [[nodiscard]] constexpr bool is_number() const noexcept { return is_number_integer() || is_number_float(); }
  [[nodiscard]] constexpr bool is_structured() const noexcept { return is_object() || is_array(); }
  [[nodiscard]] constexpr bool is_primitive() const noexcept { return !is_structured(); }

  [[nodiscard]] constexpr size_t size() const noexcept
  {
    if (is_object()) return object_value.size();
    if (is_string()) return string_value.size();
    if (is_array()) return array_value.size();
    if (is_null()) return 0;
    return 1;
  }

  [[nodiscard]] constexpr bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] constexpr iterator begin() const noexcept
  {
    if (is_array()) return iterator{ array_value.begin() };
    if (is_object()) return iterator{ object_value.begin() };
    return iterator{};
  }

  [[nodiscard]] constexpr iterator end() const noexcept
  {
    if (is_array()) return iterator{ array_value.end() };
    if (is_object()) return iterator{ object_value.end() };
    return iterator{};
  }

  [[nodiscard]] constexpr iterator find(const std::basic_string_view<CharType> &key) const noexcept
  {
    if (is_object()) {
      const auto &obj_data = object_value;
      auto it = std::find_if(obj_data.begin(), obj_data.end(), [&key](const auto &pair) { return pair.first == key; });
      if (it != obj_data.end()) return iterator{ it };
    }
    return end();
  }

  [[nodiscard]] constexpr const basic_array_t<CharType> &array_data() const
  {
    if (!is_array()) throw std::runtime_error("Not an array");
    return array_value;
  }

  [[nodiscard]] constexpr const basic_object_t<CharType> &object_data() const
  {
    if (!is_object()) throw std::runtime_error("Not an object");
    return object_value;
  }

  [[nodiscard]] constexpr std::basic_string_view<CharType> getString() const
  {
    if (!is_string()) throw std::runtime_error("Not a string");
    return string_value.get();
  }

  [[nodiscard]] constexpr const basic_json &operator[](size_t idx) const {
	if (!is_array()) throw std::runtime_error("Not an array");
    return array_data()[idx];
  }
  
  [[nodiscard]] constexpr const basic_json &at(const std::basic_string_view<CharType> &key) const
  {
    auto it = find(key);
    if (it == end()) { throw std::out_of_range("Key not found"); }
    return *it;
  }

  [[nodiscard]] constexpr const basic_json &operator[](const std::basic_string_view<CharType> &key) const
  {
    return at(key);
  }

  template<typename Key> [[nodiscard]] constexpr bool contains(const Key &key) const noexcept
  {
    if (!is_object()) return false;
    return find(key) != end();
  }

  template<typename ValueType> [[nodiscard]] constexpr ValueType get() const
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
      throw std::runtime_error("Unsupported type requested");
    }
  }

  friend struct iterator;
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
