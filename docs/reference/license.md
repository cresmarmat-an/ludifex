# License

ludifex is released under the
[MIT License](https://github.com/cresmarmat-an/ludifex/blob/main/LICENSE).
Copyright (c) 2026 Cresmar Mat-an.

You can use it in free and commercial programs, change it, and distribute it,
as long as the copyright notice and license text are included with copies of
ludifex itself.

## Third-party software

A program built with ludifex also contains the libraries ludifex is built
from, each under its own permissive license:

| Library | License | Used for |
|---|---|---|
| SDL3 | zlib | The window, input, the GPU device, and audio output. |
| Box2D | MIT | 2D physics. |
| Box3D | MIT | 3D physics. |
| cgltf | MIT | Reading glTF models. |
| stb_image | MIT or public domain | Decoding images. |
| miniaudio, with stb_vorbis | MIT No Attribution or public domain | Mixing and decoding audio. |
| Assimp | BSD 3-Clause | Reading model formats other than glTF. Left out with `LUDIFEX_ASSIMP=OFF`. |

Assimp includes several libraries of its own (zlib, minizip, pugixml,
Clipper, poly2tri, OpenDDL-Parser, RapidJSON, Open3DGC, UTF8-CPP, and
stb_image), all under permissive licenses.

[`THIRD_PARTY_NOTICES.md`](https://github.com/cresmarmat-an/ludifex/blob/main/THIRD_PARTY_NOTICES.md)
reproduces all of their licenses. Ship that file with your program and the
notices are covered.
