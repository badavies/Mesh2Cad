// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string_view>

namespace m2c {

/// Told how far a long operation has got: a short stage name ("fitting the cage") and the
/// fraction of that stage done, from 0 to 1, or a negative number when it is not known. Called
/// on the thread doing the work. May be empty.
using Progress = std::function<void(std::string_view stage, double fraction)>;

/// Calls `progress` if it is set.
inline void report(const Progress& progress, std::string_view stage, double fraction = -1.0) {
    if (progress) progress(stage, fraction);
}

} // namespace m2c
