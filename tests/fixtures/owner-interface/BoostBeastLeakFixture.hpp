#pragma once

// Poison case for the Owner Interface standalone-compile evidence (issue
// #834): the Beast HTTP/TLS types are the wire adapter's private machinery and
// must never appear on an Owner Interface (ADR 0054, ADR 0065). The standalone
// compile provides no third-party include path, so this include cannot resolve.

#include <boost/beast/core.hpp>
