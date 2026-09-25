# Third-party notices

ForeverTAS portable bundles dynamically deploy the Qt runtime components used
by the application. Qt is available under the GNU Lesser General Public License
version 3; the complete license text and Qt GPL exception are included beside
this notice. Corresponding Qt source code is available from
https://download.qt.io/official_releases/qt/.

ForeverTAS embeds ForeverValidator. Its license is installed as
`ForeverValidator-LICENSE.txt` in every bundle.

The AMD HIP Linux AppImage bundles the HIP runtime, ROCr HSA runtime,
rocprofiler-register, and libdrm-amdgpu. Their license notices are included
beside this file in that bundle.

Additional system libraries collected into a Linux AppImage retain their own
licenses. Release builders must inspect the generated AppDir and include any
notices required by those libraries before publishing a release.
