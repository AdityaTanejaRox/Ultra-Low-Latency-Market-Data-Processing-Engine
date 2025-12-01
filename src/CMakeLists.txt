add_library(common STATIC
  common/net.cpp
)
target_include_directories(common PUBLIC ${CMAKE_SOURCE_DIR}/include)

add_library(engine_core STATIC
  engine/order_book.cpp
  engine/engine.cpp
)
target_include_directories(engine_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(engine_core PRIVATE common)

add_executable(engine_app engine/main.cpp)
target_link_libraries(engine_app PRIVATE engine_core common)

add_executable(streamer_app streamer/main.cpp streamer/streamer.cpp)
target_link_libraries(streamer_app PRIVATE common)

target_include_directories(engine_core PUBLIC ${CMAKE_SOURCE_DIR}/third_party)
target_include_directories(common      PUBLIC ${CMAKE_SOURCE_DIR}/third_party)
