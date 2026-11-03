add_executable(colgate_tests engine_tests.cpp)
target_link_libraries(colgate_tests PRIVATE colgate_engine)
target_include_directories(colgate_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME colgate_engine COMMAND colgate_tests)

add_executable(colgate_measure ${CMAKE_SOURCE_DIR}/tools/measure.cpp)
target_link_libraries(colgate_measure PRIVATE colgate_engine)

add_executable(colgate_render ${CMAKE_SOURCE_DIR}/tools/colgate_render.cpp)
target_link_libraries(colgate_render PRIVATE colgate_engine)

add_executable(colgate_bands ${CMAKE_SOURCE_DIR}/tools/colgate_bands.cpp)
target_link_libraries(colgate_bands PRIVATE colgate_engine)
