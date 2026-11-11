# PlatformIO pre-build step: gzip web/index.html into include/web_index.h (served at "/").
# Edit the page in web/index.html; this regenerates the header whenever the page is newer.
import gzip
import os

Import("env")  # noqa: F821  (provided by PlatformIO/SCons)

root = env.subst("$PROJECT_DIR")  # noqa: F821
src = os.path.join(root, "web", "index.html")
dst = os.path.join(root, "include", "web_index.h")

if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(src):
    data = gzip.compress(open(src, "rb").read(), compresslevel=9, mtime=0)
    with open(dst, "w") as f:
        f.write("// GENERATED from web/index.html by tools/embed_web.py: do not edit\n#pragma once\n#include <pgmspace.h>\n")
        f.write("const size_t WEB_INDEX_GZ_LEN = %d;\nconst uint8_t WEB_INDEX_GZ[] PROGMEM = {\n" % len(data))
        for i in range(0, len(data), 20):
            f.write("  " + ",".join("0x%02x" % b for b in data[i:i + 20]) + ",\n")
        f.write("};\n")
    print("embed_web: %s -> %s (%d bytes gzipped)" % (src, dst, len(data)))
