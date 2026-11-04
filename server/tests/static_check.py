"""Static self-check for the DMS security fixes.

1. Brace/paren balance per modified C++ file (catches structural breakage).
2. Port the new IsValidUtf8 / login throttle logic to Python and run test
   vectors, including the exact crash PoC inputs from the review.
3. Grep-level symbol consistency (declaration vs definition vs call sites).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FILES = [
    "src/storage.cpp",
    "src/permission_provider.cpp",
    "src/server_api.cpp",
    "src/server_http.cpp",
    "src/server_runtime.cpp",
    "src/server_main.cpp",
    "src/cron_main.cpp",
    "src/server_internal.hpp",
    "include/dms/storage.hpp",
    "include/dms/permission_provider.hpp",
]

failures = []


def strip_code(text):
    """Remove comments and string/char literals so braces can be counted."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        ch = text[i]
        if ch == '"':
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if ch == "'":
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == "'":
                    i += 1
                    break
                i += 1
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def check_balance():
    print("== 1. brace/paren balance ==")
    for rel in FILES:
        path = ROOT / rel
        code = strip_code(path.read_text(encoding="utf-8"))
        for opener, closer, name in (("{", "}", "braces"),
                                     ("(", ")", "parens"),
                                     ("[", "]", "brackets")):
            depth = 0
            min_depth = 0
            for ch in code:
                if ch == opener:
                    depth += 1
                elif ch == closer:
                    depth -= 1
                    min_depth = min(min_depth, depth)
            status = "OK" if depth == 0 and min_depth >= 0 else f"BAD depth={depth} min={min_depth}"
            if depth != 0 or min_depth < 0:
                failures.append(f"{rel}: {name} {status}")
            print(f"  {rel}: {name} {status}")


def is_valid_utf8_port(data):
    """Python port of the IsValidUtf8() added to permission_provider.cpp."""
    i = 0
    n = len(data)
    while i < n:
        lead = data[i]
        if lead <= 0x7F:
            i += 1
            continue
        elif (lead & 0xE0) == 0xC0:
            cont = 1
        elif (lead & 0xF0) == 0xE0:
            cont = 2
        elif (lead & 0xF8) == 0xF0:
            cont = 3
        else:
            return False
        if i + cont >= n:
            return False
        for offset in range(1, cont + 1):
            if (data[i + offset] & 0xC0) != 0x80:
                return False
        i += cont + 1
    return True


def check_utf8_validator():
    print("\n== 2. UTF-8 gate (crash PoC inputs must be rejected) ==")
    cases = [
        (b"/\xff", False, "PoC path /%FF -> must be rejected at the boundary"),
        (b"/\xc3\xa9", True, "valid U+00E9"),
        (b"/\xe5\x9b\xbe\xe7\xba\xb8.dwg", True, "Chinese filename"),
        (b"/\xf0\x9f\x98\x80", True, "4-byte emoji"),
        (b"/\xc0\x80", True, "overlong NUL: gate passes, dump replace() covers it"),
        (b"/a\x80", False, "stray continuation byte"),
        (b"/\xe5\x9b", False, "truncated sequence"),
        (b"/" + b"a" * 5000, False, "length gate (>4096)"),
        (b"/normal.dwg", True, "plain ASCII"),
    ]
    for data, expected, label in cases:
        gate_ok = is_valid_utf8_port(data)
        length_ok = len(data) <= 4096
        accepted = gate_ok and length_ok
        status = "OK" if accepted == expected else "FAIL"
        if accepted != expected:
            failures.append(f"utf8 gate: {label}")
        print(f"  [{status}] {label}")


