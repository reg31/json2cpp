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
#include <compare>

namespace json2cpp {

    template<typename CharType> struct basic_json;    
    template<typename F, typename S> struct pair { F first; [[no_unique_address]] S second; };
	template<typename T> struct span
	{
	    template<std::size_t Size>
		constexpr explicit span(const std::array<T, Size> &input)
		: begin_{ input.data() }, end_{ std::next(input.data(), Size) } {}

	    constexpr span() : begin_{ nullptr }, end_{ nullptr } {}
	    [[nodiscard]] constexpr const T *begin() const noexcept { return begin_; }
		[[nodiscard]] constexpr const T *end() const noexcept { return end_; }
		[[nodiscard]] constexpr const T& operator[](std::size_t index) const { return begin_[index]; }
		[[nodiscard]] constexpr bool empty() const noexcept { return begin_ == end_; }
		[[nodiscard]] constexpr std::size_t size() const noexcept
		{
			return static_cast<std::size_t>(std::distance(begin_, end_));
		}

		const T *begin_;
		const T *end_;
	};

    template<typename CharType> using basic_value_pair_t = pair<std::basic_string_view<CharType>, basic_json<CharType>>;
    template<typename CharType> using basic_object_t = span<basic_value_pair_t<CharType>>;
    template<typename CharType> using basic_array_t = span<basic_json<CharType>>;
    
    template<typename CharType> struct basic_json {
		
		private:
        enum class Type { Null, Boolean, Binary, Array, Object, Integer, UInteger, Float, String };        
        Type value_type {Type::Null};
		
        union {
            bool boolean_value;
            basic_array_t<CharType> array_value;
            basic_object_t<CharType> object_value;
            std::int64_t int_value;
            std::uint64_t uint_value;
            double float_value;
            std::basic_string_view<CharType> string_value;
        };

		struct iterator {
			constexpr iterator() noexcept : container_type(Type::Null), index_(0), value_(nullptr) {}        
			constexpr iterator(Type type, const basic_json &value, std::size_t index = 0) noexcept
				: index_(index), value_(&value), container_type(type) {}
		  
			[[nodiscard]] constexpr const basic_json &operator*() const
			{
				if (container_type == Type::Array) return value_->array_data()[index_];
				else if (container_type == Type::Object) return value_->object_data()[index_].second;
				return *value_;
			}
		  
			[[nodiscard]] constexpr const basic_json *operator->() const noexcept { return &(operator*()); }
			[[nodiscard]] constexpr std::size_t index() const noexcept { return index_; }
			[[nodiscard]] constexpr auto operator<=>(const iterator&) const = default;
			constexpr iterator &operator++() noexcept { ++index_; return *this; }
			constexpr iterator &operator--() noexcept { --index_; return *this; }         
		  
			[[nodiscard]] constexpr std::basic_string_view<CharType> key() const
			{
				if (container_type == Type::Object) return value_->object_data()[index_].first;
				throw std::runtime_error("json value is not an object, it has no key");
			}

			private:
			std::size_t index_ { 0 };
			Type container_type {Type::Null};
			const basic_json *value_{ nullptr };
		};
        
        public:
        constexpr basic_json() noexcept = default;
        constexpr basic_json(bool v) noexcept : boolean_value{v}, value_type(Type::Boolean) {}
        constexpr basic_json(basic_array_t<CharType> v) noexcept : array_value{v}, value_type(Type::Array) {}
        constexpr basic_json(basic_object_t<CharType> v) noexcept : object_value{v}, value_type(Type::Object) {}
        constexpr basic_json(std::int64_t v) noexcept : int_value{v}, value_type(Type::Integer) {}
        constexpr basic_json(std::uint64_t v) noexcept : uint_value{v}, value_type(Type::UInteger) {}
        constexpr basic_json(double v) noexcept : float_value{v}, value_type(Type::Float) {}
        constexpr basic_json(std::basic_string_view<CharType> v) noexcept : string_value{v}, value_type(Type::String) {}
        constexpr basic_json(std::nullptr_t) noexcept : value_type(Type::Null) {}

        [[nodiscard]] constexpr iterator begin() const noexcept { return iterator{value_type, *this, 0}; }
        [[nodiscard]] constexpr iterator end() const noexcept {
            return iterator{value_type, *this, is_array() ? array_data().size() : is_object() ? object_data().size() : 1};
        }

        [[nodiscard]] constexpr const basic_array_t<CharType> &array_data() const {
            if (!is_array()) throw std::runtime_error("Not an array");
            return array_value;
        }

        [[nodiscard]] constexpr const basic_object_t<CharType> &object_data() const {
            if (!is_object()) throw std::runtime_error("Not an object");
            return object_value;
        }

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
        [[nodiscard]] constexpr bool is_primitive() const noexcept {
            return is_null() || is_string() || is_boolean() || is_number();
        }

        [[nodiscard]] constexpr std::size_t size() const noexcept {
            if (is_null()) return 0;
            if (is_object()) return object_data().size();
            if (is_array()) return array_data().size();
            return 1;
        }

        [[nodiscard]] constexpr bool empty() const noexcept { return size() == 0; }
        [[nodiscard]] constexpr const basic_json& operator[](std::size_t idx) const {
            if (!is_array() || idx >= array_data().size()) 
                throw std::runtime_error("Invalid array access");
            return array_data()[idx];
        }

        [[nodiscard]] constexpr const basic_json& at(const std::basic_string_view<CharType>& key) const {
            if (!is_object()) throw std::runtime_error("Not an object");
            
            auto it = std::find_if(object_data().begin(), object_data().end(), [&](const auto& p) { return p.first == key; });
            
            if (it == object_data().end()) throw std::out_of_range("Key not found");
            return it->second;
        }

        [[nodiscard]] constexpr const basic_json& operator[](const std::basic_string_view<CharType>& key) const { return at(key); }
        [[nodiscard]] constexpr iterator find(const std::basic_string_view<CharType>& key) const noexcept {
            if (is_object()) {
                auto it = std::find_if(object_data().begin(), object_data().end(), [&](const auto& p) { return p.first == key; });
                
                if (it != object_data().end())
                    return iterator{value_type, *this, static_cast<std::size_t>(it - object_data().begin())};
            }
            return end();
        }

        template<typename Key> [[nodiscard]] constexpr bool contains(const Key& key) const noexcept { return find(key) != end(); }        
        template<typename Type> [[nodiscard]] constexpr Type get() const
        {
            if constexpr (std::is_same_v<Type, std::uint64_t> || std::is_same_v<Type, std::int64_t> || std::is_same_v<Type, double>) {
                if (is_uinteger()) return uint_value;
                if (is_integer()) return int_value;
                if (is_float()) return float_value;
                throw std::runtime_error("Unexpected type: number requested");
            } else if constexpr (std::is_same_v<Type, std::basic_string_view<CharType>>) {
              if (is_string()) return string_value;
                throw std::runtime_error("Unexpected type: string-like requested");
            } else if constexpr (std::is_same_v<Type, bool>) {
              if (is_boolean()) return boolean_value;
                throw std::runtime_error("Unexpected type: bool requested");
            } else if constexpr (std::is_same_v<Type, std::nullptr_t>) {
              if (is_null()) return nullptr;
                throw std::runtime_error("Unexpected type: null requested");
            } else {
                throw std::runtime_error("Unexpected type for get()");
            }
        }
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

} // namespace json2cpp

#endif
