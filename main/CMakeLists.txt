# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

file(GLOB app_sources "src/*.cpp" "src/*.c")

# Register the main component using only the filtered sources
idf_component_register(SRCS ${app_sources} 
                       INCLUDE_DIRS "include" "${CMAKE_CURRENT_BINARY_DIR}"
                       REQUIRES soc nvs_flash esp_http_server)
