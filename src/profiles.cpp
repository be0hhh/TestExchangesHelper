#include "exchange_probe/model.hpp"

#include "profile_builders.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace exchange_probe {

std::vector<ProductSpec> make_profiles() {
  std::vector<ProductSpec> products;
  products.reserve(28);
  append_major_profiles(products);
  append_misc_profiles(products);
  append_extended_profiles(products);

  std::set<std::pair<std::string, std::string>> identities;
  for (auto& product : products) {
    if (product.venue.empty() || product.product.empty()) {
      throw std::logic_error{"profile_identity_missing"};
    }
    if (!identities.emplace(product.venue, product.product).second) {
      throw std::logic_error{
          "duplicate_profile:" + product.venue + ":" + product.product};
    }
    finalize_capabilities(product);
  }
  std::sort(
      products.begin(),
      products.end(),
      [](const ProductSpec& lhs, const ProductSpec& rhs) {
        return std::tie(lhs.venue, lhs.product) <
               std::tie(rhs.venue, rhs.product);
      });
  return products;
}

}  // namespace exchange_probe
