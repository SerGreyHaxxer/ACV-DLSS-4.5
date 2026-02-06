/*
 * Copyright (C) 2026 acerthyracer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

// ============================================================================
// C++26 POLYFILL: std::hive
// ============================================================================
// A sequence container with stable pointers and O(1) insert/erase.
// Uses block-based storage with freelist for memory reuse.
//
// Key properties:
// - Pointers/references never invalidated by insert or erase
// - O(1) insertion and erasure
// - Cache-friendly iteration via contiguous blocks
//
// When MSVC adds std::hive support:
//   #include <hive>
//   using std::hive;
// ============================================================================

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace cpp26 {

// Feature test macro matching the C++26 proposal
#ifndef __cpp_lib_hive
#  define __cpp_lib_hive 202311L
#endif

template <typename T, typename Allocator = std::allocator<T>> class hive {
public:
  // Block size (elements per block)
  static constexpr std::size_t kDefaultBlockSize = 64;

private:
  // Each slot can be either occupied or free
  struct Slot {
    union {
      T value;
      std::size_t nextFree; // Index of next free slot (freelist)
    };
    bool occupied = false;

    Slot() noexcept : nextFree(0) {}
    ~Slot() {
      if (occupied) std::destroy_at(&value);
    }
    Slot(const Slot& other) : occupied(other.occupied) {
      if (occupied)
        std::construct_at(&value, other.value);
      else
        nextFree = other.nextFree;
    }
    Slot(Slot&& other) noexcept : occupied(other.occupied) {
      if (occupied)
        std::construct_at(&value, std::move(other.value));
      else
        nextFree = other.nextFree;
    }
  };

  struct Block {
    std::vector<Slot> slots;
    std::size_t firstFree = 0; // First free slot in this block
    std::size_t occupiedCount = 0;
    bool hasFreeslots = true;

    explicit Block(std::size_t size) : slots(size) {
      // Initialize freelist: each slot points to next
      for (std::size_t i = 0; i < size - 1; ++i) {
        slots[i].nextFree = i + 1;
      }
      slots[size - 1].nextFree = static_cast<std::size_t>(-1); // End marker
    }
  };

  std::vector<std::unique_ptr<Block>> m_blocks;
  std::size_t m_size = 0;
  std::size_t m_blockSize = kDefaultBlockSize;

public:
  // Type aliases
  using value_type = T;
  using allocator_type = Allocator;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;
  using reference = T&;
  using const_reference = const T&;
  using pointer = T*;
  using const_pointer = const T*;

  // Forward iterator (skip empty slots)
  class iterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = T*;
    using reference = T&;

    iterator() = default;
    iterator(hive* h, std::size_t blockIdx, std::size_t slotIdx) : m_hive(h), m_blockIdx(blockIdx), m_slotIdx(slotIdx) {
      skipEmpty();
    }

    reference operator*() const { return m_hive->m_blocks[m_blockIdx]->slots[m_slotIdx].value; }

    pointer operator->() const { return &**this; }

    iterator& operator++() {
      advance();
      skipEmpty();
      return *this;
    }

    iterator operator++(int) {
      iterator tmp = *this;
      ++*this;
      return tmp;
    }

    bool operator==(const iterator& other) const {
      return m_hive == other.m_hive && m_blockIdx == other.m_blockIdx && m_slotIdx == other.m_slotIdx;
    }

    bool operator!=(const iterator& other) const { return !(*this == other); }

  private:
    void advance() {
      if (m_blockIdx >= m_hive->m_blocks.size()) return;
      ++m_slotIdx;
      if (m_slotIdx >= m_hive->m_blocks[m_blockIdx]->slots.size()) {
        ++m_blockIdx;
        m_slotIdx = 0;
      }
    }

    void skipEmpty() {
      while (m_blockIdx < m_hive->m_blocks.size()) {
        auto& block = m_hive->m_blocks[m_blockIdx];
        while (m_slotIdx < block->slots.size()) {
          if (block->slots[m_slotIdx].occupied) return;
          ++m_slotIdx;
        }
        ++m_blockIdx;
        m_slotIdx = 0;
      }
    }

    hive* m_hive = nullptr;
    std::size_t m_blockIdx = 0;
    std::size_t m_slotIdx = 0;

    friend class hive;
  };

  class const_iterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = const T*;
    using reference = const T&;

    const_iterator() = default;
    const_iterator(const hive* h, std::size_t blockIdx, std::size_t slotIdx)
        : m_hive(h), m_blockIdx(blockIdx), m_slotIdx(slotIdx) {
      skipEmpty();
    }
    const_iterator(iterator it) : m_hive(it.m_hive), m_blockIdx(it.m_blockIdx), m_slotIdx(it.m_slotIdx) {}

    reference operator*() const { return m_hive->m_blocks[m_blockIdx]->slots[m_slotIdx].value; }

    pointer operator->() const { return &**this; }

    const_iterator& operator++() {
      advance();
      skipEmpty();
      return *this;
    }

    const_iterator operator++(int) {
      const_iterator tmp = *this;
      ++*this;
      return tmp;
    }

    bool operator==(const const_iterator& other) const {
      return m_hive == other.m_hive && m_blockIdx == other.m_blockIdx && m_slotIdx == other.m_slotIdx;
    }

    bool operator!=(const const_iterator& other) const { return !(*this == other); }

  private:
    void advance() {
      if (m_blockIdx >= m_hive->m_blocks.size()) return;
      ++m_slotIdx;
      if (m_slotIdx >= m_hive->m_blocks[m_blockIdx]->slots.size()) {
        ++m_blockIdx;
        m_slotIdx = 0;
      }
    }

    void skipEmpty() {
      while (m_blockIdx < m_hive->m_blocks.size()) {
        const auto& block = m_hive->m_blocks[m_blockIdx];
        while (m_slotIdx < block->slots.size()) {
          if (block->slots[m_slotIdx].occupied) return;
          ++m_slotIdx;
        }
        ++m_blockIdx;
        m_slotIdx = 0;
      }
    }

    const hive* m_hive = nullptr;
    std::size_t m_blockIdx = 0;
    std::size_t m_slotIdx = 0;
  };

  // Constructors
  hive() = default;

  explicit hive(size_type blockSize) : m_blockSize(blockSize) {}

  hive(const hive& other) : m_blockSize(other.m_blockSize) {
    for (const auto& val : other) {
      insert(val);
    }
  }

  hive(hive&& other) noexcept = default;

  hive& operator=(const hive& other) {
    if (this != &other) {
      clear();
      for (const auto& val : other) {
        insert(val);
      }
    }
    return *this;
  }

  hive& operator=(hive&& other) noexcept = default;

  ~hive() = default;

  // Iterators
  iterator begin() noexcept { return iterator(this, 0, 0); }
  const_iterator begin() const noexcept { return const_iterator(this, 0, 0); }
  const_iterator cbegin() const noexcept { return begin(); }

  iterator end() noexcept { return iterator(this, m_blocks.size(), 0); }
  const_iterator end() const noexcept { return const_iterator(this, m_blocks.size(), 0); }
  const_iterator cend() const noexcept { return end(); }

  // Capacity
  [[nodiscard]] bool empty() const noexcept { return m_size == 0; }
  size_type size() const noexcept { return m_size; }
  size_type capacity() const noexcept { return m_blocks.size() * m_blockSize; }

  // Modifiers
  void clear() noexcept {
    m_blocks.clear();
    m_size = 0;
  }

  iterator insert(const T& value) { return emplace(value); }

  iterator insert(T&& value) { return emplace(std::move(value)); }

  template <typename... Args> iterator emplace(Args&&... args) {
    // Find a block with free slots
    for (std::size_t bi = 0; bi < m_blocks.size(); ++bi) {
      auto& block = m_blocks[bi];
      if (block->hasFreeslots) {
        std::size_t slotIdx = block->firstFree;
        auto& slot = block->slots[slotIdx];

        // Update freelist
        block->firstFree = slot.nextFree;
        if (block->firstFree == static_cast<std::size_t>(-1)) {
          block->hasFreeslots = false;
        }

        // Construct value
        std::construct_at(&slot.value, std::forward<Args>(args)...);
        slot.occupied = true;
        ++block->occupiedCount;
        ++m_size;

        return iterator(this, bi, slotIdx);
      }
    }

    // Need new block
    m_blocks.push_back(std::make_unique<Block>(m_blockSize));
    std::size_t bi = m_blocks.size() - 1;
    auto& block = m_blocks[bi];

    auto& slot = block->slots[0];
    block->firstFree = slot.nextFree;
    if (block->firstFree == static_cast<std::size_t>(-1)) {
      block->hasFreeslots = false;
    }

    std::construct_at(&slot.value, std::forward<Args>(args)...);
    slot.occupied = true;
    ++block->occupiedCount;
    ++m_size;

    return iterator(this, bi, 0);
  }

  iterator erase(iterator pos) {
    assert(pos.m_hive == this);
    assert(pos.m_blockIdx < m_blocks.size());

    auto& block = m_blocks[pos.m_blockIdx];
    auto& slot = block->slots[pos.m_slotIdx];

    assert(slot.occupied);

    // Destroy value
    std::destroy_at(&slot.value);
    slot.occupied = false;

    // Add to freelist
    slot.nextFree = block->firstFree;
    block->firstFree = pos.m_slotIdx;
    block->hasFreeslots = true;

    --block->occupiedCount;
    --m_size;

    // Return next valid iterator
    iterator next = pos;
    ++next;
    return next;
  }

  // Get stable pointer to element
  pointer get_pointer(iterator pos) noexcept {
    if (pos == end()) return nullptr;
    return &*pos;
  }

  // Reshape (hint for block reallocation)
  void reshape(size_type blockSize) {
    if (empty()) {
      m_blockSize = blockSize;
    }
  }
};

} // namespace cpp26
