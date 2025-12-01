add_executable(tests_book tests_book.cpp)

target_link_libraries(tests_book
PRIVATE
    engine_core
    gtest_main
)

target_include_directories(tests_book PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_sources(tests_book PRIVATE ${CMAKE_SOURCE_DIR}/src/engine/order_book.cpp)

add_test(NAME tests_book COMMAND tests_book)
