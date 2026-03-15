// Copyright (c) 2011, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#include <memory>
#include <ranges>

namespace agi {
	namespace of_type_detail {
		/// Tag type returned from of_type<T>() to select the operator| overload
		template<class T> struct of_type_tag {};

		/// Take the address of a value and dynamic_cast it to Type*
		template<class Type>
		struct cast_to {
			typedef Type *result_type;

			template<class InType> Type *operator()(InType &ptr) const {
				return typeid(ptr) == typeid(Type) ? static_cast<Type*>(&ptr) : nullptr;
			}

			template<class InType> Type *operator()(std::unique_ptr<InType>& ptr) const {
				return (*this)(*ptr);
			}

			template<class InType> Type *operator()(std::unique_ptr<InType> const& ptr) const {
				return (*this)(*ptr);
			}

			template<class InType> Type *operator()(InType *ptr) const {
				return (*this)(*ptr);
			}
		};

		template<class Rng, class Type>
		inline auto operator|(Rng&& r, of_type_tag<Type>) {
			return std::forward<Rng>(r)
				| std::views::transform([](auto& value) { return cast_to<Type>{}(value); })
				| std::views::filter([](auto* ptr) { return ptr != nullptr; });
		}
	}

	template<class T>
	inline of_type_detail::of_type_tag<T> of_type() {
		return of_type_detail::of_type_tag<T>();
	}
}
