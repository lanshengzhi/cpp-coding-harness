#include <boost/throw_exception.hpp>

#include <exception>

namespace boost {

#if defined(BOOST_NO_EXCEPTIONS)

// BOOST_NORETURN matches the attribute kind of Boost's first declaration
// (GNU attribute on Clang), which [[noreturn]] does not.
BOOST_NORETURN void throw_exception(const std::exception&) { std::terminate(); }

BOOST_NORETURN void throw_exception(const std::exception&, const boost::source_location&) { std::terminate(); }

#endif

} // namespace boost
