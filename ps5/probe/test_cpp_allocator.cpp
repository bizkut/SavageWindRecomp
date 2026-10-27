// Throwing title operator new must invoke its handler and propagate failure.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cstddef>
#include <limits>
#include <new>

static bool called;
static void failed() { called = true; throw std::bad_alloc(); }
int main() {
    std::set_new_handler(failed);
    try {
        void *p = ::operator new(std::numeric_limits<std::size_t>::max());
        ::operator delete(p);
        assert(false && "impossibly large allocation unexpectedly succeeded");
    } catch (const std::bad_alloc &) {
        assert(called);
    }
    std::set_new_handler(nullptr);
    bool caught = false;
    try { (void)::operator new(std::numeric_limits<std::size_t>::max()); }
    catch (const std::bad_alloc &) { caught = true; }
    assert(caught);
    return 0;
}
