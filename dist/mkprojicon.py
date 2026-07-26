#!/usr/bin/env python3
# mkprojicon.py — turn a WBTOOL .info into a WBPROJECT .info with a DefaultTool,
# i.e. a double-clickable script icon (DefaultTool "IconX" runs the script).
#
#   mkprojicon.py <in.info> <out.info> <defaulttool>
#
# The DefaultTool string block belongs right after the DiskObject images
# (before any ToolTypes). Our tool icons may carry an appended GlowIcons
# `FORM ICON` (OS3.5 colour states) — the string must be INSERTED before
# that IFF tail, not appended after it, or icon.library reads the FORM
# header as the string length and the icon breaks.
import struct, sys

d = bytearray(open(sys.argv[1], 'rb').read())
d[48] = 4                          # do_Type = WBPROJECT (1=DISK 2=DRAWER 3=TOOL 4=PROJECT)
d[50:54] = struct.pack('>I', 1)    # do_DefaultTool present (non-zero)
tool = sys.argv[3].encode('latin-1')
block = struct.pack('>I', len(tool) + 1) + tool + b'\x00'  # String: len(incl NUL) + bytes

# Find an appended colour-icon FORM (FORM....ICON) and insert before it.
insert_at = len(d)
i = d.rfind(b'FORM')
if i != -1 and d[i+8:i+12] == b'ICON':
    insert_at = i
d[insert_at:insert_at] = block
open(sys.argv[2], 'wb').write(d)
