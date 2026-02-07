#include <catch2/catch_test_macros.hpp>
#include "cpp26/inplace_vector.h"
#include <string>
#include <numeric>

TEST_CASE("inplace_vector default construction", "[inplace_vector]") {
    cpp26::inplace_vector<int, 10> v;
    REQUIRE(v.empty());
    REQUIRE(v.size() == 0);
    REQUIRE(v.capacity() == 10);
    REQUIRE(v.max_size() == 10);
}

TEST_CASE("inplace_vector push_back and emplace_back", "[inplace_vector]") {
    SECTION("push_back lvalue") {
        cpp26::inplace_vector<int, 5> v;
        int val = 42;
        v.push_back(val);
        REQUIRE(v.size() == 1);
        REQUIRE(v[0] == 42);
    }

    SECTION("push_back rvalue") {
        cpp26::inplace_vector<std::string, 5> v;
        v.push_back(std::string("hello"));
        REQUIRE(v.size() == 1);
        REQUIRE(v[0] == "hello");
    }

    SECTION("emplace_back") {
        cpp26::inplace_vector<std::string, 5> v;
        v.emplace_back(3, 'x');
        REQUIRE(v.size() == 1);
        REQUIRE(v[0] == "xxx");
    }

    SECTION("multiple push_back") {
        cpp26::inplace_vector<int, 8> v;
        for (int i = 0; i < 8; ++i) {
            v.push_back(i);
        }
        REQUIRE(v.size() == 8);
        for (int i = 0; i < 8; ++i) {
            REQUIRE(v[i] == i);
        }
    }
}

TEST_CASE("inplace_vector pop_back", "[inplace_vector]") {
    cpp26::inplace_vector<int, 5> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
    REQUIRE(v.size() == 3);

    v.pop_back();
    REQUIRE(v.size() == 2);
    REQUIRE(v.back() == 2);

    v.pop_back();
    REQUIRE(v.size() == 1);
    REQUIRE(v.back() == 1);
}

TEST_CASE("inplace_vector at() access", "[inplace_vector]") {
    cpp26::inplace_vector<int, 5> v;
    v.push_back(10);
    v.push_back(20);
    v.push_back(30);

    SECTION("valid index") {
        REQUIRE(v.at(0) == 10);
        REQUIRE(v.at(1) == 20);
        REQUIRE(v.at(2) == 30);
    }

    SECTION("out of range throws") {
        REQUIRE_THROWS_AS(v.at(3), std::out_of_range);
        REQUIRE_THROWS_AS(v.at(100), std::out_of_range);
    }
}

TEST_CASE("inplace_vector operator[] access", "[inplace_vector]") {
    cpp26::inplace_vector<int, 5> v;
    v.push_back(100);
    v.push_back(200);
    REQUIRE(v[0] == 100);
    REQUIRE(v[1] == 200);

    v[0] = 999;
    REQUIRE(v[0] == 999);
}

TEST_CASE("inplace_vector front() and back()", "[inplace_vector]") {
    cpp26::inplace_vector<int, 5> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);

    REQUIRE(v.front() == 1);
    REQUIRE(v.back() == 3);

    v.front() = 99;
    v.back() = 77;
    REQUIRE(v[0] == 99);
    REQUIRE(v[2] == 77);
}

TEST_CASE("inplace_vector size/empty/capacity/max_size", "[inplace_vector]") {
    cpp26::inplace_vector<int, 4> v;
    REQUIRE(v.empty());
    REQUIRE(v.size() == 0);
    REQUIRE(v.capacity() == 4);
    REQUIRE(v.max_size() == 4);

    v.push_back(1);
    REQUIRE(!v.empty());
    REQUIRE(v.size() == 1);
}

TEST_CASE("inplace_vector clear", "[inplace_vector]") {
    cpp26::inplace_vector<std::string, 5> v;
    v.push_back("a");
    v.push_back("b");
    v.push_back("c");
    REQUIRE(v.size() == 3);

    v.clear();
    REQUIRE(v.empty());
    REQUIRE(v.size() == 0);
}

TEST_CASE("inplace_vector copy constructor", "[inplace_vector]") {
    cpp26::inplace_vector<int, 5> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);

    cpp26::inplace_vector<int, 5> copy(v);
    REQUIRE(copy.size() == 3);
    REQUIRE(copy[0] == 1);
    REQUIRE(copy[1] == 2);
    REQUIRE(copy[2] == 3);

    // Modifying copy doesn't affect original
    copy[0] = 99;
    REQUIRE(v[0] == 1);
}

TEST_CASE("inplace_vector copy assignment", "[inplace_vector]") {
    cpp26::inplace_vector<int, 5> v;
    v.push_back(10);
    v.push_back(20);

    cpp26::inplace_vector<int, 5> other;
    other.push_back(99);
    other = v;
    REQUIRE(other.size() == 2);
    REQUIRE(other[0] == 10);
    REQUIRE(other[1] == 20);
}

TEST_CASE("inplace_vector move constructor", "[inplace_vector]") {
    cpp26::inplace_vector<std::string, 5> v;
    v.push_back("hello");
    v.push_back("world");

    cpp26::inplace_vector<std::string, 5> moved(std::move(v));
    REQUIRE(moved.size() == 2);
    REQUIRE(moved[0] == "hello");
    REQUIRE(moved[1] == "world");
    REQUIRE(v.empty());
}

