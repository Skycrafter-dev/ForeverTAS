# Installation and updates

The public download links are stable across releases:

- [Windows installer](https://github.com/Skycrafter-dev/ForeverTAS/releases/latest/download/ForeverTAS-Windows-Setup.exe)
- [Linux AppImage](https://github.com/Skycrafter-dev/ForeverTAS/releases/latest/download/ForeverTAS-Linux.AppImage)

These are **universal** CPU + Vulkan builds. After the first launch,
ForeverTAS checks the latest published GitHub Release and recommends a more
specific package for an NVIDIA or supported AMD GPU when available. The user
confirms the download once; the app verifies its SHA-256 digest, installs it,
and restarts. The **Change compute package** button in the Bruteforce panel
opens the same selector at any time. Manual selection persists; checking
**Choose automatically for this computer** restores hardware selection. App
settings remain in the platform's per-user settings store across switches.
The [release assets](https://github.com/Skycrafter-dev/ForeverTAS/releases/latest)
also expose every GPU package separately for a direct first download when the
user knows their compute capability.

The initial installer/AppImage is downloaded once. Windows uses a per-user
Inno Setup installation with a stable AppId; the same install is upgraded when
switching package. Linux uses a writable AppImage path; a verified new image
replaces that path after the old process exits, keeps one `.previous` backup,
and launches the new image. Read-only AppImages and portable Windows ZIPs do
not offer in-app replacement. No update or package switch starts without a
click.

Each published GitHub Release includes an `updates.json` asset. The app reads
that stable latest-release URL on launch; the file lists the version, package
names, and SHA-256 hashes. No GitHub API rate limit or paginated asset list is
involved. The check is asynchronous and a network failure never prevents
startup. A versioned
package name has the form:

```text
ForeverTAS-<version>-linux-<flavor>-x86_64.AppImage
ForeverTAS-<version>-windows-<flavor>-x86_64-Setup.exe
```

`flavor` is `universal`, `nvidia-smNN`, or `amd-rx7000-rx9000`. Each NVIDIA
package is built for **one** CUDA SM target, with a same-target PTX image;
CUDA 5.0+ and Fast CUDA's 7.5+ runtime requirement are distinct from package
selection. The AMD package contains HIP code for gfx1100, gfx1101, gfx1102,
gfx1200, and gfx1201 (RX 7000/9000 families). Each package also contains
CPU and Vulkan backends. An unsupported or undetected GPU falls back to the
universal build, and users can manually choose another published package.
Choosing a GPU package does not make incompatible hardware run that backend.

The app only accepts a catalog with an exact package name and HTTPS release
URL for this repository. It constructs the download URL itself and checks the
catalog's SHA-256 digest against downloaded bytes before executing the
installer/AppImage. Keep
GitHub release-publishing access protected; sign Windows installers before
uploading and regenerate their sidecars after signing. The release process
checks the GitHub-hosted digest of **every** asset, including `updates.json`,
before publication.

## Release process

1. Commit all release source, including the desired version in `CMakeLists.txt`
   and `packaging/release/manifest.json`, then tag that commit `v<version>`
   and push the tag to `origin`.
2. Build Linux with `packaging/release/distribution.py build-linux`. It reuses
   pinned Docker toolchains and separate build directories per flavor.
   `--flavor nvidia-sm75` limits a development build to one flavor.
3. Build Windows on the MSVC build host with
   `packaging/release/build-distribution-windows.ps1`. Inno Setup 6/7,
   CUDA 12.8, the HIP SDK, Qt, vcpkg, and the MSVC runtime must be available.
   Copy the resulting installers and `.sha256` sidecars into the same local
   `dist/` directory as the Linux AppImages before preparing the release.
4. Sign each Windows installer if a signing certificate is available, then
   regenerate its `.sha256` sidecar. Run
   `packaging/release/distribution.py prepare` to create stable first-install
   aliases and `updates.json`, followed by `distribution.py verify` to require
   the full matrix.
5. `distribution.py draft` uploads the versioned packages, sidecars, and two
   stable aliases. `distribution.py publish` compares every GitHub asset digest
   against the local file and only then makes the release visible.
6. Test the universal first install, automatic recommendation, manual switch,
   and next-version update on Windows and Linux before making the release the
   default download route.

All large binaries are GitHub Release assets, not Git-tracked files. The
current older release pipeline in `local-release.py` still builds its legacy
CUDA fat binary; do not use it to publish a hardware-specific release.
