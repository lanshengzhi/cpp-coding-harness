# CTest include script for the de-pi test classification audit (#625).
#
# Catch2's ADD_TAGS_AS_LABELS option carries explicit [compat-pi] and [diverge]
# tags from a test case into CTest. Every other test is product specification
# by default. This script runs after the Catch discovery include files, so it
# can append the default without replacing the existing module and issue tags.
# A diverging test must also name its owning migration ticket in an [issueNNN]
# tag; the check below keeps that requirement machine-verifiable.

get_property(_cch_classification_tests DIRECTORY PROPERTY TESTS)
foreach(_cch_test IN LISTS _cch_classification_tests)
    get_property(_cch_labels TEST "${_cch_test}" PROPERTY LABELS)

    set(_cch_has_compat_pi FALSE)
    set(_cch_has_diverge FALSE)
    list(FIND _cch_labels "compat-pi" _cch_compat_index)
    list(FIND _cch_labels "diverge" _cch_diverge_index)
    if(NOT _cch_compat_index EQUAL -1)
        set(_cch_has_compat_pi TRUE)
    endif()
    if(NOT _cch_diverge_index EQUAL -1)
        set(_cch_has_diverge TRUE)
    endif()

    set(_cch_category_count 0)
    if(_cch_has_compat_pi)
        math(EXPR _cch_category_count "${_cch_category_count} + 1")
    endif()
    if(_cch_has_diverge)
        math(EXPR _cch_category_count "${_cch_category_count} + 1")
    endif()
    if(_cch_category_count GREATER 1)
        message(FATAL_ERROR
            "CTest test '${_cch_test}' has incompatible de-pi categories: ${_cch_labels}")
    endif()

    if(_cch_has_diverge)
        set(_cch_has_owner_ticket FALSE)
        foreach(_cch_ticket IN ITEMS 622 623 624 626)
            list(FIND _cch_labels "issue${_cch_ticket}" _cch_ticket_index)
            if(NOT _cch_ticket_index EQUAL -1)
                set(_cch_has_owner_ticket TRUE)
            endif()
        endforeach()
        if(NOT _cch_has_owner_ticket)
            message(FATAL_ERROR
                "Diverging CTest test '${_cch_test}' must carry issue622, issue623, issue624, or issue626")
        endif()
    endif()

    if(NOT _cch_has_compat_pi AND NOT _cch_has_diverge)
        list(APPEND _cch_labels spec)
    endif()
    list(REMOVE_DUPLICATES _cch_labels)
    set_tests_properties("${_cch_test}" PROPERTIES LABELS "${_cch_labels}")
endforeach()
