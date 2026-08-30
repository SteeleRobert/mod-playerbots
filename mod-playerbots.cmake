# mod-playerbots build extras.
#
# The slow (LLM) strategic layer talks HTTP to an Ollama/OpenAI-compatible
# endpoint and parses JSON, so the module needs libcurl and nlohmann/json.
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

  # nlohmann/json. The dependency predates this module's LLM work - it arrived
  # with the long-term AI scaffold (PlayerbotLongTermAI, FunctionTool) - but it
  # has never been resolved explicitly: the module compiled only because a
  # sibling module happened to put its bundled copy on the shared `modules`
  # include path, which silently depends on that module being enabled and on
  # cmake ordering.
  #
  # Resolve it here instead, preferring a real package, and say plainly which
  # one was used so a broken build is diagnosable from the configure log.
  find_package(nlohmann_json CONFIG QUIET)
  if(nlohmann_json_FOUND)
    target_link_libraries(modules PRIVATE nlohmann_json::nlohmann_json)
    message(STATUS "[mod-playerbots] Using system nlohmann/json")
  elseif(EXISTS "${CMAKE_SOURCE_DIR}/deps/nlohmann/json.hpp")
    target_include_directories(modules PRIVATE ${CMAKE_SOURCE_DIR}/deps)
    message(STATUS "[mod-playerbots] Using AzerothCore deps nlohmann/json")
  elseif(EXISTS "${CMAKE_SOURCE_DIR}/modules/mod-ollama-chat/deps/nlohmann/json.hpp")
    # Referenced explicitly rather than inherited by luck from that module's own
    # cmake, so at least the coupling is visible and order-independent.
    target_include_directories(modules PRIVATE ${CMAKE_SOURCE_DIR}/modules/mod-ollama-chat/deps)
    message(STATUS "[mod-playerbots] Using nlohmann/json bundled by mod-ollama-chat")
  else()
    message(WARNING
      "[mod-playerbots] nlohmann/json not found. Install nlohmann-json, or place "
      "json.hpp at <core>/deps/nlohmann/json.hpp. The long-term AI sources will "
      "fail to compile without it.")
  endif()
endif()
