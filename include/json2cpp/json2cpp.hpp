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
private:
  [[no_unique_address]] const T *data_{ nullptr };
  std::size_t count_{ 0 };

public:
  template<std::size_t Size>
  constexpr explicit span(const std::array<T, Size> &input) noexcept : data_(input.data()), count_(Size)
  {}
  constexpr span(const T *ptr, std::size_t count) noexcept : data_(ptr), count_(count) {}
  constexpr span() noexcept = default;
  [[nodiscard]] constexpr const T *begin() const noexcept { return data_; }
  [[nodiscard]] constexpr const T *end() const noexcept { return data_ + count_; }
  [[nodiscard]] constexpr const T &operator[](std::size_t index) const { return data_[index]; }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return count_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }
  [[nodiscard]] constexpr const T *data() const noexcept { return data_; }
};

template<typename CharType> using basic_value_pair_t = pair<std::basic_string_view<CharType>, basic_json<CharType>>;
template<typename CharType> using basic_object_t = span<basic_value_pair_t<CharType>>;
template<typename CharType> using basic_array_t = span<basic_json<CharType>>;

template<typename CharType> struct basic_json
{
private:
  enum class Type : uint8_t { Null, Boolean, Array, Object, Integer, UInteger, Float, String };
  Type value_type{ Type::Null };

  struct StringStorage
  {
    static constexpr size_t capacity = sizeof(std::basic_string_view<CharType>) / sizeof(CharType);

    union Storage {
      std::array<CharType, capacity> short_data;
      std::basic_string_view<CharType> long_data;

      constexpr Storage() noexcept : short_data{} {}
      constexpr Storage(std::basic_string_view<CharType> v) noexcept : long_data{ v } {}
    };

    size_t length = 0;
    Storage storage{};
    bool is_short = false;

    constexpr StringStorage() noexcept = default;
    constexpr StringStorage(std::basic_string_view<CharType> sv) noexcept : length{ sv.size() }
    {
      if (is_short = (length <= capacity); is_short)
        std::copy_n(sv.begin(), length, storage.short_data.begin());
      else
        storage = Storage{ sv };
    }

    [[nodiscard]] constexpr size_t size() const noexcept { return length; }
    constexpr std::basic_string_view<CharType> get() const noexcept
    {
      return is_short ? std::basic_string_view<CharType>{ storage.short_data.begin(), length } : storage.long_data;
    }
  };

  union {
    bool boolean_value;
    basic_array_t<CharType> array_value;
    basic_object_t<CharType> object_value;
    int64_t int_value;
    uint64_t uint_value;
    double float_value;
    StringStorage string_value;
  };

  struct iterator
  {
    constexpr iterator() noexcept = default;
    constexpr iterator(const void *data_ptr, size_t index = 0, Type type = Type::Null) noexcept
      : data_ptr_(data_ptr), index_(index), container_type_(type)
    {}

    [[nodiscard]] constexpr const basic_json &operator*() const
    {
      if (container_type_ == basic_json<CharType>::Type::Array) {
        return static_cast<const basic_json<CharType> *>(data_ptr_)[index_];
      } else if (container_type_ == Type::Object) {
        return static_cast<const basic_value_pair_t<CharType> *>(data_ptr_)[index_].second;
      }
      throw std::logic_error("Dereferencing invalid JSON iterator");
    }

    [[nodiscard]] constexpr const basic_json *operator->() const noexcept { return &(operator*()); }
    [[nodiscard]] constexpr ssize_t index() const noexcept { return index_; }
    constexpr iterator &operator++() noexcept
    {
      ++index_;
      return *this;
    }
    constexpr iterator &operator--() noexcept
    {
      --index_;
      return *this;
    }
    constexpr iterator operator++(int) noexcept
    {
      iterator copy = *this;
      ++(*this);
      return copy;
    }
    constexpr iterator operator--(int) noexcept
    {
      iterator copy = *this;
      --(*this);
      return copy;
    }

    [[nodiscard]] constexpr std::basic_string_view<CharType> key() const
    {
      if (container_type_ == Type::Object) {
        return static_cast<const basic_value_pair_t<CharType> *>(data_ptr_)[index_].first;
      }
      throw std::runtime_error("json iterator does not refer to an object element");
    }

    [[nodiscard]] constexpr bool operator==(const iterator &other) const
    {
      return container_type_ == other.container_type_ && data_ptr_ == other.data_ptr_ && index_ == other.index_;
    }

  private:
    size_t index_{ 0 };
    Type container_type_{ Type::Null };
    [[no_unique_address]] const void *data_ptr_{ nullptr };
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
    if (is_object()) return object_data().size();
    if (is_array()) return array_data().size();
    if (value_type == Type::String) return string_value.size();
    if (is_null()) return 0;
    return 1;
  }

  [[nodiscard]] constexpr bool empty() const noexcept { return size() == 0; }

  [[nodiscard]] constexpr iterator begin() const noexcept
  {
    if (is_array()) return iterator{ array_value.data(), 0, value_type };
    if (is_object()) return iterator{ object_value.data(), 0, value_type };
    return end();
  }

  [[nodiscard]] constexpr iterator end() const noexcept
  {
    if (is_array()) return iterator{ array_value.data(), array_value.size(), value_type };
    if (is_object()) return iterator{ object_value.data(), object_value.size(), value_type };
    return iterator{};
  }

  [[nodiscard]] constexpr iterator find(const std::basic_string_view<CharType> &key) const noexcept
  {
    if (is_object()) {
      const auto *obj_data = object_value.data();
      for (size_t i = 0; i < object_value.size(); ++i) {
        if (obj_data[i].first == key) return iterator{ obj_data, i, value_type };
      }
      return iterator{ obj_data, object_value.size(), value_type };
    }
    return iterator{};
  }

  [[nodiscard]] constexpr const iterator cbegin() const noexcept { return begin(); }
  [[nodiscard]] constexpr const iterator cend() const noexcept { return end(); }

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

  [[nodiscard]] constexpr std::basic_string_view<CharType> string_data() const
  {
    if (!is_string()) throw std::runtime_error("Not a string");
    return string_value.get();
  }

  [[nodiscard]] constexpr const basic_json &operator[](size_t idx) const
  {
    const auto &arr = array_data();
    if (idx >= arr.size()) throw std::out_of_range("Index out of bounds");
    return arr[idx];
  }

  [[nodiscard]] constexpr const basic_json &at(const std::basic_string_view<CharType> &key) const
  {
    const auto &obj = object_data();
    for (const auto &pair : obj) {
      if (pair.first == key) return pair.second;
    }
    throw std::out_of_range("Key not found");
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
    if constexpr (std::is_same_v<ValueType,
                    uint64_t> || std::is_same_v<ValueType, int64_t> || std::is_same_v<ValueType, double>) {
      if (is_uinteger()) return uint_value;
      if (is_integer()) return int_value;
      if (is_float()) return float_value;
      throw std::runtime_error("JSON value is not a number");
    } else if constexpr (std::is_same_v<ValueType, std::basic_string_view<CharType>>) {
      return string_data();
    } else if constexpr (std::is_same_v<ValueType, bool>) {
      if (is_boolean()) return boolean_value;
      throw std::runtime_error("JSON value is not a boolean");
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
