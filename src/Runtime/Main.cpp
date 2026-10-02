#include "exchange_probe/App.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
  try {
    const auto parsed = exchange_probe::parse_cli(argc, argv);
    if (!parsed.ok) {
      std::cerr << "configuration_error: " << parsed.error << '\n';
      exchange_probe::print_help(std::cerr);
      return 2;
    }
    return exchange_probe::run_application(parsed.options, std::cout, std::cerr);
  } catch (const std::exception& error) {
    std::cerr << "internal_error: " << error.what() << '\n';
    return 4;
  } catch (...) {
    std::cerr << "internal_error: unknown_exception\n";
    return 4;
  }
}
