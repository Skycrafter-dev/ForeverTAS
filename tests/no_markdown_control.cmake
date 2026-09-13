if(NOT DEFINED FOREVERTAS_SOURCE_DIR)
    message(FATAL_ERROR "FOREVERTAS_SOURCE_DIR is required")
endif()

set(markdown_files)
file(GLOB top_level_markdown LIST_DIRECTORIES FALSE
    "${FOREVERTAS_SOURCE_DIR}/*.md"
    "${FOREVERTAS_SOURCE_DIR}/*.MD"
    "${FOREVERTAS_SOURCE_DIR}/*.markdown"
    "${FOREVERTAS_SOURCE_DIR}/*.MARKDOWN")
list(APPEND markdown_files ${top_level_markdown})

foreach(directory IN ITEMS assets cmake docs packaging qml shaders specs src tests third_party)
    file(GLOB_RECURSE directory_markdown LIST_DIRECTORIES FALSE
        "${FOREVERTAS_SOURCE_DIR}/${directory}/*.md"
        "${FOREVERTAS_SOURCE_DIR}/${directory}/*.MD"
        "${FOREVERTAS_SOURCE_DIR}/${directory}/*.markdown"
        "${FOREVERTAS_SOURCE_DIR}/${directory}/*.MARKDOWN")
    list(APPEND markdown_files ${directory_markdown})
endforeach()

if(markdown_files)
    list(REMOVE_DUPLICATES markdown_files)
    list(SORT markdown_files)
    list(JOIN markdown_files "\n" markdown_list)
    message(FATAL_ERROR "Markdown files are not allowed in the source tree:\n${markdown_list}")
endif()
