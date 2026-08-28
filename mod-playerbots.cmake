# mod-playerbots build extras.
#
# The slow (LLM) strategic layer talks HTTP to an Ollama-compatible endpoint and
# parses JSON, so the module needs libcurl and nlohmann/json.
if(TARGET modules)
  find_package(CURL QUIET)
  if(TARGET CURL::libcurl)
    target_link_libraries(modules PRIVATE CURL::libcurl)
    message(STATUS "[mod-playerbots] Using CURL::libcurl")
  elseif(CURL_FOUND)
    target_include_directories(modules PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(modules PRIVATE ${CURL_LIBRARIES})
    message(STATUS "[mod-playerbots] Using libcurl at ${CURL_LIBRARIES}")
  else()
    target_link_libraries(modules PRIVATE curl)
    message(STATUS "[mod-playerbots] libcurl not found by find_package; linking -lcurl")
  endif()

  # nlohmann/json. Until now this module compiled only because a sibling module
  # (mod-ollama-chat) happened to put its bundled copy on the shared `modules`
  # include path - build without that module present and mod-playerbots failed.
  # Prefer a real package, fall back to the copy bundled here.
  find_package(nlohmann_json CONFIG QUIET)
  if(nlohmann_json_FOUND)
    target_link_libraries(modules PRIVATE nlohmann_json::nlohmann_json)
    message(STATUS "[mod-playerbots] Using system nlohmann/json")
  elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/deps/nlohmann/json.hpp")
    target_include_directories(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/deps)
    message(STATUS "[mod-playerbots] Using bundled nlohmann/json")
  else()
    message(FATAL_ERROR "[mod-playerbots] nlohmann/json not found and deps/nlohmann/json.hpp is missing")
  endif()
endif()
