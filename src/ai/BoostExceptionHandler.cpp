#include <boost/throw_exception.hpp>

#include <exception>

namespace boost {

#if defined(BOOST_NO_EXCEPTIONS)

[[noreturn]] void throw_exception(const std::exception&) {
    std::terminate();
}

[[noreturn]] void throw_exception(
    const std::exception&,
    const boost::source_location&) {
    std::terminate();
}

#endif

} // namespace boost
