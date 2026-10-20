# Licence texts poky does not carry

`create-spdx` writes the full text of every licence into the SBOM. It looks first in poky's
`common-licenses`, then in every directory on `LICENSE_PATH`. A licence it cannot find anywhere
fails the build, which is how these two were noticed.

Both files are the upstream text, fetched from the same URLs the recipes fetch from, and both match
the checksum the recipe declares:

| File | Licence of | md5 | Declared in |
|---|---|---|---|
| `Proprietary-VSI` | VeriSilicon video encoder IP | `2c81b04b9390e9d08d51eea41d622b4e` | `meta-hailo-media-library/recipes-media-library/video-encoder/video-encoder.bb` |
| `LICENSE` | Hailo NNC firmware, binary redistribution | `263ee034adc02556d59ab1ebdaea2cda` | `meta-hailo/meta-hailo-vpu/recipes-firmware/hailo15-nnc-fw/hailo15-nnc-fw_5.3.0.bb` |

The second filename is as awkward as it looks. That recipe sets `LICENSE = "LICENSE"`, so the
licence is literally named `LICENSE`, and the file has to carry that name to be found. Putting the
right text behind the wrong name is the most this layer can fix on its own: the SBOM will quote
Hailo's terms correctly but will still call the licence `LICENSE`, which tells a reader nothing.
Naming it properly has to happen in Hailo's recipe.

Do not confuse `custom-licenses/LICENSE` with the licence of this layer, which is `LICENSE` at the
root of the repository and is MIT.
