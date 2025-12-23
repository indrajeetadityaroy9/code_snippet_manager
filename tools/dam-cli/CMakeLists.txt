# DAM CLI tool

set(DAM_CLI_SOURCES main.cpp)

# Add interactive editor sources if LLM is enabled
if(DAM_ENABLE_LLM)
    list(APPEND DAM_CLI_SOURCES
        interactive/terminal.cpp
        interactive/interactive_editor.cpp
        llm/claude_client.cpp
        llm/input_classifier.cpp
    )
endif()

add_executable(dam-cli ${DAM_CLI_SOURCES})

# Use 'dam' as the output name
set_target_properties(dam-cli PROPERTIES OUTPUT_NAME dam)

# Core library dependency
target_link_libraries(dam-cli PRIVATE dam)

# LLM feature dependencies
if(DAM_ENABLE_LLM)
    target_link_libraries(dam-cli PRIVATE
        nlohmann_json::nlohmann_json
        ${DAM_CURL_TARGET}
    )
    target_compile_definitions(dam-cli PRIVATE DAM_ENABLE_LLM=1)

    # Include directories for local headers
    target_include_directories(dam-cli PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()

# Install target
install(TARGETS dam-cli
    RUNTIME DESTINATION bin
)
