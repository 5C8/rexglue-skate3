# Creating a release package

Build from the clean source plus your locally generated implementation. Package
with the explicit runtime allowlist; never archive the entire build directory.
The packager includes docs, dependency notices and per-file SHA-256 checksums.

```powershell
python tools/package_release.py --install-dir C:/Dev/rex-sdk --output rexglue-skate3-Windows-x64.zip
```

Create a GitHub release from the corresponding source tag and attach the ZIP
and its SHA-256 checksum. Game binaries contain the compiled implementation;
the generated main-game implementation must remain local and ignored.
The initial publication packages the existing gameplay-tested local Windows
Release executables. Cleanup changes concern source organization, portability,
documentation and packaging; the runtime behavior is preserved.
