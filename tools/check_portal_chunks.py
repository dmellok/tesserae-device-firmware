#!/usr/bin/env python3
"""Guard against zero-length chunks in the captive portal's chunked responses.

A zero-length chunk is how HTTP chunked transfer encoding signals END OF
RESPONSE. ESP-IDF's httpd_resp_send_chunk() emits "0\\r\\n" + "\\r\\n" verbatim
for buf_len == 0, so httpd_resp_sendstr_chunk(req, "") ends the page: every
later chunk is written to a socket the browser has stopped reading.

This shipped once. The Local/Remote switch splices a conditional ` checked`
between static chunks, and the unselected branch passed "" -- which truncated
the portal at the Local label, taking the Remote segment, both server cards and
the Save button with it. The page looked structurally valid in source and in a
naive string-concatenation test, because concatenation cannot model a chunk
boundary. Only the chunk framing was wrong.

So: any conditional fragment must go through send_chunk(), which drops empty
strings instead of sending them. This flags raw httpd_resp_sendstr_chunk() calls
whose argument is a ternary or a literal "".

Run: python3 tools/check_portal_chunks.py
"""
import pathlib
import re
import sys

SRC = pathlib.Path(__file__).resolve().parent.parent / "src" / "provisioning.c"

# Real calls are one per line, so a line-oriented scan is enough -- and avoids
# hand-rolling a C lexer, which desyncs on this file's HTML string literals.
CALL = re.compile(r"httpd_resp_sendstr_chunk\s*\(\s*[A-Za-z_]\w*\s*,\s*(.+?)\)\s*;")


def code_lines(text: str):
    """Yield (lineno, code) for lines outside comments.

    Tracks /* */ across lines and strips //. Without this the checker flags the
    send_chunk() docs, which quote the very call they warn about.
    """
    in_block = False
    for n, raw in enumerate(text.splitlines(), 1):
        line, out = raw, []
        while line:
            if in_block:
                end = line.find("*/")
                if end < 0:
                    line = ""
                else:
                    line, in_block = line[end + 2:], False
            else:
                start = line.find("/*")
                slash = line.find("//")
                if slash >= 0 and (start < 0 or slash < start):
                    out.append(line[:slash])
                    line = ""
                elif start >= 0:
                    out.append(line[:start])
                    line, in_block = line[start + 2:], True
                else:
                    out.append(line)
                    line = ""
        yield n, "".join(out)


def main() -> int:
    bad = []
    for lineno, code in code_lines(SRC.read_text()):
        m = CALL.search(code)
        if not m:
            continue
        arg = " ".join(m.group(1).split())
        if arg == "NULL":          # the deliberate stream terminator
            continue
        if "?" in arg or '""' in arg:
            bad.append((lineno, arg))

    if bad:
        print("Zero-length-chunk hazard: a chunk that can be empty TERMINATES the")
        print("response, silently truncating the page. Use send_chunk() instead.\n")
        for lineno, arg in bad:
            print(f"  {SRC.name}:{lineno}: httpd_resp_sendstr_chunk(req, {arg});")
        return 1

    print(f"portal chunks OK: no conditional or empty raw chunk sends in {SRC.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
