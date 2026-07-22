file(GLOB_RECURSE sources_spells_qol "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

set(ModuleSources
  ${ModuleSources}
  ${sources_spells_qol}
  PARENT_SCOPE
)

AC_ADD_CONFIG_FILE("${CMAKE_CURRENT_SOURCE_DIR}/conf/spells_qol.conf.dist")
