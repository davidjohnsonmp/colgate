add_library(colgate_engine STATIC
    ColgateEngine.cpp
    PerceptualField.cpp
    PhaseSpaceAnalyzer.cpp)

target_include_directories(colgate_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(colgate_engine PRIVATE /W4)
else()
    target_compile_options(colgate_engine PRIVATE -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion)
endif()
