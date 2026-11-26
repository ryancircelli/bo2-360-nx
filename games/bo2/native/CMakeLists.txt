# metal360 native component of the BO2 package (packages/index.toml in metal360 pins this folder's parent).
# metal360's generated app project sets METAL360_GAME_TARGET and adds this folder; the BO2 build root
# (../CMakeLists.txt) then adds the glue to that target instead of making its own executable.
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/.." bo2zm-glue)
