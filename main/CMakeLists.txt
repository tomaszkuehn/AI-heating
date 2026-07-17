# main/ component: application code + embedded web assets.
#
# Web assets (index.html, style.css, app.js) are gzip-compressed at build time and
# served with "Content-Encoding: gzip" -- the browser decompresses them. This
# reclaims ~38 KB of the 1 MB factory partition the firmware had otherwise outgrown
# (was 816 B free), with no feature loss: every browser handles gzip transparently.
#
# ESP-IDF evaluates this file twice: once in an "early-expansion" script pass
# (CMAKE_BUILD_EARLY_EXPANSION=1) that only extracts idf_component_register's
# requirements, and once in the real build pass. add_custom_command is illegal in
# the script pass, so the .gz generation is guarded to run only in the real pass.
# The _gz_outputs LIST is computed unconditionally so EMBED_FILES is populated in
# both passes (the script pass discards it; only the real pass embeds).

set(_web_assets
    "${CMAKE_CURRENT_SOURCE_DIR}/../web/index.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/../web/style.css"
    "${CMAKE_CURRENT_SOURCE_DIR}/../web/app.js"
)
set(_gz_outputs)
foreach(_src ${_web_assets})
    get_filename_component(_name ${_src} NAME)
    list(APPEND _gz_outputs "${CMAKE_CURRENT_BINARY_DIR}/${_name}.gz")
endforeach()

idf_component_register(
    SRCS
        "main.c"
        "data_model.c"
        "sensor_manager.c"
        "simulation_manager.c"
        "control_engine.c"
        "heating_output.c"
        "fault_manager.c"
        "storage_manager.c"
        "network_manager.c"
        "web_ui_api.c"
        "notification_manager.c"
        "profile.c"
    INCLUDE_DIRS
        "."
    EMBED_FILES
        ${_gz_outputs}
    REQUIRES
        nvs_flash
        joltwallet__littlefs
        esp_wifi
        esp_http_server
        esp_event
        esp_netif
        esp_timer
        lwip
        driver
        mdns
        mbedtls
    )

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    find_program(GZIP_TOOL gzip)
    if(NOT GZIP_TOOL)
        # Native-Windows fallback when gzip isn't on PATH (the WSL build always has it).
        find_package(Python3 COMPONENTS Interpreter)
    endif()

    foreach(_src ${_web_assets})
        get_filename_component(_name ${_src} NAME)
        set(_gz "${CMAKE_CURRENT_BINARY_DIR}/${_name}.gz")

        if(GZIP_TOOL)
            # Copy the source into the build dir, then gzip it in place (no shell
            # redirect). -n strips the mtime so the output is reproducible across
            # builds. -f forces overwrite of an existing .gz from a prior build, so
            # editing a web file -> regenerates its .gz -> re-embeds only that file
            # (no fullclean), via the OUTPUT->EMBED_FILES dependency chain. Without
            # -f, an incremental web edit fails with "X.gz already exists; not
            # overwritten" because the stale .gz is still on disk.
            add_custom_command(
                OUTPUT ${_gz}
                COMMAND ${CMAKE_COMMAND} -E copy ${_src} ${CMAKE_CURRENT_BINARY_DIR}/${_name}
                COMMAND ${GZIP_TOOL} -9 -n -f ${_name}
                WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                DEPENDS ${_src}
                VERBATIM
                COMMENT "gzip ${_name}"
            )
        else()
            add_custom_command(
                OUTPUT ${_gz}
                COMMAND ${Python3_EXECUTABLE} -c
                    "import gzip,shutil; shutil.copy('${_src}','${CMAKE_CURRENT_BINARY_DIR}/${_name}'); f=gzip.GzipFile('${_gz}','wb',mtime=0); f.write(open('${CMAKE_CURRENT_BINARY_DIR}/${_name}','rb').read()); f.close()"
                DEPENDS ${_src}
                VERBATIM
                COMMENT "gzip ${_name} (python)"
            )
        endif()
    endforeach()
endif()