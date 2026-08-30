cmake_minimum_required(VERSION 3.20)

project(exchange_api_probe VERSION 3.0.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS json)

add_library(exchange_probe_lib
    src/application.cpp
    src/cli.cpp
    src/compare.cpp
    src/contracts.cpp
    src/credentials.cpp
    src/gzip.cpp
    src/http_client.cpp
    src/model.cpp
    src/net_common.cpp
    src/output.cpp
    src/placement.cpp
    src/profiler.cpp
    src/profiles.cpp
    src/profiles_extended.cpp
    src/profiles_major.cpp
    src/profiles_misc.cpp
    src/probe_runner.cpp
    src/redaction.cpp
    src/research.cpp
    src/research_analysis.cpp
    src/research_capture.cpp
    src/research_commands.cpp
    src/research_profile.cpp
    src/sandbox.cpp
    src/viewer_server.cpp
    src/ws_client.cpp
)

target_include_directories(exchange_probe_lib PUBLIC include)
target_compile_definitions(exchange_probe_lib PRIVATE
    EXCHANGE_PROBE_SOURCE_PROFILE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/profiles"
    EXCHANGE_PROBE_INSTALL_PROFILE_ROOT="${CMAKE_INSTALL_FULL_DATADIR}/exchange-api-probe/profiles"
)
target_link_libraries(exchange_probe_lib
    PUBLIC
        OpenSSL::Crypto
        OpenSSL::SSL
        Boost::json
        Threads::Threads
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(exchange_probe_lib PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        -Wno-missing-field-initializers
    )
endif()

add_executable(exchange-api-probe src/main.cpp)
target_link_libraries(exchange-api-probe PRIVATE exchange_probe_lib)

add_executable(binance-ws-race src/binance_ws_race.cpp)
target_link_libraries(binance-ws-race PRIVATE exchange_probe_lib)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(binance-ws-race PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

if(CXET_EXCHANGE_FEED_RACE_BUILD)
    if(NOT TARGET cxet::cxet_lib OR NOT TARGET cxet::read_only_contracts)
        message(FATAL_ERROR
            "exchange-feed-race requires the CXET root source graph")
    endif()
    add_library(exchange_feed_race_lib
        src/race_analysis.cpp
        src/race_book.cpp
        src/race_bitget.cpp
        src/race_binance.cpp
        src/race_bybit.cpp
        src/race_capture.cpp
        src/race_capture_bitget.cpp
        src/race_capture_bybit.cpp
        src/race_catalog.cpp
        src/race_common.cpp
        src/race_dashboard.cpp
        src/race_gate.cpp
        src/race_ingress.cpp
        src/race_instruments.cpp
        src/race_kucoin.cpp
        src/race_match.cpp
        src/race_okx.cpp
        src/race_session.cpp
        src/race_aster.cpp
        src/race_report.cpp
        src/race_svg.cpp
        src/race_transport.cpp
        src/race_writer.cpp
    )
    target_compile_features(exchange_feed_race_lib PUBLIC cxx_std_23)
    target_include_directories(exchange_feed_race_lib
        PUBLIC include
        PRIVATE src
    )
    target_link_libraries(exchange_feed_race_lib
        PUBLIC
            exchange_probe_lib
            cxet::cxet_lib
            cxet::read_only_contracts
    )
    if(TARGET trading_core::algorithm)
        target_link_libraries(exchange_feed_race_lib PUBLIC trading_core::algorithm)
        target_compile_definitions(exchange_feed_race_lib
            PUBLIC EXCHANGE_FEED_RACE_HAS_RECONSTRUCTION=1)
    else()
        target_compile_definitions(exchange_feed_race_lib
            PUBLIC EXCHANGE_FEED_RACE_HAS_RECONSTRUCTION=0)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(exchange_feed_race_lib PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()

    add_library(exchange_bbo_reconstruction_probe_lib
        src/reconstruction_analysis.cpp
        src/reconstruction_campaign.cpp
        src/reconstruction_capture.cpp
        src/reconstruction_catalog.cpp
        src/reconstruction_provenance.cpp
        src/reconstruction_report.cpp
        src/reconstruction_selection.cpp
    )
    target_compile_features(exchange_bbo_reconstruction_probe_lib
        PUBLIC cxx_std_23)
    target_include_directories(exchange_bbo_reconstruction_probe_lib
        PUBLIC include)
    target_link_libraries(exchange_bbo_reconstruction_probe_lib
        PUBLIC
            cxet::cxet_lib
            cxet::read_only_contracts
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(exchange_bbo_reconstruction_probe_lib PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()

    add_executable(exchange-bbo-reconstruction-probe
        src/reconstruction_main.cpp)
    target_link_libraries(exchange-bbo-reconstruction-probe PRIVATE
        exchange_bbo_reconstruction_probe_lib)

    add_executable(exchange-bbo-reconstruction-tests
        tests/reconstruction_tests.cpp)
    target_link_libraries(exchange-bbo-reconstruction-tests PRIVATE
        exchange_bbo_reconstruction_probe_lib)
    add_test(NAME exchange-bbo-reconstruction-tests
        COMMAND exchange-bbo-reconstruction-tests)

    add_executable(exchange-feed-race-contract-tests tests/race_contract_tests.cpp)
    target_link_libraries(exchange-feed-race-contract-tests PRIVATE exchange_feed_race_lib)
    add_test(NAME exchange-feed-race-contract-tests COMMAND exchange-feed-race-contract-tests)

    add_executable(exchange-feed-race-semantics-tests tests/race_semantics_tests.cpp)
    target_link_libraries(exchange-feed-race-semantics-tests PRIVATE exchange_feed_race_lib)
    add_test(NAME exchange-feed-race-semantics-tests COMMAND exchange-feed-race-semantics-tests)

    add_executable(exchange-feed-race-catalog-tests tests/race_catalog_tests.cpp)
    target_link_libraries(exchange-feed-race-catalog-tests PRIVATE exchange_feed_race_lib)
    add_test(NAME exchange-feed-race-catalog-tests COMMAND exchange-feed-race-catalog-tests)

    add_executable(exchange-feed-race-transport-tests tests/race_transport_tests.cpp)
    target_link_libraries(exchange-feed-race-transport-tests PRIVATE exchange_feed_race_lib)
    add_test(NAME exchange-feed-race-transport-tests COMMAND exchange-feed-race-transport-tests)

    add_executable(exchange-feed-race-venue-tests tests/race_venue_tests.cpp)
    target_link_libraries(exchange-feed-race-venue-tests PRIVATE exchange_feed_race_lib)
    add_test(NAME exchange-feed-race-venue-tests COMMAND exchange-feed-race-venue-tests)

    add_executable(exchange-feed-race-report-tests tests/race_report_tests.cpp)
    target_link_libraries(exchange-feed-race-report-tests PRIVATE exchange_feed_race_lib)
    add_test(NAME exchange-feed-race-report-tests COMMAND exchange-feed-race-report-tests)

    add_executable(exchange-feed-race src/race_main.cpp)
    target_link_libraries(exchange-feed-race PRIVATE exchange_feed_race_lib)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(exchange-probe-cli-tests tests/cli_tests.cpp)
    target_link_libraries(exchange-probe-cli-tests PRIVATE exchange_probe_lib)
    add_test(NAME exchange-probe-cli-tests COMMAND exchange-probe-cli-tests)

    add_executable(exchange-probe-rest-contract-tests tests/rest_contract_tests.cpp)
    target_link_libraries(exchange-probe-rest-contract-tests PRIVATE exchange_probe_lib)
    add_test(NAME exchange-probe-rest-contract-tests COMMAND exchange-probe-rest-contract-tests)

    add_executable(exchange-probe-ws-contract-tests tests/ws_contract_tests.cpp)
    target_link_libraries(exchange-probe-ws-contract-tests PRIVATE exchange_probe_lib)
    add_test(NAME exchange-probe-ws-contract-tests COMMAND exchange-probe-ws-contract-tests)

    add_executable(exchange-probe-redaction-tests tests/redaction_tests.cpp)
    target_link_libraries(exchange-probe-redaction-tests PRIVATE exchange_probe_lib)
    add_test(NAME exchange-probe-redaction-tests COMMAND exchange-probe-redaction-tests)

    add_executable(exchange-probe-gzip-tests tests/gzip_tests.cpp)
    target_link_libraries(exchange-probe-gzip-tests PRIVATE exchange_probe_lib)
    add_test(NAME exchange-probe-gzip-tests COMMAND exchange-probe-gzip-tests)

    add_executable(exchange-probe-compare-tests tests/compare_tests.cpp)
    target_link_libraries(exchange-probe-compare-tests PRIVATE exchange_probe_lib)
    add_test(NAME exchange-probe-compare-tests COMMAND exchange-probe-compare-tests)

    add_executable(exchange-probe-research-tests tests/research_tests.cpp)
    target_link_libraries(exchange-probe-research-tests PRIVATE exchange_probe_lib)
    target_compile_definitions(exchange-probe-research-tests PRIVATE
        EXCHANGE_PROBE_TEST_PROFILE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/profiles"
    )
    add_test(NAME exchange-probe-research-tests COMMAND exchange-probe-research-tests)

    add_test(NAME binance-ws-race-self-test COMMAND binance-ws-race --self-test)
endif()

install(TARGETS exchange-api-probe RUNTIME DESTINATION bin)
install(DIRECTORY profiles schemas DESTINATION share/exchange-api-probe)
install(FILES doc/ADAPTER_PROTOCOL.md
    DESTINATION share/doc/exchange-api-probe)
