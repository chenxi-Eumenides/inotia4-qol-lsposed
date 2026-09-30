set(contract_files
    "${CPP_DIR}/contracts/item_facts.h"
    "${CPP_DIR}/contracts/game_ports/item_port.h"
    "${CPP_DIR}/contracts/inventory_extension.h"
    "${CPP_DIR}/contracts/extension_bag.h"
    "${CPP_DIR}/contracts/attribute_quality.h"
    "${CPP_DIR}/contracts/inventory_action.h"
)

foreach(path IN LISTS contract_files)
    file(READ "${path}" source)
    if(source MATCHES "void[ \t]*\\*[ \t]*native_item" OR
       source MATCHES "(const[ \t]+)?void[ \t]*\\*[ \t]*item[ \t]*[,)]" OR
       source MATCHES "void[ \t]*\\*[ \t]*\\*[ \t]*out_item")
        message(FATAL_ERROR "raw item pointer crossed a handle-only contract: ${path}")
    endif()
endforeach()

file(READ "${CPP_DIR}/contracts/item_handle.h" item_handle_api)
if(item_handle_api MATCHES "resolve_item" OR
   item_handle_api MATCHES "void[ \t]*\\*[ \t]*\\*[ \t]*out_item")
    message(FATAL_ERROR "Host SDK item handle API exposes object resolution")
endif()
