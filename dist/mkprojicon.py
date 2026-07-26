#!/usr/bin/env python3
# mkprojicon.py — turn a WBTOOL .info into a WBPROJECT .info with a DefaultTool,
# i.e. a double-clickable script icon (DefaultTool "IconX" runs the script).
#
#   mkprojicon.py <in.info> <out.info> <defaulttool>
#
# A WBTOOL icon with no default tool / tooltypes is exactly DiskObject + Image1,
# so appending the DefaultTool string block right after it is the correct on-disk
# position (DefaultTool follows the images, before any ToolTypes).
import struct, sys

d = bytearray(open(sys.argv[1], 'rb').read())
d[48] = 4                          # do_Type = WBPROJECT (1=DISK 2=DRAWER 3=TOOL 4=PROJECT)
d[50:54] = struct.pack('>I', 1)    # do_DefaultTool present (non-zero)
tool = sys.argv[3].encode('latin-1')
d += struct.pack('>I', len(tool) + 1) + tool + b'\x00'   # String: len(incl NUL) + bytes
open(sys.argv[2], 'wb').write(d)
