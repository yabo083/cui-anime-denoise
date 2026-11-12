include(CMakeFindDependencyMacro)
find_dependency(Threads)
include("${CMAKE_CURRENT_LIST_DIR}/cuiTargets.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/CuiApp.cmake")
