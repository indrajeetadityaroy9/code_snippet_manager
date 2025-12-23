# Dependencies for LLM integration
#
# This module provides libcurl and nlohmann_json for the interactive
# LLM-assisted autocomplete feature.

include(FetchContent)

# nlohmann_json for JSON parsing
message(STATUS "Fetching nlohmann_json...")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# libcurl for HTTP requests
# Prefer system installation for better compatibility
find_package(CURL QUIET)
if(CURL_FOUND)
    message(STATUS "Using system libcurl: ${CURL_VERSION_STRING}")
    set(DAM_CURL_TARGET CURL::libcurl)
else()
    message(STATUS "System libcurl not found, fetching from source...")
    FetchContent_Declare(
        curl
        URL https://curl.se/download/curl-8.5.0.tar.gz
        URL_HASH SHA256=05fc17ff25b793a437a0906e0484b82172a9f4de02be5c285c16b48a91479656
    )
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(CURL_DISABLE_TESTS ON CACHE BOOL "" FORCE)
    set(HTTP_ONLY ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(curl)
    set(DAM_CURL_TARGET libcurl)
endif()

# Export variables for use in other CMakeLists.txt
set(DAM_CURL_TARGET ${DAM_CURL_TARGET} PARENT_SCOPE)
