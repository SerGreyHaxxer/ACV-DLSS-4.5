/*
 * Copyright (C) 2026 acerthyracer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <type_traits>

namespace cpp26 {

/**
 * @brief A std::inplace_vector polyfill for C++23/20.
 *
 * A dynamically-resizable vector with fixed capacity and inline storage.
 * Does not perform any dynamic memory allocation.
 *
 * @tparam T The type of elements.
 * @tparam N The capacity of the vector.
 */
template <typename T, std::size_t N>
class inplace_vector {
public:
  using value_type = T;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = value_type*;
  using const_pointer = const value_type*;
  using iterator = T*;
  using const_iterator = const T*;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  constexpr inplace_vector() noexcept = default;

  constexpr inplace_vector(const inplace_vector& other)
      noexcept(std::is_nothrow_copy_constructible_v<T>) {
    for (const auto& item : other) {
      push_back(item);
    }
  }

  constexpr inplace_vector(inplace_vector&& other)
      noexcept(std::is_nothrow_move_constructible_v<T>) {
    for (auto& item : other) {
      push_back(std::move(item));
    }
    other.clear();
  }

  constexpr inplace_vector& operator=(const inplace_vector& other)
      noexcept(std::is_nothrow_copy_assignable_v<T>) {
    if (this != &other) {
      clear();
      for (const auto& item : other) {
        push_back(item);
      }
    }
    return *this;
  }

  constexpr inplace_vector& operator=(inplace_vector&& other)
      noexcept(std::is_nothrow_move_assignable_v<T>) {
    if (this != &other) {
      clear();
      for (auto& item : other) {
        push_back(std::move(item));
      }
      other.clear();
    }
    return *this;
  }

  constexpr ~inplace_vector() {
    clear();
  }

  // Element access
  constexpr reference at(size_type pos) {
    if (pos >= size()) throw std::out_of_range("inplace_vector::at");
    return data()[pos];
  }
  constexpr const_reference at(size_type pos) const {
    if (pos >= size()) throw std::out_of_range("inplace_vector::at");
    return data()[pos];
  }

  constexpr reference operator[](size_type pos) {
    assert(pos < size());
    return data()[pos];
  }
  constexpr const_reference operator[](size_type pos) const {
    assert(pos < size());
    return data()[pos];
  }

  constexpr reference front() { return data()[0]; }
  constexpr const_reference front() const { return data()[0]; }
  constexpr reference back() { return data()[size() - 1]; }
  constexpr const_reference back() const { return data()[size() - 1]; }
  constexpr T* data() noexcept { return reinterpret_cast<T*>(m_storage.data()); }
  constexpr const T* data() const noexcept { return reinterpret_cast<const T*>(m_storage.data()); }

  // Iterators
  constexpr iterator begin() noexcept { return data(); }
  constexpr const_iterator begin() const noexcept { return data(); }
  constexpr const_iterator cbegin() const noexcept { return data(); }
  constexpr iterator end() noexcept { return data() + m_size; }
  constexpr const_iterator end() const noexcept { return data() + m_size; }
  constexpr const_iterator cend() const noexcept { return data() + m_size; }

  constexpr reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
  constexpr const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
  constexpr reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
  constexpr const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }

  // Capacity
  constexpr bool empty() const noexcept { return m_size == 0; }
  constexpr size_type size() const noexcept { return m_size; }
  constexpr size_type max_size() const noexcept { return N; }
  constexpr size_type capacity() const noexcept { return N; }

  // Modifiers
  constexpr void clear() noexcept {
    if constexpr (!std::is_trivially_destructible_v<T>) {
      for (size_type i = 0; i < m_size; ++i) {
        data()[i].~T();
      }
    }
    m_size = 0;
  }

  constexpr reference push_back(const T& value) {
    if (m_size >= N) throw std::bad_alloc(); // Standard says throw, or maybe UB in some drafts.
    new (data() + m_size) T(value);
    return data()[m_size++];
  }

  constexpr reference push_back(T&& value) {
    if (m_size >= N) throw std::bad_alloc();
    new (data() + m_size) T(std::move(value));
    return data()[m_size++];
  }

  template <typename... Args>
  constexpr reference emplace_back(Args&&... args) {
    if (m_size >= N) throw std::bad_alloc();
    new (data() + m_size) T(std::forward<Args>(args)...);
    return data()[m_size++];
  }

  constexpr void pop_back() {
    assert(m_size > 0);
    if constexpr (!std::is_trivially_destructible_v<T>) {
      data()[m_size - 1].~T();
    }
    m_size--;
  }

  constexpr void resize(size_type count) {
    if (count > N) throw std::bad_alloc();
    if (count < m_size) {
      if constexpr (!std::is_trivially_destructible_v<T>) {
        for (size_type i = count; i < m_size; ++i) {
          data()[i].~T();
        }
      }
    } else if (count > m_size) {
      for (size_type i = m_size; i < count; ++i) {
        new (data() + i) T();
      }
    }
    m_size = count;
  }

  // Extensions for STL compatibility
  constexpr iterator erase(const_iterator pos) {
    iterator p = const_cast<iterator>(pos);
    if (p == end()) return end();
    if (p + 1 != end()) {
      std::move(p + 1, end(), p);
    }
    pop_back();
    return p;
  }
  
  constexpr iterator erase(const_iterator first, const_iterator last) {
      iterator p_first = const_cast<iterator>(first);
      iterator p_last = const_cast<iterator>(last);
      if (p_first == p_last) return p_first;
      
      difference_type count = std::distance(p_first, p_last);
      std::move(p_last, end(), p_first);
      
      for(size_type i=0; i<count; ++i) pop_back();
      
      return p_first;
  }

private:
  alignas(T) std::array<std::byte, sizeof(T) * N> m_storage;
  size_type m_size = 0;
};

} // namespace cpp26
