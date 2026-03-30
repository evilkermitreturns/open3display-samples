# Open3Display Samples

Sample tools and test applications for the [Open3Display](https://github.com/evilkermitreturns/open3d) stereoscopic display runtime.

## Contents

| File | Description |
|------|-------------|
| `test_app.cpp` | Standalone test — proves the full stereo pipeline without any game |
| `render_verification.cpp` | Automated render verification for all compositor modes |
| `lenticular_test.cpp` | Lenticular/weaving display test with configurable parameters |
| `lenticular_calibrator.cpp` | Interactive calibration tool for lenticular displays (requires LeiaSR SDK) |
| `samsung_3d_test.cpp` | Samsung Odyssey 3D test with LeiaSR lens activation |
| `samsung_live_test.cpp` | Full DisplayRuntime + LeiaSR lens integration test |

## Building

These samples are built from the main Open3Display monorepo:

```bash
cd open3d
cmake -B build -G "Visual Studio 18 2026" -A x64 -DOPEN3D_BUILD_SAMPLES=ON
cmake --build build --config Release
```

Samsung/LeiaSR samples additionally require `LEIASR_SDKROOT` environment variable pointing to the LeiaSR SDK.

## Relationship to Open3Display

This repository is a **distribution copy** of `tools/` from the [open3d](https://github.com/evilkermitreturns/open3d) development monorepo (following the Khronos OpenXR-SDK / OpenXR-SDK-Source pattern). Development happens in the monorepo; this repo is for browsing and reference.

## License

MIT — see [LICENSE](LICENSE).
