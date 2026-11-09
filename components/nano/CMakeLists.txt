# The portable core: no ESP-IDF headers, builds as an IDF component on the
# target and as a plain static library for the host tests.
file(GLOB NANO_SRCS ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp)
if(COMMAND idf_component_register)
    idf_component_register(SRCS ${NANO_SRCS} INCLUDE_DIRS include)
else()
    add_library(nano STATIC ${NANO_SRCS})
    target_include_directories(nano PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
    target_compile_features(nano PUBLIC cxx_std_17)
    target_compile_options(nano PRIVATE -Wall -Wextra)
endif()
