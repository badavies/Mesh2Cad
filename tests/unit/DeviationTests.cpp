// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <numeric>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "m2c/core/DeviationField.h"

using namespace m2c;
using Catch::Matchers::WithinAbs;

TEST_CASE("A deviation field summarises its distances", "[deviation]") {
    // -1.0, -0.9, ..., 0.9, 1.0 and one outlier.
    std::vector<double> d;
    for (int i = -10; i <= 10; ++i) d.push_back(i / 10.0);
    d.push_back(5.0);
    const auto field = DeviationField::create(d, 0.25);
    REQUIRE(field);
    const DeviationStats& s = field->stats();
    CHECK(s.count == 22);
    CHECK(s.minMm == -1.0);
    CHECK(s.maxMm == 5.0);
    CHECK_THAT(s.meanMm, WithinAbs(5.0 / 22.0, 1e-12));
    const double sum2 =
        std::accumulate(d.begin(), d.end(), 0.0, [](double a, double x) { return a + x * x; });
    CHECK_THAT(s.rmsMm, WithinAbs(std::sqrt(sum2 / 22.0), 1e-12));
    // Nearest rank: ceil(0.95 * 22) = 21st smallest |d|, which is 1.0.
    CHECK(s.p95AbsMm == 1.0);
    // |d| <= 0.25: -0.2 ... 0.2.
    CHECK_THAT(s.withinTolerance, WithinAbs(5.0 / 22.0, 1e-12));
    CHECK(field->distances() == d);
}

TEST_CASE("A deviation histogram covers the whole range", "[deviation]") {
    const auto field = *DeviationField::create({0.0, 0.1, 0.3, 0.35, 0.5, 1.0}, 0.1);
    const auto h = histogram(field, 5);
    REQUIRE(h);
    CHECK(h->lowMm == 0.0);
    CHECK_THAT(h->binWidthMm, WithinAbs(0.2, 1e-12));
    CHECK(h->counts == std::vector<std::size_t>{2, 2, 1, 0, 1});
    // One value: everything in the first bin.
    const auto flat = histogram(*DeviationField::create({0.3, 0.3}, 0.1), 4);
    REQUIRE(flat);
    CHECK(flat->counts == std::vector<std::size_t>{2, 0, 0, 0});
    CHECK_FALSE(histogram(field, 0));
}

TEST_CASE("A deviation field rejects bad input", "[deviation]") {
    CHECK_FALSE(DeviationField::create({}, 0.1));
    CHECK_FALSE(DeviationField::create({0.1}, 0.0));
    CHECK_FALSE(DeviationField::create({std::nan("")}, 0.1));
}
