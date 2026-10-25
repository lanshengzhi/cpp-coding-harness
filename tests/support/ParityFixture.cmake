include_guard(GLOBAL)

# Both real-Gate fixtures configure a private miniature tree rather than the
# repository, and each needs its own generated state so their parallel runs
# cannot race. Since #1070 removed the temporary Session redirects the tree is
# the checked-in template verbatim.
function(cch_prepare_parity_fixture_source destination template_root)
    file(REMOVE_RECURSE "${destination}")
    file(COPY "${template_root}/src" DESTINATION "${destination}")
endfunction()
