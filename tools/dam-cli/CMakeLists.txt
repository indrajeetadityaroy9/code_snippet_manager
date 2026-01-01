# DAM CLI tool

set(DAM_CLI_SOURCES
    main.cpp
    commands/add_command.cpp
    commands/get_command.cpp
    commands/edit_command.cpp
    commands/list_command.cpp
    commands/rm_command.cpp
    commands/tag_command.cpp
    commands/search_command.cpp
)

# Add interactive editor sources if LLM is enabled
if(DAM_ENABLE_LLM)
    list(APPEND DAM_CLI_SOURCES
        interactive/terminal.cpp
        interactive/interactive_editor.cpp
        interactive/model_picker.cpp
        llm/input_classifier.cpp
    )
endif()

add_executable(dam-cli ${DAM_CLI_SOURCES})

# Use 'dam' as the output name
set_target_properties(dam-cli PROPERTIES OUTPUT_NAME dam)

# Include directories for local headers
target_include_directories(dam-cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Core library dependency (includes LLM and search features)
target_link_libraries(dam-cli PRIVATE dam CLI11::CLI11)

# LLM feature dependencies
if(DAM_ENABLE_LLM)
    target_compile_definitions(dam-cli PRIVATE DAM_ENABLE_LLM=1)
endif()

# Install target
install(TARGETS dam-cli
    RUNTIME DESTINATION bin
)
