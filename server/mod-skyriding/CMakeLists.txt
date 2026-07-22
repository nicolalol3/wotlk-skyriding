file(GLOB_RECURSE sources_mod_skyriding "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

set(ModuleSources
  ${ModuleSources}
  ${sources_mod_skyriding}
  PARENT_SCOPE
)

AC_ADD_CONFIG_FILE("${CMAKE_CURRENT_SOURCE_DIR}/conf/skyriding.conf.dist")
