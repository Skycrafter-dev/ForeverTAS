if(NOT DEFINED FOREVERTAS_SOURCE_DIR)
    message(FATAL_ERROR "FOREVERTAS_SOURCE_DIR is required")
endif()

set(editor_dir "${FOREVERTAS_SOURCE_DIR}/assets/blockly")
set(blockly_dir "${FOREVERTAS_SOURCE_DIR}/third_party/blockly")

foreach(required IN ITEMS
        "${editor_dir}/index.html"
        "${editor_dir}/editor.css"
        "${editor_dir}/editor.js"
        "${blockly_dir}/blockly_compressed.js"
        "${blockly_dir}/en.js")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Offline Blockly resource is missing: ${required}")
    endif()
endforeach()

file(READ "${editor_dir}/index.html" html)
if(NOT html MATCHES "connect-src 'none'" OR
   NOT html MATCHES "object-src 'none'" OR
   NOT html MATCHES "frame-src 'none'")
    message(FATAL_ERROR "Blockly editor CSP does not enforce offline isolation")
endif()

foreach(source IN ITEMS
        "${editor_dir}/index.html"
        "${editor_dir}/editor.css"
        "${editor_dir}/editor.js"
        "${FOREVERTAS_SOURCE_DIR}/qml/blocks/BlocklyWorkspace.qml")
    file(READ "${source}" contents)
    if(contents MATCHES "https?://" OR contents MATCHES "//cdn\\.")
        message(FATAL_ERROR "Blockly production path references the network: ${source}")
    endif()
endforeach()

file(READ "${FOREVERTAS_SOURCE_DIR}/qml/blocks/BlocklyWorkspace.qml" host)
if(NOT host MATCHES "qrc:///blockly/assets/blockly/index.html" OR
   NOT host MATCHES "request.reject\\(\\)")
    message(FATAL_ERROR "Blockly WebEngine host does not reject external navigation")
endif()

file(READ "${FOREVERTAS_SOURCE_DIR}/CMakeLists.txt" cmake_source)
foreach(resource IN ITEMS
        "assets/blockly/index.html"
        "assets/blockly/editor.css"
        "assets/blockly/editor.js"
        "third_party/blockly/blockly_compressed.js"
        "third_party/blockly/en.js")
    if(NOT cmake_source MATCHES "${resource}")
        message(FATAL_ERROR "Blockly resource is not embedded: ${resource}")
    endif()
endforeach()
