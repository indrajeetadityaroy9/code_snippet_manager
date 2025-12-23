# DAM CLI tool

add_executable(dam-cli main.cpp)

# Use 'dam' as the output name
set_target_properties(dam-cli PROPERTIES OUTPUT_NAME dam)

target_link_libraries(dam-cli PRIVATE dam)