TEST_CASE("inplace_vector move assignment", "[inplace_vector]") {
    cpp26::inplace_vector<std::string, 5> v;
    v.push_back("foo");
    v.push_back("bar");

    cpp26::inplace_vector<std::string, 5> other;
    other = std::move(v);
    REQUIRE(other.size() == 2);
    REQUIRE(other[0] == "foo");
    REQUIRE(other[1] == "bar");
    REQUIRE(v.empty());
}

TEST_CASE("inplace_vector iterator range", "[inplace_vector]") {
    cpp26::inplace_vector<int, 5> v;
    v.push_back(10);
    v.push_back(20);
    v.push_back(30);

    SECTION("begin/end") {
        auto it = v.begin();
        REQUIRE(*it == 10);
        ++it;
        REQUIRE(*it == 20);
        ++it;
        REQUIRE(*it == 30);
        ++it;
        REQUIRE(it == v.end());
    }

    SECTION("cbegin/cend") {
        auto it = v.cbegin();
        REQUIRE(*it == 10);
        REQUIRE(it != v.cend());
    }

    SECTION("rbegin/rend") {
        auto it = v.rbegin();
        REQUIRE(*it == 30);
        ++it;
        REQUIRE(*it == 20);
        ++it;
        REQUIRE(*it == 10);
        ++it;
        REQUIRE(it == v.rend());
    }
}

TEST_CASE("inplace_vector range-for loop", "[inplace_vector]") {
    cpp26::inplace_vector<int, 5> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);

    int sum = 0;
    for (const auto& val : v) {
        sum += val;
    }
    REQUIRE(sum == 6);
}

TEST_CASE("inplace_vector resize", "[inplace_vector]") {
    SECTION("grow") {
        cpp26::inplace_vector<int, 10> v;
        v.push_back(1);
        v.push_back(2);
        v.resize(5);
        REQUIRE(v.size() == 5);
        REQUIRE(v[0] == 1);
        REQUIRE(v[1] == 2);
        REQUIRE(v[2] == 0); // default-initialized
    }

    SECTION("shrink") {
        cpp26::inplace_vector<int, 10> v;
        for (int i = 0; i < 8; ++i) v.push_back(i);
        v.resize(3);
        REQUIRE(v.size() == 3);
        REQUIRE(v[0] == 0);
        REQUIRE(v[1] == 1);
        REQUIRE(v[2] == 2);
    }

    SECTION("resize to zero") {
        cpp26::inplace_vector<int, 5> v;
        v.push_back(1);
        v.resize(0);
        REQUIRE(v.empty());
    }

    SECTION("resize beyond capacity throws") {
        cpp26::inplace_vector<int, 3> v;
        REQUIRE_THROWS_AS(v.resize(4), std::bad_alloc);
    }
}

TEST_CASE("inplace_vector erase", "[inplace_vector]") {
    SECTION("erase single element") {
        cpp26::inplace_vector<int, 10> v;
        v.push_back(1);
        v.push_back(2);
        v.push_back(3);
        v.push_back(4);

        auto it = v.erase(v.begin() + 1); // erase '2'
        REQUIRE(v.size() == 3);
        REQUIRE(*it == 3);
        REQUIRE(v[0] == 1);
        REQUIRE(v[1] == 3);
        REQUIRE(v[2] == 4);
    }

    SECTION("erase range") {
        cpp26::inplace_vector<int, 10> v;
        for (int i = 0; i < 5; ++i) v.push_back(i); // 0,1,2,3,4

        auto it = v.erase(v.begin() + 1, v.begin() + 3); // erase 1,2
        REQUIRE(v.size() == 3);
        REQUIRE(*it == 3);
        REQUIRE(v[0] == 0);
        REQUIRE(v[1] == 3);
        REQUIRE(v[2] == 4);
    }

    SECTION("erase empty range is no-op") {
        cpp26::inplace_vector<int, 5> v;
        v.push_back(1);
        auto it = v.erase(v.begin(), v.begin());
        REQUIRE(v.size() == 1);
        REQUIRE(*it == 1);
    }
}

TEST_CASE("inplace_vector throws bad_alloc when capacity exceeded", "[inplace_vector]") {
    cpp26::inplace_vector<int, 2> v;
    v.push_back(1);
    v.push_back(2);
    REQUIRE_THROWS_AS(v.push_back(3), std::bad_alloc);
    REQUIRE_THROWS_AS(v.emplace_back(3), std::bad_alloc);
}

TEST_CASE("inplace_vector with non-trivial types (std::string)", "[inplace_vector]") {
    cpp26::inplace_vector<std::string, 4> v;
    v.push_back("alpha");
    v.emplace_back("beta");
    v.push_back(std::string("gamma"));

    REQUIRE(v.size() == 3);
    REQUIRE(v[0] == "alpha");
    REQUIRE(v[1] == "beta");
    REQUIRE(v[2] == "gamma");

    v.pop_back();
    REQUIRE(v.size() == 2);
    REQUIRE(v.back() == "beta");

    v.clear();
    REQUIRE(v.empty());
}
