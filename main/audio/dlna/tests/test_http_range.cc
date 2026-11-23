#include "dlna_http_range.h"

#include <cassert>
#include <cstddef>
#include <string>

using DlnaHttpRange::Parse;
using DlnaHttpRange::Result;

int main() {
    size_t first = 0;
    size_t last = 0;

    assert(Parse("bytes=0-99", 100, first, last) == Result::kValid);
    assert(first == 0 && last == 99);

    assert(Parse("bytes=10-19", 100, first, last) == Result::kValid);
    assert(first == 10 && last == 19);

    assert(Parse("bytes=90-", 100, first, last) == Result::kValid);
    assert(first == 90 && last == 99);

    assert(Parse("bytes=-10", 100, first, last) == Result::kValid);
    assert(first == 90 && last == 99);

    assert(Parse("bytes=-200", 100, first, last) == Result::kValid);
    assert(first == 0 && last == 99);

    // An end offset beyond EOF is clipped to the final byte.
    assert(Parse("bytes=90-200", 100, first, last) == Result::kValid);
    assert(first == 90 && last == 99);

    assert(Parse("bytes=100-", 100, first, last) == Result::kUnsatisfiable);
    assert(Parse("bytes=100-120", 100, first, last) == Result::kUnsatisfiable);
    assert(Parse("bytes=-10", 0, first, last) == Result::kUnsatisfiable);

    // Unsupported units, multipart requests and malformed syntax are ignored.
    assert(Parse("items=0-1", 100, first, last) == Result::kIgnore);
    assert(Parse("bytes=0-1,4-5", 100, first, last) == Result::kIgnore);
    assert(Parse("bytes=abc-def", 100, first, last) == Result::kIgnore);
    assert(Parse("bytes=10-9", 100, first, last) == Result::kIgnore);
    assert(Parse("bytes=1-2-3", 100, first, last) == Result::kIgnore);
    assert(Parse("bytes=", 100, first, last) == Result::kIgnore);

    return 0;
}
