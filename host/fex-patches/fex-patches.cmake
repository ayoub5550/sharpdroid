# the FEX fixes this repository carries, applied to copies in the build tree.
#
# **the FEX checkout is still never modified.** every `*.patch` beside this file is a git diff against
# the pinned FEX, rooted at the FEX checkout (`a/FEXCore/Source/...`). at configure time the files a
# patch touches are copied out of the checkout into `<build>/fex-patched/`, the patches are applied
# there, and the FEXCore target is pointed at the copies instead of the originals. the submodule stays
# clean, `git describe --dirty` keeps meaning what it says, and the whole delta is a directory anyone
# can read in one sitting.
#
# **a patch that no longer applies stops the configure.** that is the moment a FEX bump has either
# fixed the bug or moved the code, and both want a person: delete the patch, or refresh it. a patch
# silently skipped would be a fix silently lost.
#
# only `.cpp` files that a FEXCore target compiles can be carried this way. a patched header would need
# the include path rearranged ahead of FEX's own, which is a different and riskier thing, so it refuses
# rather than half-working.

set(FEX_PATCHES_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(FEX_PATCHES_STAGE_DIR "${CMAKE_BINARY_DIR}/fex-patched")

# fills FEX_PATCHES (the patch files, in order) and FEX_PATCHED_FILES (FEX-relative paths of what they
# touch) in the caller's scope, and leaves patched copies of those files under FEX_PATCHES_STAGE_DIR.
function(fex_patches_stage)
  file(GLOB patches CONFIGURE_DEPENDS "${FEX_PATCHES_SOURCE_DIR}/*.patch")
  list(SORT patches)
  file(REMOVE_RECURSE "${FEX_PATCHES_STAGE_DIR}")
  set(touched "")
  if (patches AND NOT GIT_FOUND)
    message(FATAL_ERROR "host/fex-patches needs git to apply its patches, and none was found")
  endif()

  foreach(patch IN LISTS patches)
    get_filename_component(name "${patch}" NAME)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} apply --numstat "${patch}"
      OUTPUT_VARIABLE numstat
      RESULT_VARIABLE rc
      ERROR_VARIABLE err)
    if (NOT rc EQUAL 0)
      message(FATAL_ERROR "host/fex-patches/${name} is not a patch git can read:\n${err}")
    endif()
    string(REGEX MATCHALL "[^\n]+" lines "${numstat}")
    foreach(line IN LISTS lines)
      string(REGEX REPLACE "^[0-9-]+\t[0-9-]+\t" "" path "${line}")
      if (NOT path IN_LIST touched)
        list(APPEND touched "${path}")
        if (NOT EXISTS "${FEX_ROOT}/${path}")
          message(FATAL_ERROR "host/fex-patches/${name} patches ${path}, which this FEX does not have")
        endif()
        get_filename_component(dir "${FEX_PATCHES_STAGE_DIR}/${path}" DIRECTORY)
        file(COPY "${FEX_ROOT}/${path}" DESTINATION "${dir}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FEX_ROOT}/${path}")
      endif()
    endforeach()
  endforeach()

  # applied one at a time, so a failure names the patch rather than the set. GIT_CEILING_DIRECTORIES
  # keeps git from discovering whichever repository the build tree happens to sit inside -- this one,
  # usually -- and resolving the patch's paths against that instead of the staging directory.
  foreach(patch IN LISTS patches)
    get_filename_component(name "${patch}" NAME)
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E env "GIT_CEILING_DIRECTORIES=${CMAKE_BINARY_DIR}"
              ${GIT_EXECUTABLE} apply --whitespace=nowarn "${patch}"
      WORKING_DIRECTORY "${FEX_PATCHES_STAGE_DIR}"
      RESULT_VARIABLE rc
      ERROR_VARIABLE err)
    if (NOT rc EQUAL 0)
      message(FATAL_ERROR
        "host/fex-patches/${name} no longer applies to FEX ${GIT_DESCRIBE_STRING}:\n${err}\n"
        "if this FEX fixed what it fixes, delete the patch. otherwise refresh it against the new pin.")
    endif()
    message(STATUS "FEX patch: ${name}")
  endforeach()

  set(FEX_PATCHES "${patches}" PARENT_SCOPE)
  set(FEX_PATCHED_FILES "${touched}" PARENT_SCOPE)
endfunction()

# points the FEXCore targets at the patched copies. called after add_subdirectory(FEXCore).
function(fex_patches_redirect)
  foreach(path IN LISTS FEX_PATCHED_FILES)
    if (NOT path MATCHES "^FEXCore/Source/(.+\\.cpp)$")
      message(FATAL_ERROR "host/fex-patches touches ${path}: only FEXCore/Source .cpp files can be carried")
    endif()
    set(rel "${CMAKE_MATCH_1}")
    set(found FALSE)
    foreach(target FEXCore_object FEXCore_Base)
      get_target_property(sources ${target} SOURCES)
      list(FIND sources "${rel}" index)
      if (index EQUAL -1)
        list(FIND sources "${FEX_ROOT}/${path}" index)
      endif()
      if (NOT index EQUAL -1)
        list(REMOVE_AT sources ${index})
        list(INSERT sources ${index} "${FEX_PATCHES_STAGE_DIR}/${path}")
        set_property(TARGET ${target} PROPERTY SOURCES "${sources}")
        set(found TRUE)
      endif()
    endforeach()
    if (NOT found)
      message(FATAL_ERROR "host/fex-patches touches ${path}, which no FEXCore target compiles")
    endif()
  endforeach()
endfunction()
