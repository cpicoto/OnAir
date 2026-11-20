# WW7ATS receiver (onair-ww7ats)

A one-screen DVB-T receiver for the **WW7ATS amateur TV repeater on Cougar Mountain** (Issaquah, WA). It is fixed to
**1255 MHz, 6 MHz channel, DVB-T** and only has the controls you need to watch the repeater:

- **Radio**: pick the SDR (the first one found is used by default) and **Refresh** the list.
- **Start / Stop**.
- **Gain**: one slider. On a HackRF it is split over the LNA (steps of 8 dB) and the VGA (steps of 2 dB), with an
  optional **RF amp** (+14 dB). On other radios (SoapySDR, RTL-SDR, Airspy, PlutoSDR, ...) it is the radio's overall gain.
  Changes apply live.
- **Volume** and **Mute**.
- **Picture**, and a **constellation** of the equalised data cells with the ideal points of the signalled modulation
  (green once the error correction is locked).
- A status line: lock stage, SNR, frequency offset, the TPS parameters (FFT size, modulation, guard, code rate) and the
  Reed-Solomon counters. When the multiplex carries more than one service, a **Service** picker appears.

The receiver itself is the OnAir engine (`dect2core`), the same DVB-T decoder as the full app.

## Radio

1255 MHz is in the 23 cm band. A HackRF One covers it directly; so do most RTL-SDR (R820T/R828D tuners, up to about
1.7 GHz), Airspy R2/Mini, PlutoSDR and LimeSDR. An RTL-SDR's 2.4-3.2 Msps is too narrow for a 6 MHz DVB-T channel; use a
radio that runs at least 8 Msps. Start with the gain around the middle and raise it until the constellation points form
tight clusters; back off if the SNR drops (overload).

## Build

It builds with the rest of OnAir when the GUI is on (`DECT2_BUILD_APP=ON`); turn it off with `-DONAIR_BUILD_WW7ATS=OFF`.
It uses GLFW and Dear ImGui on OpenGL 3 on every system, independent of `DECT2_UI_BACKEND`.

macOS:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON -DDECT2_BUILD_TESTS=OFF
cmake --build build -j 8 --target onair-ww7ats
./build/onair-ww7ats
```

Windows: build in an MSYS2 UCRT64 shell as described in [BUILD.md](BUILD.md) (or cross-compile from macOS with
`tools/package/make_windows_cross.sh`), with the target `onair-ww7ats`. With `DECT2_COPY_RUNTIME_DLLS=ON` the DLLs it
needs are copied next to `onair-ww7ats.exe`.

The logic without a window (gain split, picture colours, lock stages) is tested by `test_ww7ats_logic`.

## Licence

GPL-3.0-or-later, as OnAir. The About box carries the attribution line required by
[ADDITIONAL-TERMS.md](../ADDITIONAL-TERMS.md): "Based on OnAir by Kristijonas Adomaitis - https://on-air.dev".
