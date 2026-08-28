# mod-playerbots build extras.
#
# The slow (LLM) strategic layer talks HTTP to an Ollama-compatible endpoint, so
# the module needs libcurl. Everything else in the module is unchanged.
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
    # Last resort: the plain link name, which is what sibling modules use.
    target_link_libraries(modules PRIVATE curl)
    message(STATUS "[mod-playerbots] libcurl not found by find_package; linking -lcurl")
  endif()
endif()
