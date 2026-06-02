set(Python3_FIND_IMPLEMENTATIONS CPython)
set(Python3_FIND_STRATEGY LOCATION)
if(NOT Python3_EXECUTABLE)
    find_program(_CONFLUX_DEFAULT_PYTHON3
        NAMES python3
        PATHS /usr/bin /usr/local/bin /bin
        NO_DEFAULT_PATH)
    if(_CONFLUX_DEFAULT_PYTHON3)
        set(Python3_EXECUTABLE "${_CONFLUX_DEFAULT_PYTHON3}" CACHE FILEPATH
            "Python 3 interpreter used by Conflux configure-time scripts")
    endif()
endif()
