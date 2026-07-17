# Unit tests for loti-core (fast, no OMNeT++ / no sockets). Stage 0 wires the
# pipeline with a single smoke test; real tests arrive with the core modules.
add_executable(loti_core_tests
  test_main.cpp
  test_smoke.cpp
  test_hashing.cpp
  test_wire.cpp
  test_discovery.cpp
  test_keystore.cpp
  ${CMAKE_SOURCE_DIR}/adapters/os/keystore.cpp  # the crypto adapter under test
)
target_link_libraries(loti_core_tests PRIVATE loti_core OpenSSL::Crypto)
target_include_directories(loti_core_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/test/doctest
  ${CMAKE_SOURCE_DIR}/test
  ${CMAKE_SOURCE_DIR})  # for adapters/os/keystore.hpp

add_test(NAME loti_core_tests COMMAND loti_core_tests)
