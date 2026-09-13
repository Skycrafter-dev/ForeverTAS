# ForeverTAS replacement textures

The physical materials are resized from photo-scanned CC0 Poly Haven sets and
an ambientCG foliage atlas. The game-specific materials are built-in ImageGen
outputs created expressly for this renderer. No texture pixels are extracted
from TrackMania.

Every shipped texture is a 512 x 512 PNG. The same albedo asset, baked UV
mapping, material parameters, vertex-color rule, and two-sided opaque surface
are used by the raster and ray-traced renderers. Their lighting implementations
remain intentionally independent.

Concrete is the single deliberate exception to the detailed material set: its
base color is uniform by art direction, without white aggregate spots. See
`PROVENANCE.txt` for the source and prompt used for every material.
