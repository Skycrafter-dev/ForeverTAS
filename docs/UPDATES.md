# Installation and updates

The download links are the same for every release:

- [Windows installer](https://github.com/Skycrafter-dev/ForeverTAS/releases/latest/download/ForeverTAS-Windows-Setup.exe)
- [Linux AppImage](https://github.com/Skycrafter-dev/ForeverTAS/releases/latest/download/ForeverTAS-Linux.AppImage)

There is one ForeverTAS per platform, for every computer. It has no GPU code
of its own: the CPU backends work everywhere, and the GPU backends come as a
**GPU module**.

## GPU modules

At start, ForeverTAS looks at the graphics card (NVIDIA: its compute
capability, through the driver; AMD: an RX 7000 or 9000) and, if it has no
module for it yet, downloads the one for it in the background (a few MB to a
few tens of MB), checks its SHA-256, unpacks it and loads it: CUDA or HIP
appears in the physics backends, no restart. The Search settings show the
progress, and the log says what happened.

- NVIDIA: one module per GPU generation (sm 5.0, 5.2, 6.0, 6.1, 7.0, 7.5, 8.0,
  8.6, 8.9, 9.0, 10.0, 12.0), each with the code compiled for it. Other GPUs of
  a generation run its code (8.7 runs 8.6's, 12.1 runs 12.0's). A later GPU
  than all of them compiles the 12.0 module's PTX when it loads it (once; the
  driver keeps the result).
- AMD: one HIP module for gfx1100, gfx1101, gfx1102, gfx1200 and gfx1201
  (RX 7000 and 9000), with its HIP runtime.

Modules live in the app data folder (`compute/`, one folder per module). A
module is only for the app it was built with: each carries a key (a hash of
the code they share), and the app loads only modules with its own key. After
an app update the old module no longer loads, and the new app downloads its
own (often the same size; GPU code only changes with the physics).

The app finds its modules through its own version's `updates.json`
(`releases/download/v<version>/updates.json`), so an older ForeverTAS still
finds the modules made for it.

## App updates

At start, ForeverTAS reads `updates.json` of the latest release. If it is
newer, it offers the update; the download is checked against its SHA-256
before it runs. Windows: per-user Inno Setup install, upgraded in place.
Linux: a writable AppImage is replaced after the old process exits (one
`.previous` copy is kept), and started again. Read-only AppImages and copies
not installed by the installer do not update themselves. Nothing is installed
without a click, except the GPU module of the installed version.

## updates.json

```json
{
 "schema": 1,
 "version": "0.3.0",
 "release_url": "https://github.com/Skycrafter-dev/ForeverTAS/releases/tag/v0.3.0",
 "assets": [{"name": "ForeverTAS-0.3.0-linux-universal-x86_64.AppImage", "sha256": "..."}, ...],
 "compute": {
  "linux": [{"id": "cuda-sm86", "backend": "cuda", "sm": 86, "ptx": false, "abi": "...",
             "url": "https://github.com/.../releases/download/compute-modules/forevertas-compute-linux-cuda-sm86-<hash>.tar.zst",
             "sha256": "...", "size": 7300000}, ...],
  "windows": [...]
 }
}
```

`assets` is what ForeverTAS 0.2.7 and earlier read: they update to the
`universal` asset, which is the one app, and that one then installs its GPU
module. The app accepts only HTTPS URLs of this repository's releases, and
checks every download against its SHA-256 before using it.
