#pragma once

#include "exchange_probe/Model.hpp"

#include <vector>

namespace exchange_probe {

void append_major_profiles(std::vector<ProductSpec>& products);
void append_extended_profiles(std::vector<ProductSpec>& products);
void append_misc_profiles(std::vector<ProductSpec>& products);

}  // namespace exchange_probe
