#include <catch2/catch_test_macros.hpp>
#include "cpp26/hive.h"
#include <string>
#include <vector>
#include <algorithm>

TEST_CASE("hive default construction", "[hive]") {
    cpp26::hive<int> h;
    REQUIRE(h.empty());
    REQUIRE(h.size() == 0);
    REQUIRE(h.begin() == h.end());
}

TEST_CASE("hive insert and emplace", "[hive]") {
    SECTION("insert lvalue") {
        cpp26::hive<int> h;
        int val = 42;
        auto it = h.insert(val);
        REQUIRE(*it == 42);
        REQUIRE(h.size() == 1);
    }

    SECTION("insert rvalue") {
        cpp26::hive<std::string> h;
        auto it = h.insert(std::string("hello"));
        REQUIRE(*it == "hello");
        REQUIRE(h.size() == 1);
    }

    SECTION("emplace") {
        cpp26::hive<std::string> h;
        auto it = h.emplace(3, 'x');
        REQUIRE(*it == "xxx");
        REQUIRE(h.size() == 1);
    }

    SECTION("multiple inserts") {
        cpp26::hive<int> h;
        for (int i = 0; i < 100; ++i) {
            h.insert(i);
        }
        REQUIRE(h.size() == 100);
    }
}

TEST_CASE("hive erase with iterator", "[hive]") {
    cpp26::hive<int> h;
    auto it1 = h.insert(10);
    h.insert(20);
    h.insert(30);

    REQUIRE(h.size() == 3);
    h.erase(it1);
    REQUIRE(h.size() == 2);

    // Verify 10 is gone
    std::vector<int> remaining;
    for (auto& val : h) {
        remaining.push_back(val);
    }
    REQUIRE(std::find(remaining.begin(), remaining.end(), 10) == remaining.end());
    REQUIRE(remaining.size() == 2);
}

TEST_CASE("hive size tracking after insert/erase", "[hive]") {
    cpp26::hive<int> h;
    REQUIRE(h.size() == 0);

    auto it1 = h.insert(1);
    REQUIRE(h.size() == 1);
    auto it2 = h.insert(2);
    REQUIRE(h.size() == 2);
    auto it3 = h.insert(3);
    REQUIRE(h.size() == 3);

    h.erase(it2);
    REQUIRE(h.size() == 2);
    h.erase(it1);
    REQUIRE(h.size() == 1);
    h.erase(it3);
    REQUIRE(h.size() == 0);
    REQUIRE(h.empty());
}

TEST_CASE("hive empty", "[hive]") {
    cpp26::hive<int> h;
    REQUIRE(h.empty());

    h.insert(1);
    REQUIRE(!h.empty());

    h.erase(h.begin());
    REQUIRE(h.empty());
}

TEST_CASE("hive clear", "[hive]") {
    cpp26::hive<int> h;
    for (int i = 0; i < 50; ++i) {
        h.insert(i);
    }
    REQUIRE(h.size() == 50);

    h.clear();
    REQUIRE(h.empty());
    REQUIRE(h.size() == 0);
    REQUIRE(h.begin() == h.end());
}

TEST_CASE("hive iterator traversal skips erased elements", "[hive]") {
    cpp26::hive<int> h;
    h.insert(1);
    auto it2 = h.insert(2);
    h.insert(3);
    auto it4 = h.insert(4);
    h.insert(5);

    h.erase(it2);
    h.erase(it4);

    std::vector<int> values;
    for (auto& val : h) {
        values.push_back(val);
    }
    REQUIRE(values.size() == 3);
    // 2 and 4 should not appear
    REQUIRE(std::find(values.begin(), values.end(), 2) == values.end());
    REQUIRE(std::find(values.begin(), values.end(), 4) == values.end());
}

TEST_CASE("hive pointer stability", "[hive]") {
    cpp26::hive<int> h;

    auto it1 = h.insert(100);
    int* ptr1 = &*it1;

    // Insert many more to trigger new block allocations
    for (int i = 0; i < 200; ++i) {
        h.insert(i);
    }

    // The pointer to the first element must still be valid
    REQUIRE(*ptr1 == 100);

    // Erase some other elements
    auto it = h.begin();
    for (int i = 0; i < 50 && it != h.end(); ++i) {
        if (&*it != ptr1) {
            it = h.erase(it);
        } else {
            ++it;
        }
    }

    // ptr1 must still be valid
    REQUIRE(*ptr1 == 100);
}

