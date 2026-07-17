# Unit tests for loti-core (fast, no OMNeT++ / no sockets). Stage 0 wires the
# pipeline with a single smoke test; real tests arrive with the core modules.
add_executable(loti_core_tests
  test_main.cpp
  test_smoke.cpp
  test_hashing.cpp
  test_wire.cpp
  test_discovery.cpp
)
target_link_libraries(loti_core_tests PRIVATE loti_core)
target_include_directories(loti_core_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/test/doctest
  ${CMAKE_SOURCE_DIR}/test)

add_test(NAME loti_core_tests COMMAND loti_core_tests)
