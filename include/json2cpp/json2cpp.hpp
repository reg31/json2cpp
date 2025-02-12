/*
MIT License

Copyright (c) 2022 Jason Turner

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
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <variant>

namespace json2cpp {

template<typename First, typename Second> struct pair
{
  First first;
  Second second;
};

template<typename T> struct span
{
  using iterator = const T *;
  template<std::size_t Size>
  consteval explicit span(const std::array<T, Size> &input) noexcept
    : begin_{ input.data() }, end_{ input.data() + Size }
  {}
  consteval span() noexcept : begin_{ nullptr }, end_{ nullptr } {}
  consteval span(const T *begin, const std::size_t size) noexcept : begin_{ begin }, end_{ begin + size } {}
  [[nodiscard]] constexpr iterator begin() const noexcept { return begin_; }
  [[nodiscard]] constexpr iterator end() const noexcept { return end_; }
  [[nodiscard]] constexpr std::size_t size() const noexcept
  {
    return static_cast<std::size_t>(std::distance(begin_, end_));
  }
  [[nodiscard]] constexpr const T &operator[](std::size_t index) const { return *(begin_ + index); }
  [[nodiscard]] constexpr bool empty() const noexcept { return size() == 0; }
  iterator begin_;
  iterator end_;
};

template<typename CharType> struct basic_json;
template<typename CharType> using basic_array_t = span<basic_json<CharType>>;
template<typename CharType> using basic_value_pair_t = pair<std::basic_string_view<CharType>, basic_json<CharType>>;
template<typename CharType> using basic_object_t = span<basic_value_pair_t<CharType>>;

using binary_t = span<std::uint8_t>;

template<typename CharType> struct data_variant
{
  using variant_t = std::variant<
    std::monostate,
    bool,
    binary_t,
    basic_array_t<CharType>,
    basic_object_t<CharType>,
    std::int64_t,
    std::uint64_t,
    double,
    std::basic_string_view<CharType>,
    std::nullptr_t
  >;

  variant_t value;

  consteval data_variant() : value{ std::monostate{} } {}
  consteval data_variant(std::monostate) : value{ std::monostate{} } {}
  consteval data_variant(bool b) : value{ b } {}
  consteval data_variant(binary_t b) : value{ b } {}
  consteval data_variant(basic_array_t<CharType> a) : value{ a } {}
  consteval data_variant(basic_object_t<CharType> o) : value{ o } {}
  consteval data_variant(std::int64_t i) : value{ i } {}
  consteval data_variant(std::uint64_t i) : value{ i } {}
  consteval data_variant(double d) : value{ d } {}
  consteval data_variant(std::basic_string_view<CharType> s) : value{ s } {}
  consteval data_variant(std::nullptr_t) : value{ nullptr } {}

  [[nodiscard]] constexpr bool is_boolean() const noexcept { return std::holds_alternative<bool>(value); }
  [[nodiscard]] constexpr const bool *get_if_boolean() const noexcept { if (is_boolean()) return &std::get<bool>(value); else return nullptr; }
  [[nodiscard]] constexpr bool is_array() const noexcept { return std::holds_alternative<basic_array_t<CharType>>(value); }
  [[nodiscard]] constexpr const basic_array_t<CharType> *get_if_array() const noexcept
  {
    if (is_array()) return &std::get<basic_array_t<CharType>>(value); else return nullptr;
  }
  [[nodiscard]] constexpr bool is_object() const noexcept { return std::holds_alternative<basic_object_t<CharType>>(value); }
  [[nodiscard]] constexpr const basic_object_t<CharType> *get_if_object() const noexcept
  {
    if (is_object()) return &std::get<basic_object_t<CharType>>(value); else return nullptr;
  }
  [[nodiscard]] constexpr bool is_integer() const noexcept { return std::holds_alternative<std::int64_t>(value); }
  [[nodiscard]] constexpr const std::int64_t *get_if_integer() const noexcept
  {
    if (is_integer()) return &std::get<std::int64_t>(value); else return nullptr;
  }
  [[nodiscard]] constexpr bool is_uinteger() const noexcept { return std::holds_alternative<std::uint64_t>(value); }
  [[nodiscard]] constexpr const std::uint64_t *get_if_uinteger() const noexcept
  {
    if (is_uinteger()) return &std::get<std::uint64_t>(value); else return nullptr;
  }
  [[nodiscard]] constexpr bool is_floating_point() const noexcept { return std::holds_alternative<double>(value); }
  [[nodiscard]] constexpr const double *get_if_floating_point() const noexcept
  {
    if (is_floating_point()) return &std::get<double>(value); else return nullptr;
  }
  [[nodiscard]] constexpr bool is_string() const noexcept { return std::holds_alternative<std::basic_string_view<CharType>>(value); }
  [[nodiscard]] constexpr const std::basic_string_view<CharType> *get_if_string() const noexcept
  {
    if (is_string()) return &std::get<std::basic_string_view<CharType>>(value); else return nullptr;
  }
  [[nodiscard]] constexpr bool is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(value); }
  [[nodiscard]] constexpr bool is_binary() const noexcept { return std::holds_alternative<binary_t>(value); }
  [[nodiscard]] constexpr const binary_t *get_if_binary() const noexcept { if (is_binary()) return &std::get<binary_t>(value); else return nullptr; }
};

template<typename CharType> struct basic_json
{
  using data_t = data_variant<CharType>;
  using value_type = basic_json;
  using const_reference = const basic_json&;
  using pointer = const basic_json*;

  struct iterator
  {
    using iterator_category = std::random_access_iterator_tag;
    using value_type = basic_json;
    using difference_type = std::ptrdiff_t;
    using pointer = const value_type *;
    using reference = const value_type &;
    enum class container_type { single, array, object };
    constexpr iterator() noexcept = default;
    constexpr iterator(const basic_json &value, std::size_t index = 0) noexcept : index_{ index }
    {
      if (value.is_array()) {
        container_ = container_type::array;
        array_ptr_ = std::get<basic_array_t<CharType>>(value.data.value).begin();
      } else if (value.is_object()) {
        container_ = container_type::object;
        object_ptr_ = std::get<basic_object_t<CharType>>(value.data.value).begin();
      } else {
        container_ = container_type::single;
        parent_value_ = &value;
      }
    }
    [[nodiscard]] constexpr reference operator*() const noexcept
    {
      if (container_ == container_type::array) return array_ptr_[index_];
      if (container_ == container_type::object) return object_ptr_[index_].second;
      return *parent_value_;
    }
    [[nodiscard]] constexpr pointer operator->() const noexcept { return &**this; }
    [[nodiscard]] constexpr std::size_t index() const noexcept { return index_; }
    [[nodiscard]] constexpr std::basic_string_view<CharType> key() const
    {
      if (container_ == container_type::object) return object_ptr_[index_].first;
      throw std::runtime_error("json value is not an object, it has no key");
    }
    constexpr bool operator==(const iterator &other) const noexcept
    {
      return container_ == other.container_
             && (container_ == container_type::single ? other.parent_value_ == parent_value_ : index_ == other.index_);
    }
    constexpr bool operator!=(const iterator &other) const noexcept { return !(*this == other); }
    constexpr bool operator<(const iterator &other) const noexcept
    {
      return container_ == other.container_ && container_ != container_type::single && index_ < other.index_;
    }
    constexpr bool operator>(const iterator &other) const noexcept { return other < *this; }
    constexpr bool operator<=(const iterator &other) const noexcept { return !(other < *this); }
    constexpr bool operator>=(const iterator &other) const noexcept { return !(*this < other); }

    constexpr iterator &operator--() noexcept
    {
      --index_;
      return *this;
    }
    [[nodiscard]] constexpr iterator operator--(int) noexcept
    {
      iterator result{ *this };
      --index_;
      return result;
    }
    constexpr iterator &operator++() noexcept
    {
      ++index_;
      return *this;
    }
    [[nodiscard]] constexpr iterator operator++(int) noexcept
    {
      iterator result{ *this };
      ++index_;
      return result;
    }
    constexpr iterator operator+(std::ptrdiff_t value) const noexcept { iterator temp = *this; temp += value; return temp; }
    constexpr iterator operator-(std::ptrdiff_t value) const noexcept { iterator temp = *this; temp -= value; return temp; }
    constexpr iterator &operator+=(std::ptrdiff_t value) noexcept
    {
      index_ = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(index_) + value);
      return *this;
    }
    constexpr iterator &operator-=(std::ptrdiff_t value) noexcept
    {
      index_ = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(index_) - value);
      return *this;
    }
    constexpr std::ptrdiff_t operator-(const iterator& other) const noexcept
    {
        if (container_ != other.container_ || container_ == container_type::single) {
            throw std::runtime_error("Iterators incompatible for subtraction.");
        }
        return static_cast<std::ptrdiff_t>(index_) - static_cast<std::ptrdiff_t>(other.index_);
    }


  private:
    container_type container_;
    union {
      const basic_json *parent_value_;
      span<basic_json<CharType>>::iterator array_ptr_;
      span<basic_value_pair_t<CharType>>::iterator object_ptr_;
    };
    std::size_t index_{ 0 };
  };

  using const_iterator = iterator;
  [[nodiscard]] constexpr iterator begin() const noexcept
  {
    if (is_array()) return iterator{ *this, 0 };
    if (is_object()) return iterator{ *this, 0 };
    return iterator{ *this };
  }
  [[nodiscard]] constexpr iterator end() const noexcept
  {
    if (is_array()) return iterator{ *this, array_data().size() };
    if (is_object()) return iterator{ *this, object_data().size() };
    return iterator{ *this, 1 };
  }
  [[nodiscard]] constexpr iterator cbegin() const noexcept { return begin(); }
  [[nodiscard]] constexpr iterator cend() const noexcept { return end(); }
  [[nodiscard]] constexpr std::size_t size() const noexcept
  {
    if (is_null()) return 0;
    if (is_object()) return object_data().size();
    if (is_array()) return array_data().size();
    return 1;
  }
  [[nodiscard]] constexpr bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] constexpr const_reference operator[](std::size_t idx) const
  {
    if (is_array()) {
      if (idx >= array_data().size()) throw std::out_of_range("index out of range");
      return array_data()[idx];
    }
    throw std::runtime_error("value is not an array type");
  }
  [[nodiscard]] constexpr const_reference at(const std::basic_string_view<CharType> key) const
  {
    if (is_object()) {
      auto it = std::ranges::find_if(object_data(), [&](const auto &p) { return p.first == key; });
      if (it != object_data().end()) return it->second;
      throw std::out_of_range("Key not found");
    }
    throw std::runtime_error("value is not an object type");
  }
  template<typename Key> [[nodiscard]] constexpr std::size_t count(const Key &key) const
  {
    return (is_object() && find(key) != end()) ? 1 : 0;
  }
  [[nodiscard]] constexpr iterator find(const std::basic_string_view<CharType> key) const
  {
    if (is_object()) {
      auto it = std::ranges::find_if(object_data(), [&](const auto &p) { return p.first == key; });
      if (it != object_data().end())
        return iterator{ *this, static_cast<std::size_t>(std::distance(object_data().begin(), it)) };
    } else if (is_array()) {
      auto it = std::ranges::find_if(
        array_data(), [&](const auto &element) { return element.is_string() && *element.data.get_if_string() == key; });
      if (it != array_data().end())
        return iterator{ *this, static_cast<std::size_t>(std::distance(array_data().begin(), it)) };
    }
    return end();
  }
  template<typename Key> [[nodiscard]] constexpr bool contains(const Key &key) const { return find(key) != end(); }
  [[nodiscard]] constexpr const_reference operator[](const std::basic_string_view<CharType> key) const
  {
    return at(key);
  }
  [[nodiscard]] constexpr const basic_array_t<CharType> &array_data() const
  {
    if (data.is_array()) return *data.get_if_array();
    throw std::runtime_error("value is not an array type");
  }
  [[nodiscard]] constexpr const basic_object_t<CharType> &object_data() const
  {
    if (data.is_object()) return *data.get_if_object();
    throw std::runtime_error("value is not an object type");
  }
  [[nodiscard]] static consteval basic_json object() noexcept { return { data_t{ basic_object_t<CharType>{} } }; }
  [[nodiscard]] static consteval basic_json array() noexcept { return { data_t{ basic_array_t<CharType>{} } }; }
  template<typename Type> [[nodiscard]] constexpr Type get() const
  {
    if constexpr (std::is_same_v<Type,
                    std::uint64_t> || std::is_same_v<Type, std::int64_t> || std::is_same_v<Type, double>) {
      if (data.is_uinteger()) return static_cast<Type>(*data.get_if_uinteger());
      if (data.is_integer()) return static_cast<Type>(*data.get_if_integer());
      if (data.is_floating_point()) return static_cast<Type>(*data.get_if_floating_point());
      throw std::runtime_error("Unexpected type: number requested");
    } else if constexpr (std::is_same_v<Type,
                           std::basic_string_view<CharType>> || std::is_same_v<Type, std::basic_string<CharType>>) {
      if (data.is_string()) return static_cast<Type>(*data.get_if_string());
      throw std::runtime_error("Unexpected type: string-like requested");
    } else if constexpr (std::is_same_v<Type, bool>) {
      if (data.is_boolean()) return *data.get_if_boolean();
      throw std::runtime_error("Unexpected type: bool requested");
    } else if constexpr (std::is_same_v<Type, std::nullptr_t>) {
      if (data.is_null()) return nullptr;
      throw std::runtime_error("Unexpected type: null requested");
    } else {
      throw std::runtime_error("Unexpected type for get()");
    }
  }
  [[nodiscard]] constexpr bool is_object() const noexcept { return data.is_object(); }
  [[nodiscard]] constexpr bool is_array() const noexcept { return data.is_array(); }
  [[nodiscard]] constexpr bool is_string() const noexcept { return data.is_string(); }
  [[nodiscard]] constexpr bool is_boolean() const noexcept { return data.is_boolean(); }
  [[nodiscard]] constexpr bool is_structured() const noexcept { return is_object() || is_array(); }
  [[nodiscard]] constexpr bool is_number() const noexcept { return is_number_integer() || is_number_float(); }
  [[nodiscard]] constexpr bool is_number_integer() const noexcept
  {
    return data.is_integer() || data.is_uinteger();
  }
  [[nodiscard]] constexpr bool is_null() const noexcept { return data.is_null(); }
  [[nodiscard]] constexpr bool is_binary() const noexcept { return data.is_binary(); }
  [[nodiscard]] constexpr bool is_number_signed() const noexcept
  {
    return data.is_integer();
  }
  [[nodiscard]] constexpr bool is_number_unsigned() const noexcept
  {
    return data.is_uinteger();
  }
  [[nodiscard]] constexpr bool is_number_float() const noexcept
  {
    return data.is_floating_point();
  }
  [[nodiscard]] constexpr bool is_primitive() const noexcept
  {
    return is_null() || is_string() || is_boolean() || is_number() || is_binary();
  }
  consteval basic_json() = default;
  consteval basic_json(const data_t &d) noexcept : data{ d } {}
  data_t data;
};

#ifdef JSON2CPP_USE_UTF16
typedef char16_t basicType;
#else
typedef char basicType;
#endif

using json = basic_json<basicType>;
using object_t = basic_object_t<basicType>;
using value_pair_t = basic_value_pair_t<basicType>;
using array_t = basic_array_t<basicType>;

}// namespace json2cpp

#endif
