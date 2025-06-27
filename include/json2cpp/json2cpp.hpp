#ifndef CONSTEXPR_JSON_HPP_INCLUDED
#define CONSTEXPR_JSON_HPP_INCLUDED

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <span>

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
    enum class Type : uint8_t { Null, Boolean, Array, Object, Integer, UInteger, Float, String };

    static constexpr size_t TYPE_SHIFT = sizeof(size_t) * 8 - 4;
    static constexpr size_t LENGTH_MASK = (1ULL << TYPE_SHIFT) - 1;
	static constexpr size_t capacity = sizeof(CharType*) / sizeof(CharType);

    size_t type_and_length_ = 0;
    
    union Data {
        const basic_json<CharType> *array_value;
        const basic_value_pair_t<CharType> *object_value;
		std::array<CharType, capacity> short_data{};
		const CharType* long_data;
        int64_t int_value;
        uint64_t uint_value;
        double float_value;
        bool boolean_value;
    } data_;

    constexpr void set_type_and_length(Type t, size_t len) noexcept {
        type_and_length_ = (static_cast<size_t>(t) << TYPE_SHIFT) | (len & LENGTH_MASK);
    }

    constexpr Type type() const noexcept { return static_cast<Type>(type_and_length_ >> TYPE_SHIFT); }
    constexpr const CharType* string_data() const noexcept
    {
        return size() <= capacity ? data_.short_data.data() : data_.long_data;
    }

    constexpr basic_array_t<CharType> array_data() const { return { data_.array_value, size() }; }
    constexpr basic_object_t<CharType> object_data() const { return { data_.object_value, size() }; }

public:
    constexpr basic_json() noexcept { set_type_and_length(Type::Null, 0); }
	constexpr basic_json(std::nullptr_t) noexcept { set_type_and_length(Type::Null, 0); }
    constexpr basic_json(bool v) noexcept : data_{ .boolean_value = v } { set_type_and_length(Type::Boolean, 0); }
    constexpr basic_json(basic_array_t<CharType> v) noexcept : data_{ .array_value = v.data() } { set_type_and_length(Type::Array, v.size()); }
    constexpr basic_json(basic_object_t<CharType> v) noexcept : data_{ .object_value = v.data() } { set_type_and_length(Type::Object, v.size()); }
    constexpr basic_json(int64_t v) noexcept : data_{ .int_value = v } { set_type_and_length(Type::Integer, 0); }
    constexpr basic_json(uint64_t v) noexcept : data_{ .uint_value = v } { set_type_and_length(Type::UInteger, 0); }
    constexpr basic_json(double v) noexcept : data_{ .float_value = v } { set_type_and_length(Type::Float, 0); }
    constexpr basic_json(std::basic_string_view<CharType> v) noexcept
    {
        const size_t len = v.size();
		set_type_and_length(Type::String, len);
		
        if (len <= capacity) {
            std::ranges::copy(v, data_.short_data.begin());
        } else {
            data_.long_data = v.data();
        }
    }

    constexpr bool is_object() const noexcept { return type() == Type::Object; }
    constexpr bool is_array() const noexcept { return type() == Type::Array; }
    constexpr bool is_string() const noexcept { return type() == Type::String; }
    constexpr bool is_uinteger() const noexcept { return type() == Type::UInteger; }
    constexpr bool is_integer() const noexcept { return type() == Type::Integer; }
    constexpr bool is_float() const noexcept { return type() == Type::Float; }
    constexpr bool is_boolean() const noexcept { return type() == Type::Boolean; }
    constexpr bool is_null() const noexcept { return type() == Type::Null; }
    constexpr bool is_number_integer() const noexcept { return is_integer() || is_uinteger(); }
    constexpr bool is_number_float() const noexcept { return is_float(); }
    constexpr bool is_number() const noexcept { return is_number_integer() || is_number_float(); }
    constexpr bool is_structured() const noexcept { return is_object() || is_array(); }
    constexpr bool is_primitive() const noexcept { return !is_structured(); }

	constexpr size_t size() const noexcept { return type_and_length_ & LENGTH_MASK; }
    constexpr bool empty() const noexcept { return size() == 0; }
	
	constexpr const basic_json &operator[](std::basic_string_view<CharType> key) const {
		return find(key)->second;        
	}
	
    constexpr const basic_json &operator[](size_t index) const
	{
        const Type t = type();
		if (t == Type::Array) return array_data()[index];
		if (t == Type::Object) return object_data()[index].second;
        throw std::runtime_error("JSON value is not an array or object");
	}
    
    constexpr bool operator==(std::basic_string_view<CharType> other) const noexcept
    {
		const auto len = size();
        if (!is_string() || len != other.length())	return false;
           
		const CharType *this_data = string_data();
		const CharType *other_data = other.data();
		
		return std::equal(this_data, this_data + len, other_data);
    }
	
	constexpr const basic_json &at(std::basic_string_view<CharType> key) const
    {
        if (!is_object()) throw std::runtime_error("JSON value is not an object");
		if (const auto it = find(key); it != object_data().end()) {
            return it->second;
        }
        throw std::out_of_range("JSON key not found");
    }
	
	constexpr const basic_json &at(size_t index) const
    {
		if (index < size())
			return this->operator[](index);
		throw std::runtime_error("JSON value is either not a container or is out of range");
    }

    constexpr auto find(std::basic_string_view<CharType> key) const
    {
        if (!is_object()) throw std::runtime_error("JSON value is not an object");
		const auto object = object_data();
		return std::ranges::find_if(object, [key](const auto &pair) { return pair.first == key; });
    }

    constexpr bool contains(std::basic_string_view<CharType> key) const noexcept
	{ 
		if (is_object())
			return find(key) != object_data().end();
		
		if(is_array())
		{
			const auto arr = array_data();
			auto it = std::ranges::find_if(arr, [key](const auto& element) { 
				return element.is_string() && element == key; 
			});
			return it != arr.end();
		}
		return false;
	}
	
    constexpr basic_object_t<CharType> items() const {
		if (!is_object()) throw std::runtime_error("JSON value is not an object");
		return object_data(); 
	}
	
	constexpr auto begin() const {
		if (!is_array()) throw std::runtime_error("JSON value is not an array");
		return array_data().begin();
	}
	constexpr auto end() const {
		if (!is_array()) throw std::runtime_error("JSON value is not an array");
		return array_data().end();
	}

    constexpr std::basic_string_view<CharType> getString() const {
        if (!is_string()) throw std::runtime_error("Not a string");
        return { string_data(), size() };
    }
    
    constexpr double getNumber() const {
		if (!is_number()) throw std::runtime_error("Not a number");
        return get<double>();
    }

    template<typename ValueType> constexpr ValueType get() const
    {
        if constexpr (std::is_same_v<ValueType, bool>) {
            if (!is_boolean()) throw std::runtime_error("JSON value is not a boolean");
            return data_.boolean_value;
        } else if constexpr (std::is_arithmetic_v<ValueType>) {
            if (is_uinteger()) return static_cast<ValueType>(data_.uint_value);
            if (is_integer()) return static_cast<ValueType>(data_.int_value);
            if (is_float()) return static_cast<ValueType>(data_.float_value);
            throw std::runtime_error("JSON value is not a number");
        } else if constexpr (std::is_same_v<ValueType, std::basic_string_view<CharType>>) {
            return getString();
        } else if constexpr (std::is_same_v<ValueType, std::nullptr_t>) {
            if (!is_null()) throw std::runtime_error("JSON value is not null");
            return nullptr;
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

}

#endif
