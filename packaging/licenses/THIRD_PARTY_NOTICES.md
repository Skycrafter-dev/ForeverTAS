# Third-party notices

ForeverTAS portable bundles dynamically deploy the Qt runtime components used
by the application. Qt is available under the GNU Lesser General Public License
version 3; the complete license text and Qt GPL exception are included beside
this notice. Corresponding Qt source code is available from
https://download.qt.io/official_releases/qt/.

ForeverTAS embeds ForeverValidator. Its license is installed as
`ForeverValidator-LICENSE.txt` in every bundle.

ForeverTAS embeds Google Blockly 12.5.1 for the local visual block editor.
Blockly is licensed under the Apache License 2.0; its license is installed as
`Blockly-LICENSE.txt` in every bundle. The vendored source-version metadata is
kept in `third_party/blockly/VERSION` and `third_party/blockly/package.json`.

Additional system libraries collected into a Linux AppImage retain their own
licenses. Release builders must inspect the generated AppDir and include any
notices required by those libraries before publishing a release.
