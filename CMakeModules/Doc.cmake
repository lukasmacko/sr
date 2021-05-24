# Prepare building doxygen documentation
macro(SYSREPO_DOC)
    find_package(Doxygen)
    if(DOXYGEN_FOUND)
        find_program(DOT_PATH dot PATH_SUFFIXES graphviz2.38/bin graphviz/bin)
        if(DOT_PATH)
            set(HAVE_DOT "YES")
        else()
            set(HAVE_DOT "NO")
            message(AUTHOR_WARNING "Doxygen: to generate UML diagrams please install graphviz")
        endif()

        if(GEN_LANGUAGE_BINDINGS AND GEN_CPP_BINDINGS)
            set(CPP_HEADERS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/bindings/cpp/src)
        endif()

        add_custom_target(doc
                COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_BINARY_DIR}/Doxyfile
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        configure_file(Doxyfile.in Doxyfile)
    endif()
endmacro()