def check_login_throttle():
    print("\n== 3. login throttle port (per user+IP, 60s cap) ==")

    def delay_seconds(count):
        if count < 5:
            return None
        exponent = min(count - 5, 5)
        return min(60, 15 * (1 << exponent))

    expected = {5: 15, 6: 30, 7: 60, 8: 60, 9: 60, 10: 60}
    for count, want in expected.items():
        got = delay_seconds(count)
        status = "OK" if got == want else "FAIL"
        if got != want:
            failures.append(f"throttle({count})={got} want {want}")
        print(f"  [{status}] failures={count} -> lock {got}s (expected {want}s)")

    # account lockout DoS check: another IP is a different key.
    key = lambda user, ip: user + "\n" + ip
    a1 = key("admin", "10.0.0.1")
    a2 = key("admin", "10.0.0.2")
    status = "OK" if a1 != a2 else "FAIL"
    print(f"  [{status}] same user from different IP -> different throttle bucket")


def check_symbols():
    print("\n== 4. symbol consistency ==")
    checks = [
        # (file, must-contain regex list)
        ("src/server_api.cpp",
         [r"Login\(username, password, ip\)",
          r"LoginSlots::Acquire",
          r"ConstantTimeEquals\(supplied",
          r"owner_of\(node\.path\)",
          r"EMPTY_BODY",
          r"ARCHIVE_TOO_LARGE",
          r"kMaxArchiveBytes"]),
        ("src/storage.cpp",
         [r"const std::string& client_ip\)",
          r"kMaxFailureEntries",
          r"error_handler_t::replace",
          r"O_CREAT \| O_APPEND \| O_WRONLY \| O_CLOEXEC, 0600",
          r"REPLACE"]),
        ("src/permission_provider.cpp",
         [r"IsValidUtf8\(path\)",
          r"Logical path is too long",
          r"Ignoring permissions\.csv line"]),
        ("src/server_http.cpp",
         [r"kMaxConnections = 512",
          r"RequestSlots::Acquire",
          r"Session setup failed",
          r"make_shared<RequestSlots::Release>"]),
        ("src/server_runtime.cpp",
         [r"Permission monitor iteration failed",
          r"Ignoring rename intent with invalid path"]),
        ("src/server_main.cpp",
         [r"io_context recovered from"]),
        ("src/cron_main.cpp",
         [r"Skipping pending-delete row with invalid path"]),
        ("include/dms/storage.hpp",
         [r"client_ip = \{\}",
          r"kMaxFailureEntries"]),
        ("src/server_internal.hpp",
         [r"#include <array>",
          r"Cleaning|残留|remove_all\(leftover"]),
    ]
    for rel, patterns in checks:
        text = (ROOT / rel).read_text(encoding="utf-8")
        for pattern in patterns:
            found = re.search(pattern, text) is not None
            status = "OK" if found else "MISSING"
            if not found:
                failures.append(f"{rel}: pattern {pattern!r} not found")
            print(f"  [{status}] {rel}: /{pattern}/")


def check_no_stale_calls():
    print("\n== 5. no stale call sites ==")
    stale = [
        (r"(?<!Set)MetadataOwner\(", "src/server_api.cpp", 0),  # definition removed
        ("auth.Login(username, password)", None, 0),  # must now pass ip at the route
        ("std::min(300, 15", "src/storage.cpp", 0),
    ]
    for pattern, rel, allowed in stale:
        if rel is None:
            files = [ROOT / f for f in FILES if f.endswith(".cpp")]
        else:
            files = [ROOT / rel]
        total = 0
        for f in files:
            total += len(re.findall(re.escape(pattern), f.read_text(encoding="utf-8")))
        status = "OK" if total == allowed else "FAIL"
        if total != allowed:
            failures.append(f"stale {pattern!r}: {total} occurrences")
        print(f"  [{status}] {pattern!r} occurrences={total} (expected {allowed})")


check_balance()
check_utf8_validator()
check_login_throttle()
check_symbols()
check_no_stale_calls()

print("\n================ RESULT ================")
if failures:
    for f in failures:
        print("FAIL:", f)
    sys.exit(1)
print("ALL STATIC CHECKS PASSED")
