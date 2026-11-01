# Notices

WireStationVPN is GPL-3.0-or-later ([LICENSE](LICENSE)). It is built from
third-party open-source code under its own licenses, which the GPL allows.
The main parts:

| Code | License | What it does here |
| --- | --- | --- |
| [Tailscale](https://github.com/tailscale/tailscale) (`tailscale.com`) | BSD-3-Clause | The whole Tailscale client: sign-in, WireGuard, NAT traversal, the user-space network stack that makes the exit node |
| [gVisor](https://github.com/google/gvisor) netstack | Apache-2.0 | The TCP/IP stack inside Tailscale's user-space mode |
| [wireguard-go](https://git.zx2c4.com/wireguard-go) | MIT | WireGuard, inside Tailscale |
| [go-qrcode](https://github.com/skip2/go-qrcode) | MIT | The sign-in QR code on the status page |
| The Go standard library | BSD-3-Clause | |

Every module compiled into the payload is in `go.sum`. `scripts/licenses.sh`
writes `THIRD-PARTY-LICENSES.txt` with each one's license text, and the
build ships it next to `wirestation.elf` and in the app.

## In the PS5 app

The app (`app/`, PPSA99556) is built by `app/setup.sh` and `app/build.sh`.
Its screen, controller and drawing code (`app/src/ui/`, `vk_context`,
`log`, `compat.c`, `check-imports.py`, `setup.sh`) comes from
[TokenStation5](https://github.com/ThatRetiredDude/TokenStation5), the same
author's GPL-3.0 project.

These projects are fetched at pinned revisions and built into the app;
none of their code is stored in this repo:

| Project | What the app uses | License |
| --- | --- | --- |
| [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) | title start code (`app_crt.cpp`), link recipe, RADV's PS5 platform layer, `libc.prx`, AGC link stubs | GPL-3.0-or-later |
| [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK) | cross toolchain wrapper, system module stubs, platform layer | GPL-3.0-or-later; its `include/freebsd` headers are BSD |
| [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) (Mesa 26.2 with RADV and ACO) | the Vulkan driver that draws the screen, linked in whole | MIT for RADV and most of Mesa; individual files carry their own SPDX identifiers |
| [LLVM libc++](https://libcxx.llvm.org/) | the C++ standard library | Apache-2.0 WITH LLVM-exception |

Whoever distributes a built app must also offer its corresponding source:
this repo at the commit it was built from (`SOURCE.txt` in the app names
it), plus the pinned revisions of the projects above.

### QR Code generator (MIT)

`app/third_party/qrcodegen` is Project Nayuki's
[QR Code generator library](https://github.com/nayuki/QR-Code-generator) (C++).
The PS5 app draws the Tailscale sign-in link with it, as a QR code on the TV.

```
QR Code generator library (C++)

Copyright (c) Project Nayuki. (MIT License)
https://www.nayuki.io/page/qr-code-generator-library

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:
- The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.
- The Software is provided "as is", without warranty of any kind, express or
  implied, including but not limited to the warranties of merchantability,
  fitness for a particular purpose and noninfringement. In no event shall the
  authors or copyright holders be liable for any claim, damages or other
  liability, whether in an action of contract, tort or otherwise, arising from,
  out of or in connection with the Software or the use or other dealings in the
  Software.
```


### Fonts for the PS5 app (`app/src/ui/font.bin`)

`app/src/ui/make_font.py` (from TokenStation5) renders glyphs from two fonts
into `app/src/ui/font.bin`, which the PS5 app builds in as bitmaps; no font
file is stored or shipped.

[Inter](https://github.com/rsms/inter) 4.0 (title, headings and body text),
Copyright 2016-2023 The Inter Project Authors, under the SIL Open Font
License 1.1.

```
Copyright 2016-2023 The Inter Project Authors (https://github.com/rsms/inter)

This Font Software is licensed under the SIL Open Font License, Version 1.1.

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font
creation efforts of academic and linguistic communities, and to
provide a free and open framework in which fonts may be shared and
improved in partnership with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded,
redistributed and/or sold with any software provided that any
reserved names are not used by derivative works. The fonts and
derivatives, however, cannot be released under any other type of
license. The requirement for fonts to remain under this license
does not apply to any document created using the fonts or their
derivatives.

DEFINITIONS
Font Software refers to the set of files released by the
Copyright Holder(s) under this license and clearly marked as such.
This may include source files, build scripts and documentation.

Reserved Font Name refers to any names specified as such after
the copyright statement(s).

Original Version refers to the collection of Font Software
components as distributed by the Copyright Holder(s).

Modified Version refers to any derivative made by adding to,
deleting, or substituting in part or in whole any of the components
of the Original Version, by changing formats or by porting the Font
Software to a new environment.

Author refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person
obtaining a copy of the Font Software, to use, study, copy, merge,
embed, modify, redistribute, and sell modified and unmodified copies
of the Font Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
   in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
   redistributed and/or sold with any software, provided that each
   copy contains the above copyright notice and this license. These
   can be included either as stand-alone text files, human-readable
   headers or in the appropriate machine-readable metadata fields
   within text or binary files as long as those fields can be easily
   viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
   Name(s) unless explicit written permission is granted by the
   corresponding Copyright Holder. This restriction only applies to
   the primary font name as presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the
   Font Software shall not be used to promote, endorse or advertise
   any Modified Version, except to acknowledge the contribution(s)
   of the Copyright Holder(s) and the Author(s) or with their
   explicit written permission.

5) The Font Software, modified or unmodified, in part or in whole,
   must be distributed entirely under this license, and must not be
   distributed under any other license. The requirement for fonts to
   remain under this license does not apply to any document created
   using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions
are not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED AS IS, WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL
THE COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR
CONSEQUENTIAL DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF THE USE OR INABILITY TO USE THE FONT
SOFTWARE OR FROM OTHER DEALINGS IN THE FONT SOFTWARE.
```

[DejaVu Sans Mono](https://dejavu-fonts.github.io/) (logs and code), from
Bitstream Vera. DejaVu's changes are in the public domain; Bitstream Vera's
license:

```
Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved. Bitstream Vera is
a trademark of Bitstream, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of the fonts accompanying this license ("Fonts") and associated
documentation files (the "Font Software"), to reproduce and distribute the
Font Software, including without limitation the rights to use, copy, merge,
publish, distribute, and/or sell copies of the Font Software, and to permit
persons to whom the Font Software is furnished to do so, subject to the
following conditions:

The above copyright and trademark notices and this permission notice shall
be included in all copies of one or more of the Font Software typefaces.

The Font Software may be modified, altered, or added to, and in particular
the designs of glyphs or characters in the Fonts may be modified and
additional glyphs or characters may be added to the Fonts, only if the fonts
are renamed to names not containing either the words "Bitstream" or the word
"Vera".

This License becomes null and void to the extent applicable to Fonts or Font
Software that has been modified and is distributed under the "Bitstream
Vera" names.

The Font Software may be sold as part of a larger software package but no
copy of one or more of the Font Software typefaces may be sold by itself.

THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF COPYRIGHT, PATENT,
TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL BITSTREAM OR THE GNOME
FOUNDATION BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, INCLUDING
ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL DAMAGES,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM OTHER DEALINGS IN THE
FONT SOFTWARE.

Except as contained in this notice, the names of Gnome, the Gnome
Foundation, and Bitstream Inc., shall not be used in advertising or
otherwise to promote the sale, use or other dealings in this Font Software
without prior written authorization from the Gnome Foundation or Bitstream
Inc., respectively. For further information, contact: fonts at gnome dot
org.
```


WireStationVPN isn't affiliated with or endorsed by Tailscale Inc.
"Tailscale" is their trademark, used here only to say what the program
works with.
