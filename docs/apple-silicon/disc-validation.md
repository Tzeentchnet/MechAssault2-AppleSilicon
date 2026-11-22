# Private disc validation

- Repository HEAD: `5ecc611d18e20de1ab04313b0c50e453d8d63b29`, with the
  validation-only portability changes in the working tree.
- Main archive integrity (`7zz t`): **pass**.
- Input form: a plain XDVDFS disc image extracted to private session storage.
- Quick supported-retail executable check: **pass**, using the existing
  installer disc-validation stage.
- Complete supported US/NTSC disc-manifest check: **pass**, using the existing
  installer extraction and manifest-verification stages with the shipped
  reference data.

The image and extracted game files remain outside the repository. The checks
ran through `Installer.run_steps()` with `until='extract'` (the stage sequence
selected by `--until extract`), without invoking Windows build prerequisites.
