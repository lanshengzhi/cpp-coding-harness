# stb image processing headers

This directory vendors `stb_image.h`, `stb_image_resize2.h`, and
`stb_image_write.h` from
[`nothings/stb`](https://github.com/nothings/stb) commit
`f75e8d1cad7d90d72ef7a4661f1b994ef78b4e31`.

The headers are compiled only by `src/coding_agent/ImageInput.cpp`; no stb type
or macro is exposed through a project header. Each vendored header contains the
upstream dual public-domain/MIT license notice.
