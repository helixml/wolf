add_library(wolf_input_bridge STATIC
    input_bridge.cpp
    input_bridge.hpp
)

target_include_directories(wolf_input_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(wolf_input_bridge PRIVATE fmt::fmt)
