// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Serial stand-in for Intel's parallel stable sort, which Instant Meshes uses (ADR-009).

#include <algorithm>

namespace pss {

template <class Iterator, class Compare>
void parallel_stable_sort(Iterator first, Iterator last, Compare compare) {
    std::stable_sort(first, last, compare);
}

} // namespace pss
