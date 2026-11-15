# Third-party notices

## RetroCPU

Source: <https://github.com/achaulk/retro_cpu>

Pinned revision: `ea9049ab25084334f7cc1907b3a98bf1c2604a03`

Copyright (c) 2019 Albert Chaulk

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## snes_spc 0.9.0

Source: <https://github.com/blarggs-audio-libraries/snes_spc>

Pinned revision: `ec8ee2bbe30451614c1d02a83f7af1c97d497d45`

Copyright (C) 2004-2007 Shay Green

Licensed under the GNU Lesser General Public License, version 2.1 or (at your
option) any later version. The complete license text is distributed in the
upstream source as `license.txt`.

## Misaki Gothic localization font

Copyright (C) 2002–2021 Num Kadoma. Source and license:
<https://littlelimit.net/misaki.htm> and <https://littlelimit.net/font.htm#license>.
Permission permits use, copying and distribution with or without modification,
commercially or noncommercially, without warranty. The source BDF, the author's
license text and the generation instructions are in `assets/fonts` in this
repository.

## xBRZ (earlier revisions only)

Source: <https://github.com/janisozaur/xbrz>

Copyright (C) Zenju (zenju AT gmx DOT de)

Earlier revisions of this repository carried an optional xBRZ backend for the
desktop `2D FILTER` option and an xBRZ-derived GPU shader, licensed under the
GNU General Public License, version 3 ([LICENSE-XBRZ.txt](LICENSE-XBRZ.txt)).
Nintendo 3DS builds never included them. The current source neither contains
nor fetches xBRZ; the `XBRZ` filter value is reported as not compiled in.

## ScaleFX

`src/render/scalefx.cpp`, the CPU implementation of the ScaleFX 2D filter, is
derived from Sp00kyFox's ScaleFX passes 0–4 in libretro/slang-shaders, revision
b61e1ee4fc9e2119ec933461a0bfad024dd2950a (2017-03-01 shader revision).
Source: https://github.com/libretro/slang-shaders/tree/b61e1ee4fc9e2119ec933461a0bfad024dd2950a/edge-smoothing/scalefx

Copyright (c) 2016 Sp00kyFox - ScaleFX@web.de

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

## Built-in updater transport

The Starwing-specific updater adapter and policy are newly written for this
port. They use curl 8.4.0 (curl license), Jansson 2.14 (MIT), and mbedTLS 2.28.8
(Apache-2.0), with the official devkitPro 3DS patches. Exact source URLs and
SHA-256 records and the full component licenses are retained under
`platform/3ds/update-dependencies/`. The public Mozilla CA set is distributed
through curl's CA extraction service; its source and conversion header remain
in `platform/3ds/romfs/update-ca.crt`.

These component notices do not assign a new blanket license to the engine or
to Nintendo game data. No game-specific updater implementation from another
port is incorporated into this adapter.
