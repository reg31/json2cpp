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

// Important note: the types in this file are only intended for compile-time construction
// but consteval doesn't exist in C++17, and we're targeting C++17

#ifndef CONSTEXPR_JSON_HPP_INCLUDED
#define CONSTEXPR_JSON_HPP_INCLUDED

#include <array>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <algorithm>
#include <iterator>
#include <type_traits>

namespace json2cpp {

// simple pair to speed up compilation a bit compared to std::pair
template<typename First, typename Second> struct pair
{
  First first;
  Second second;
};

// Custom span implementation for C++17
template<typename T> struct span
{
  using iterator = const T*;

  // Constructor from std::array
  template<std::size_t Size>
  constexpr explicit span(const std::array<T, Size> &input) noexcept
    : begin_{ input.data() }, end_{ input.data() + Size }
  {}

  // Default constructor
  constexpr span() noexcept : begin_{ nullptr }, end_{ nullptr } {}

  // Constructor from a pointer and a size
  constexpr span(const T *begin, const std::size_t size) noexcept : begin_{begin}, end_{begin + size} {}

  // Begin iterator
  [[nodiscard]] constexpr iterator begin() const noexcept { return begin_; }

  // End iterator
  [[nodiscard]] constexpr iterator end() const noexcept { return end_; }

  // Size of the span
  [[nodiscard]] constexpr std::size_t size() const noexcept
  {
    return static_cast<std::size_t>(std::distance(begin_, end_));
  }

  // Access element at given index
    [[nodiscard]] constexpr const T& operator[](std::size_t index) const {
        return *(begin_ + index);
    }

    [[nodiscard]] constexpr bool empty() const noexcept { return size() == 0;}

  iterator begin_;
  iterator end_;
};

// Forward declaration of basic_json
template<typename CharType> struct basic_json;

// Type aliases
template<typename CharType> using basic_array_t = span<basic_json<CharType>>;
template<typename CharType> using basic_value_pair_t = pair<std::basic_string_view<CharType>, basic_json<CharType>>;
template<typename CharType> using basic_object_t = span<basic_value_pair_t<CharType>>;

using binary_t = span<std::uint8_t>;

template<typename CharType> struct data_variant
{
    struct monostate {};

    union value_t {
        monostate empty_;
        bool bool_;
        binary_t binary_;
        basic_array_t<CharType> array_;
        basic_object_t<CharType> object_;
        std::int64_t int64_t_;
        std::uint64_t uint64_t_;
        double double_;
        std::basic_string_view<CharType> string_view_;
        std::nullptr_t null_;

        constexpr value_t() : empty_{} {}
        constexpr value_t(monostate) : value_t() {}
        constexpr value_t(bool b) : bool_{b} {}
        constexpr value_t(binary_t b) : binary_{b} {}
        constexpr value_t(basic_array_t<CharType> a) : array_{a} {}
        constexpr value_t(basic_object_t<CharType> o) : object_{o} {}
        constexpr value_t(std::int64_t i) : int64_t_{i} {}
        constexpr value_t(std::uint64_t i) : uint64_t_{i} {}
        constexpr value_t(double d) : double_{d} {}
        constexpr value_t(std::basic_string_view<CharType> s) : string_view_{s} {}
        constexpr value_t(std::nullptr_t) : null_{} {}
    };

  enum struct selected_type {
    empty,
    boolean,
    binary,
    array,
    object,
    integer,
    uinteger,
    floating_point,
    string,
    nullish
  };

  value_t value{ monostate{} };
  selected_type selected{ selected_type::empty };

  // Implicit constructors for performance
  constexpr data_variant() = default;
  constexpr data_variant(monostate) : data_variant() {}
  constexpr data_variant(bool b) : value{ b }, selected{ selected_type::boolean } {}
  constexpr data_variant(binary_t b) : value{ b }, selected{ selected_type::binary } {}
  constexpr data_variant(basic_array_t<CharType> a) : value{ a }, selected{ selected_type::array } {}
  constexpr data_variant(basic_object_t<CharType> o) : value{ o }, selected{ selected_type::object } {}
  constexpr data_variant(std::int64_t i) : value{ i }, selected{ selected_type::integer } {}
  constexpr data_variant(std::uint64_t i) : value{ i }, selected{ selected_type::uinteger } {}
  constexpr data_variant(double d) : value{ d }, selected{ selected_type::floating_point } {}
  constexpr data_variant(std::basic_string_view<CharType> s) : value{ s }, selected{ selected_type::string } {}
  constexpr data_variant(std::nullptr_t) : value{ nullptr }, selected{ selected_type::nullish } {}

  // Helper functions to access the different data types
  [[nodiscard]] constexpr bool is_boolean() const noexcept { return selected == selected_type::boolean; }
  [[nodiscard]] constexpr const bool *get_if_boolean() const noexcept { return is_boolean() ? &value.bool_ : nullptr; }

  [[nodiscard]] constexpr bool is_array() const noexcept { return selected == selected_type::array; }
  [[nodiscard]] constexpr const basic_array_t<CharType> *get_if_array() const noexcept { return is_array() ? &value.array_ : nullptr; }

  [[nodiscard]] constexpr bool is_object() const noexcept { return selected == selected_type::object; }
  [[nodiscard]] constexpr const basic_object_t<CharType> *get_if_object() const noexcept { return is_object() ? &value.object_ : nullptr; }

  [[nodiscard]] constexpr bool is_integer() const noexcept { return selected == selected_type::integer; }
  [[nodiscard]] constexpr const std::int64_t *get_if_integer() const noexcept { return is_integer() ? &value.int64_t_ : nullptr; }

  [[nodiscard]] constexpr bool is_uinteger() const noexcept { return selected == selected_type::uinteger; }
  [[nodiscard]] constexpr const std::uint64_t *get_if_uinteger() const noexcept { return is_uinteger() ? &value.uint64_t_ : nullptr; }

  [[nodiscard]] constexpr bool is_floating_point() const noexcept { return selected == selected_type::floating_point; }
  [[nodiscard]] constexpr const double *get_if_floating_point() const noexcept { return is_floating_point() ? &value.double_ : nullptr; }

  [[nodiscard]] constexpr bool is_string() const noexcept { return selected == selected_type::string; }
  [[nodiscard]] constexpr const std::basic_string_view<CharType> *get_if_string() const noexcept { return is_string() ? &value.string_view_ : nullptr; }

  [[nodiscard]] constexpr bool is_null() const noexcept { return selected == selected_type::nullish; }
};


template<typename CharType> struct basic_json
{
    using data_t = data_variant<CharType>;
    
    struct iterator {
        using iterator_category = std::random_access_iterator_tag;
        using value_type = basic_json;
        using difference_type = std::ptrdiff_t;
        using pointer = const value_type*;
        using reference = const value_type&;
        
        constexpr iterator() noexcept = default;

        constexpr explicit iterator(const basic_json &value, std::size_t index = 0) noexcept
            : parent_value_(&value), index_{ index }
        {}

        [[nodiscard]] constexpr reference operator*() const
        {
          if (parent_value_->is_array()) {
            return (*parent_value_)[index_];
          } else if (parent_value_->is_object()) {
            return std::next(parent_value_->object_data().begin(), static_cast<std::ptrdiff_t>(index_))->second;
          } else {
            return *parent_value_;
          }
        }

        [[nodiscard]] constexpr pointer operator->() const noexcept { return &(*(*this)); }

        [[nodiscard]] constexpr std::size_t index() const noexcept { return index_; }

        [[nodiscard]] constexpr reference value() const noexcept { return *(*this); }

        [[nodiscard]] constexpr std::basic_string_view<CharType> key() const
        {
            if (parent_value_->is_object()) {
                return std::next(parent_value_->object_data().begin(), static_cast<std::ptrdiff_t>(index_))->first;
            } else {
                throw std::runtime_error("json value is not an object, it has no key");
            }
        }

        constexpr bool operator==(const iterator &other) const noexcept
        {
            return other.parent_value_ == parent_value_ && other.index_ == index_;
        }
        constexpr bool operator!=(const iterator &other) const noexcept { return !(*this == other); }

        constexpr bool operator<(const iterator &other) const noexcept
        {
            return other.parent_value_ == parent_value_ && index_ < other.index_;
        }


        constexpr iterator &operator--() noexcept
        {
            --index_;
            return *this;
        }

        [[nodiscard]] constexpr iterator operator--(int) noexcept
        {
            iterator result{ *this };
            index_--;
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
            index_++;
            return result;
        }

        constexpr iterator &operator+=(const std::ptrdiff_t value) noexcept
        {
            index_ = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(index_) + value);
            return *this;
        }


        constexpr iterator &operator+=(const std::size_t value) noexcept
        {
            index_ += value;
            return *this;
        }

        const basic_json *parent_value_{ nullptr };
        std::size_t index_{ 0 };
    };

