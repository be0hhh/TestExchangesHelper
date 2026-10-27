#include "exchange_probe/App.hpp"
#include "exchange_probe/TbankGrpcDiagnostic.hpp"

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
    const exchange_probe::tbank_grpc::Factories factories{
        .nghttp2 = &exchange_probe::tbank_grpc::make_nghttp2_connection,
#if defined(CXET_EXCHANGE_PROBE_WITH_CXET)
        .cxet = &exchange_probe::tbank_grpc::make_cxet_connection,
#else
        .cxet = nullptr,
#endif
    };
    return exchange_probe::run_application(parsed.options, std::cout, std::cerr,
                                           &factories);
  } catch (const std::exception& error) {
    std::cerr << "internal_error: " << error.what() << '\n';
    return 4;
  } catch (...) {
    std::cerr << "internal_error: unknown_exception\n";
    return 4;
  }
}
