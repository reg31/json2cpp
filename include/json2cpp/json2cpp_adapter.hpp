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

#ifndef JSON2CPP_VALIJSON_ADAPTER_HPP_INCLUDED
#define JSON2CPP_VALIJSON_ADAPTER_HPP_INCLUDED

#include "json2cpp.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <valijson/exceptions.hpp>
#include <valijson/internal/adapter.hpp>
#include <valijson/internal/basic_adapter.hpp>
#include <valijson/internal/frozen_value.hpp>

namespace valijson::adapters {

class json2cppJsonAdapter;
class json2cppJsonArrayValueIterator;
class json2cppJsonObjectMemberIterator;

using json2cppJsonObjectMember = std::pair<std::string, json2cppJsonAdapter>;

namespace json2cpp_adapter_detail {
  inline constexpr json2cpp::json empty_array{ json2cpp::array_t{} };
  inline constexpr json2cpp::json empty_object{ json2cpp::object_t{} };
}// namespace json2cpp_adapter_detail

class json2cppJsonArrayValueIterator
{
public:
  using iterator_category = std::bidirectional_iterator_tag;
  using value_type = json2cppJsonAdapter;
  using difference_type = std::ptrdiff_t;
  using pointer = void;
  using reference = value_type;

  constexpr json2cppJsonArrayValueIterator(const json2cpp::json *owner, std::size_t index) noexcept
    : m_owner(owner), m_index(index)
  {}

  value_type operator*() const;
  DerefProxy<value_type> operator->() const;

  constexpr bool operator==(const json2cppJsonArrayValueIterator &) const noexcept = default;

  constexpr json2cppJsonArrayValueIterator &operator++() noexcept
  {
    ++m_index;
    return *this;
  }

  constexpr json2cppJsonArrayValueIterator operator++(int) noexcept
  {
    auto previous = *this;
    ++(*this);
    return previous;
  }

  constexpr json2cppJsonArrayValueIterator &operator--() noexcept
  {
    --m_index;
    return *this;
  }

  constexpr void advance(difference_type offset) noexcept
  {
    m_index = static_cast<std::size_t>(static_cast<difference_type>(m_index) + offset);
  }

private:
  const json2cpp::json *m_owner = nullptr;
  std::size_t m_index = 0;
};

class json2cppJsonObjectMemberIterator
{
public:
  using iterator_category = std::bidirectional_iterator_tag;
  using value_type = json2cppJsonObjectMember;
  using difference_type = std::ptrdiff_t;
  using pointer = void;
  using reference = value_type;

  constexpr json2cppJsonObjectMemberIterator(const json2cpp::json *owner, std::size_t index) noexcept
    : m_owner(owner), m_index(index)
  {}

  value_type operator*() const;
  DerefProxy<value_type> operator->() const;

  constexpr bool operator==(const json2cppJsonObjectMemberIterator &) const noexcept = default;

  constexpr json2cppJsonObjectMemberIterator &operator++() noexcept
  {
    ++m_index;
    return *this;
  }

  constexpr json2cppJsonObjectMemberIterator operator++(int) noexcept
  {
    auto previous = *this;
    ++(*this);
    return previous;
  }

  constexpr json2cppJsonObjectMemberIterator &operator--() noexcept
  {
    --m_index;
    return *this;
  }

private:
  const json2cpp::json *m_owner = nullptr;
  std::size_t m_index = 0;
};

class json2cppJsonArray
{
public:
  using const_iterator = json2cppJsonArrayValueIterator;
  using iterator = const_iterator;

  json2cppJsonArray() noexcept : m_value(json2cpp_adapter_detail::empty_array) {}

  explicit json2cppJsonArray(const json2cpp::json &value) : m_value(value)
  {
    if (!value.is_array()) throwRuntimeError("Value is not an array.");
  }

  [[nodiscard]] const_iterator begin() const noexcept { return { &m_value, 0 }; }
  [[nodiscard]] const_iterator end() const noexcept { return { &m_value, m_value.size() }; }
  [[nodiscard]] std::size_t size() const noexcept { return m_value.size(); }

private:
  const json2cpp::json &m_value;
};

class json2cppJsonObject
{
public:
  using const_iterator = json2cppJsonObjectMemberIterator;
  using iterator = const_iterator;

  json2cppJsonObject() noexcept : m_value(json2cpp_adapter_detail::empty_object) {}

  explicit json2cppJsonObject(const json2cpp::json &value) : m_value(value)
  {
    if (!value.is_object()) throwRuntimeError("Value is not an object.");
  }

  [[nodiscard]] const_iterator begin() const noexcept { return { &m_value, 0 }; }
  [[nodiscard]] const_iterator end() const noexcept { return { &m_value, m_value.size() }; }

  [[nodiscard]] const_iterator find(std::string_view property_name) const noexcept
  {
    const auto entry = m_value.find_entry(property_name);
    return { &m_value, entry ? m_value.index(*entry->second) : m_value.size() };
  }

  [[nodiscard]] std::size_t size() const noexcept { return m_value.size(); }

private:
  const json2cpp::json &m_value;
};

class json2cppJsonFrozenValue final : public FrozenValue
{
public:
  explicit json2cppJsonFrozenValue(json2cpp::json source) noexcept : m_value(source) {}

