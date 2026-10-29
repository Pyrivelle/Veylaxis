# Reused source

`earcut/` contains the unchanged header and ISC license from the existing
perevia-osgkit checkout, upstream commit 177bd66b5338ab66f4e9ce8f3cca418b9463d809.
SHA-256: 29091c0d7af22a2f985f5ebd9aedb1f1b056fe099d1d31395c393c5f1ecb8c52.
Only the optional OSG Builder uses it; it does not perform boolean clipping.

The deterministic island formula in IslandFixture.cpp was ported from
perevia-osgkit/tests/IslandExample.h. Its MIT notice is retained in
notices/perevia-MIT.txt. No files in that original project were modified.