TEST_CASE("hive copy constructor", "[hive]") {
    cpp26::hive<int> h;
    h.insert(10);
    h.insert(20);
    h.insert(30);

    cpp26::hive<int> copy(h);
    REQUIRE(copy.size() == 3);

    std::vector<int> origVals, copyVals;
    for (auto& v : h) origVals.push_back(v);
    for (auto& v : copy) copyVals.push_back(v);
    std::sort(origVals.begin(), origVals.end());
    std::sort(copyVals.begin(), copyVals.end());
    REQUIRE(origVals == copyVals);
}

TEST_CASE("hive copy assignment", "[hive]") {
    cpp26::hive<int> h;
    h.insert(1);
    h.insert(2);

    cpp26::hive<int> other;
    other.insert(99);
    other = h;

    REQUIRE(other.size() == 2);
    std::vector<int> vals;
    for (auto& v : other) vals.push_back(v);
    std::sort(vals.begin(), vals.end());
    REQUIRE(vals[0] == 1);
    REQUIRE(vals[1] == 2);
}

TEST_CASE("hive move constructor", "[hive]") {
    cpp26::hive<int> h;
    h.insert(10);
    h.insert(20);

    cpp26::hive<int> moved(std::move(h));
    REQUIRE(moved.size() == 2);
    // After move, source is in valid but unspecified state
}

TEST_CASE("hive move assignment", "[hive]") {
    cpp26::hive<int> h;
    h.insert(5);
    h.insert(6);

    cpp26::hive<int> other;
    other = std::move(h);
    REQUIRE(other.size() == 2);
    // After move, source is in valid but unspecified state
}

TEST_CASE("hive multiple blocks allocated when capacity exceeded", "[hive]") {
    // Use a small block size to force multiple blocks
    cpp26::hive<int> h(4);

    for (int i = 0; i < 20; ++i) {
        h.insert(i);
    }
    REQUIRE(h.size() == 20);
    // With block size 4, we should have at least 5 blocks worth of capacity
    REQUIRE(h.capacity() >= 20);

    // Verify all elements are accessible
    std::vector<int> vals;
    for (auto& v : h) vals.push_back(v);
    std::sort(vals.begin(), vals.end());
    REQUIRE(vals.size() == 20);
    for (int i = 0; i < 20; ++i) {
        REQUIRE(vals[i] == i);
    }
}

TEST_CASE("hive works with std::string", "[hive]") {
    cpp26::hive<std::string> h;
    h.insert("alpha");
    h.insert("beta");
    h.emplace("gamma");

    REQUIRE(h.size() == 3);

    std::vector<std::string> vals;
    for (auto& v : h) vals.push_back(v);
    std::sort(vals.begin(), vals.end());
    REQUIRE(vals[0] == "alpha");
    REQUIRE(vals[1] == "beta");
    REQUIRE(vals[2] == "gamma");
}

TEST_CASE("hive reshape when empty", "[hive]") {
    cpp26::hive<int> h;
    h.reshape(128);

    // Insert elements — block size should be 128
    for (int i = 0; i < 128; ++i) {
        h.insert(i);
    }
    REQUIRE(h.size() == 128);
    REQUIRE(h.capacity() == 128); // One full block of 128
}

TEST_CASE("hive freelist reuse after erase", "[hive]") {
    cpp26::hive<int> h(8);

    // Fill one block
    std::vector<cpp26::hive<int>::iterator> iters;
    for (int i = 0; i < 8; ++i) {
        iters.push_back(h.insert(i * 10));
    }
    REQUIRE(h.size() == 8);

    // Erase some
    h.erase(iters[2]);
    h.erase(iters[5]);
    REQUIRE(h.size() == 6);

    // Insert new elements — should reuse freed slots (no new block)
    h.insert(999);
    h.insert(888);
    REQUIRE(h.size() == 8);
    REQUIRE(h.capacity() == 8); // Still only one block
}