  FrozenValue *clone() const override { return new json2cppJsonFrozenValue(m_value); }
  bool equalTo(const Adapter &other, bool strict) const override;

private:
  json2cpp::json m_value;
};

class json2cppJsonValue
{
public:
  json2cppJsonValue() noexcept : m_value(json2cpp_adapter_detail::empty_object) {}
  explicit json2cppJsonValue(const json2cpp::json &value) noexcept : m_value(value) {}

  FrozenValue *freeze() const { return new json2cppJsonFrozenValue(m_value); }

  [[nodiscard]] std::optional<json2cppJsonArray> getArrayOptional() const
  {
    return m_value.is_array() ? std::optional{ json2cppJsonArray{ m_value } } : std::nullopt;
  }

  bool getArraySize(std::size_t &result) const noexcept
  {
    if (!m_value.is_array()) return false;
    result = m_value.size();
    return true;
  }

  bool getBool(bool &result) const
  {
    if (!m_value.is_boolean()) return false;
    result = m_value.get<bool>();
    return true;
  }

  bool getDouble(double &result) const
  {
    if (m_value.type() != json2cpp::json::Type::Float) return false;
    result = m_value.get<double>();
    return std::isfinite(result);
  }

  bool getInteger(std::int64_t &result) const
  {
    if (m_value.type() == json2cpp::json::Type::Integer) {
      result = m_value.get<std::int64_t>();
      return true;
    }
    if (m_value.type() != json2cpp::json::Type::UInteger) return false;
    const auto value = m_value.get<std::uint64_t>();
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return false;
    result = static_cast<std::int64_t>(value);
    return true;
  }

  [[nodiscard]] std::optional<json2cppJsonObject> getObjectOptional() const
  {
    return m_value.is_object() ? std::optional{ json2cppJsonObject{ m_value } } : std::nullopt;
  }

  bool getObjectSize(std::size_t &result) const noexcept
  {
    if (!m_value.is_object()) return false;
    result = m_value.size();
    return true;
  }

  bool getString(std::string &result) const
  {
    if (!m_value.is_string()) return false;
    const auto value = m_value.get<std::string_view>();
    result.assign(value.data(), value.size());
    return true;
  }

  static constexpr bool hasStrictTypes() noexcept { return true; }

  [[nodiscard]] bool isArray() const noexcept { return m_value.is_array(); }
  [[nodiscard]] bool isBool() const noexcept { return m_value.is_boolean(); }

  [[nodiscard]] bool isDouble() const noexcept
  {
    return m_value.type() == json2cpp::json::Type::Float && std::isfinite(m_value.getNumber());
  }

  [[nodiscard]] bool isInteger() const
  {
    if (m_value.type() == json2cpp::json::Type::Integer) return true;
    return m_value.type() == json2cpp::json::Type::UInteger
           && m_value.get<std::uint64_t>() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  }

  [[nodiscard]] bool isNull() const noexcept
  {
    return m_value.is_null() || (m_value.type() == json2cpp::json::Type::Float && !std::isfinite(m_value.getNumber()));
  }

  [[nodiscard]] bool isNumber() const noexcept
  {
    return m_value.is_number() && (m_value.type() != json2cpp::json::Type::Float || std::isfinite(m_value.getNumber()));
  }

  [[nodiscard]] bool isObject() const noexcept { return m_value.is_object(); }
  [[nodiscard]] bool isString() const noexcept { return m_value.is_string(); }

private:
  const json2cpp::json &m_value;
};

class json2cppJsonAdapter
  : public BasicAdapter<json2cppJsonAdapter,
      json2cppJsonArray,
      json2cppJsonObjectMember,
      json2cppJsonObject,
      json2cppJsonValue>
{
public:
  json2cppJsonAdapter() = default;
  explicit json2cppJsonAdapter(const json2cpp::json &value) : BasicAdapter(json2cppJsonValue{ value }) {}
};

inline json2cppJsonArrayValueIterator::value_type json2cppJsonArrayValueIterator::operator*() const
{
  return json2cppJsonAdapter{ m_owner->at(m_index) };
}

inline DerefProxy<json2cppJsonArrayValueIterator::value_type> json2cppJsonArrayValueIterator::operator->() const
{
  return DerefProxy<value_type>{ **this };
}

inline json2cppJsonObjectMemberIterator::value_type json2cppJsonObjectMemberIterator::operator*() const
{
  const json2cpp::item_key_t key{ m_owner, m_index };
  return { std::string{ key.getString() }, json2cppJsonAdapter{ m_owner->at(m_index) } };
}

inline DerefProxy<json2cppJsonObjectMemberIterator::value_type> json2cppJsonObjectMemberIterator::operator->() const
{
  return DerefProxy<value_type>{ **this };
}

template<> struct AdapterTraits<json2cppJsonAdapter>
{
  using DocumentType = json2cpp::json;
  static std::string adapterName() { return "json2cppJsonAdapter"; }
};

inline bool json2cppJsonFrozenValue::equalTo(const Adapter &other, bool strict) const
{
  return json2cppJsonAdapter{ m_value }.equalTo(other, strict);
}

}// namespace valijson::adapters

#endif
