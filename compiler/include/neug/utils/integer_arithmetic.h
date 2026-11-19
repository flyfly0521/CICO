#pragma once

#include <bit>
#include <cstdint>
#include <type_traits>

namespace neug::numeric {

// Runtime integer arithmetic is modulo 2^width. Perform the operation in an
// unsigned type, then preserve its bits instead of invoking signed-overflow UB
// or relying on an out-of-range unsigned-to-signed conversion. Widen small
// integers first so that integral promotion cannot overflow a signed int.
template <typename T>
inline constexpr bool isWrappingInteger =
    std::is_integral_v<T> && !std::is_same_v<T, bool> && sizeof(T) <= 8;

template <typename T>
constexpr T fromIntegerBits(uint64_t value) {
  using U = std::make_unsigned_t<T>;
  const U bits = static_cast<U>(value);
  if constexpr (std::is_signed_v<T>) {
    return std::bit_cast<T>(bits);
  } else {
    return bits;
  }
}

template <typename T>
constexpr T wrapAdd(T left, T right) {
  if constexpr (isWrappingInteger<T>) {
    using U = std::make_unsigned_t<T>;
    return fromIntegerBits<T>(static_cast<uint64_t>(static_cast<U>(left)) +
                              static_cast<uint64_t>(static_cast<U>(right)));
  } else {
    return left + right;
  }
}

template <typename T>
constexpr T wrapSub(T left, T right) {
  if constexpr (isWrappingInteger<T>) {
    using U = std::make_unsigned_t<T>;
    return fromIntegerBits<T>(static_cast<uint64_t>(static_cast<U>(left)) -
                              static_cast<uint64_t>(static_cast<U>(right)));
  } else {
    return left - right;
  }
}

template <typename T>
constexpr T wrapMul(T left, T right) {
  if constexpr (isWrappingInteger<T>) {
    using U = std::make_unsigned_t<T>;
    return fromIntegerBits<T>(static_cast<uint64_t>(static_cast<U>(left)) *
                              static_cast<uint64_t>(static_cast<U>(right)));
  } else {
    return left * right;
  }
}

}  // namespace neug::numeric
