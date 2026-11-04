// Host coverage for PS5 control authentication.
#include "../runtime/ps5/control_auth.h"
#include <assert.h>
#include <string.h>

int main(void) {
    const char token[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char line[5 + CONTROL_TOKEN_HEX + 1];
    memcpy(line, "auth ", 5);
    memcpy(line + 5, token, CONTROL_TOKEN_HEX + 1);
    ControlAuth good = {100, false};
    assert(control_auth_line(&good, line, strlen(line), token, 101));
    assert(good.authenticated);
    assert(!control_auth_expired(&good, CONTROL_AUTH_TIMEOUT_NS + 100));
    ControlAuth bad = {100, false};
    line[30] ^= 1;
    assert(!control_auth_line(&bad, line, strlen(line), token, 101));
    assert(!bad.authenticated);
    line[30] ^= 1;
    assert(!control_auth_line(&bad, "state", 5, token, 101));
    assert(!control_auth_expired(&bad, CONTROL_AUTH_TIMEOUT_NS + 99));
    assert(control_auth_expired(&bad, CONTROL_AUTH_TIMEOUT_NS + 100));
    assert(!control_auth_line(&bad, line, strlen(line), token, CONTROL_AUTH_TIMEOUT_NS + 100));
    return 0;
}