    using const_iterator = iterator;
    
    [[nodiscard]] constexpr iterator begin() const noexcept { return iterator{ *this }; }

    [[nodiscard]] constexpr iterator end() const noexcept { return iterator{ *this, size() }; }

    [[nodiscard]] constexpr iterator cbegin() const noexcept { return begin(); }

    [[nodiscard]] constexpr iterator cend() const noexcept { return end(); }

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        if (is_null()) return 0;
        if (is_object()) return object_data().size();
        if (is_array()) return array_data().size();
        return 1;
    }

    [[nodiscard]] constexpr bool empty() const noexcept { return size() == 0; }
    
    [[nodiscard]] constexpr const basic_json &operator[](const std::size_t idx) const
    {
        if (is_array()) {
            try{
                return array_data()[idx];
            } catch (const std::out_of_range&) {
              throw std::runtime_error("index out of range");
            }
        }
        throw std::runtime_error("value is not an array type");
    }

  [[nodiscard]] constexpr const basic_json &at(const std::basic_string_view<CharType> key) const
    {
        if (is_object()) {
            const auto &children = object_data();

            // find_if is not constexpr in C++17, so we rolled our own,
            // and this helps us work around bugs in older versions of GCC
            // and constexpr
            const auto finder = [&]() {
                auto itr = children.begin();

                for (; itr != children.end(); ++itr) {
                    if (itr->first == key) { return itr; }
                }

                return itr;
            };

            const auto obj = finder();

            if (obj != children.end()) {
                return obj->second;
            } else {
                throw std::runtime_error("Key not found");
            }
        } else {
            throw std::runtime_error("value is not an object type");
        }
  }

    template<typename Key> [[nodiscard]] constexpr std::size_t count(const Key &key) const
    {
        if (is_object()) {
            const auto found = find(key);
            if (found == end()) {
                return 0;
            } else {
                return 1;
            }
        }
        return 0;
    }

    [[nodiscard]] constexpr iterator find(const std::basic_string_view<CharType> key) const
    {
        for (auto itr = begin(); itr != end(); ++itr) {
            if (itr.key() == key) { return itr; }
        }
        return end();
    }

  [[nodiscard]] constexpr const basic_json &operator[](const std::basic_string_view<CharType> key) const
  {
    return at(key);
  }

    [[nodiscard]] constexpr const basic_array_t<CharType>& array_data() const
    {
        if (data.is_array()) {
            return *data.get_if_array();
        } else {
            throw std::runtime_error("value is not an array type");
        }
    }

    [[nodiscard]] constexpr const basic_object_t<CharType>& object_data() const
    {
        if (data.is_object()) {
            return *data.get_if_object();
        } else {
            throw std::runtime_error("value is not an object type");
        }
    }

    constexpr static basic_json object() { return basic_json{ data_t{ basic_object_t<CharType>{} } }; }
    constexpr static basic_json array() { return basic_json{ data_t{ basic_array_t<CharType>{} } }; }


  template<typename Type> [[nodiscard]] constexpr auto get() const
  {
      if constexpr (std::is_same_v<Type, std::uint64_t> || std::is_same_v<Type, std::int64_t> || std::is_same_v<Type, double>) {
          if (data.is_uinteger()) {
              return Type(*data.get_if_uinteger());
          } else if (data.is_integer()) {
              return Type(*data.get_if_integer());
          } else if (data.is_floating_point()) {
              return Type(*data.get_if_floating_point());
          } else {
              throw std::runtime_error("Unexpected type: number requested");
          }
      } else if constexpr (std::is_same_v<Type, std::basic_string_view<CharType>> || std::is_same_v<Type, std::basic_string<CharType>>) {
          if (data.is_string()) {
              return *data.get_if_string();
          } else {
              throw std::runtime_error("Unexpected type: string-like requested");
          }
      } else if constexpr (std::is_same_v<Type, bool>) {
          if (data.is_boolean()) {
              return *data.get_if_boolean();
          } else {
              throw std::runtime_error("Unexpected type: bool requested");
          }
      } else if constexpr (std::is_same_v<Type, std::nullptr_t>) {
          if (data.is_null()) {
              return nullptr;
          } else {
              throw std::runtime_error("Unexpected type: null requested");
          }
      } else {
          throw std::runtime_error("Unexpected type for get()");
      }
  }

  [[nodiscard]] constexpr bool is_object() const noexcept { return data.selected == data_t::selected_type::object; }
  [[nodiscard]] constexpr bool is_array() const noexcept { return data.selected == data_t::selected_type::array; }
  [[nodiscard]] constexpr bool is_string() const noexcept { return data.selected == data_t::selected_type::string; }
  [[nodiscard]] constexpr bool is_boolean() const noexcept { return data.selected == data_t::selected_type::boolean; }
  [[nodiscard]] constexpr bool is_structured() const noexcept { return is_object() || is_array(); }
  [[nodiscard]] constexpr bool is_number() const noexcept { return is_number_integer() || is_number_float(); }
  [[nodiscard]] constexpr bool is_number_integer() const noexcept { return is_number_signed() || is_number_unsigned(); }
  [[nodiscard]] constexpr bool is_null() const noexcept { return data.selected == data_t::selected_type::nullish; }
  [[nodiscard]] constexpr bool is_binary() const noexcept { return data.selected == data_t::selected_type::binary; }

  [[nodiscard]] constexpr bool is_number_signed() const noexcept { return data.selected == data_t::selected_type::integer; }
  [[nodiscard]] constexpr bool is_number_unsigned() const noexcept { return data.selected == data_t::selected_type::uinteger; }
  [[nodiscard]] constexpr bool is_number_float() const noexcept { return data.selected == data_t::selected_type::floating_point; }
  [[nodiscard]] constexpr bool is_primitive() const noexcept
  {
      return is_null() || is_string() || is_boolean() || is_number() || is_binary();
  }

  constexpr basic_json() = default;
  constexpr basic_json(const data_t& d) : data{d} {}

  data_t data;
};

using json = basic_json<char16_t>;
using object_t = basic_object_t<char16_t>;
using value_pair_t = basic_value_pair_t<char16_t>;
using array_t = basic_array_t<char16_t>;
} // namespace json2cpp

#endif
