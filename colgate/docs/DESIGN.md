# How Colgate works

Colgate started from three public descriptions of perceptual equalisation: an operation manual, an interview with a developer in the field, and patent application US 2005/0065781. Versions 1.1 and 1.2 were then tuned by measurement against 31 renders of three songs through Gullfoss, in its standard and Master modes. Neither that plug-in's code nor its formulas were available, so this is an independent model fitted to what its output does to the audio. It is close on that material, and it isn't a copy.

Measuring changed the design, sometimes against what the texts suggested.

## What the renders showed

| Measured in the reference | Colgate does | Where |
| --- | --- | --- |
| The EQ is minimum phase. Phase matches the Hilbert transform of log magnitude within about 1 degree. | The log magnitude and its Hilbert transform are built per band and added up. | `ColgateEngine::buildResponseBasis`, `runHop` |
| Both channels get one curve. Gains on left, right, mid and side agree. | One curve from the mean power of both channels. | `ColgateEngine::runAnalysisHop` |
| Recover and Tame add up. On every song, 50/50 is the sum of 50/0 and 0/50: within 0.05 dB on three of the four pairs, 0.13 dB on the other. | Each control contributes its own part, and they are summed. | `PerceptualField::compute` |
| Each control both lifts and cuts. Recover alone dips the mids by about 0.7 to 1 dB and Tame alone lifts the treble by about 1.4 to 2 dB. The manual says the same: the combination keeps perceived loudness. | One broadband gain follows the two controls. | `keepLoudnessDb` |
| That broadband gain holds the sum over the bands of power^0.35 constant, with no ear weighting. Frame by frame this stays within about 0.15 dB across all three songs. It follows the music over time: correlation about 0.99 for Tame alone and 0.84 to 0.92 for Recover alone. | The same rule, solved every hop. | `keepLoudnessDb` |
| The gain depends on a band's level relative to the total and to its near neighbours. Wider scales add nothing once the broadband gain is modelled. A masking gradient, tonality, pitch and rhythm layers add under 1%. | Two weights and a floor at -60 dB. The masking model and both layers are gone, although the patent describes them. | `PerceptualField::compute` |
| Recover and Tame have different zero points across the spectrum. In the bass both act on the same band at once. | A static curve per control, with a soft knee at its zero. | `ModeTuning` |
| Bias had no effect with Recover equal to Tame, the only case measured. | Bias enters only through the difference between them, so that's exact. | `PerceptualField::compute` |
| Brighten is a tilt around 900 Hz that levels off in the treble and levels off with amount. Upward it also calms the treble's movement. | Both, in a form that stays smooth through zero. | `PerceptualField::compute` |
| Gains ease off above about 8 kHz, including the broadband gain, however the signal behaves. | The same fade, by frequency. | `kFadeKnotZ` |
| Boost follows the ISO 226 contour difference closely, to within about 0.25 dB up to 5.5 kHz in standard mode and 0.7 dB in Master, but 1.25 times as large in standard mode and 2.2 times in Master. It keeps rising above the standard's 12.5 kHz. | The contour difference, times a scale per mode, plus a measured treble correction. | `ColgateEngine::updateBoostCurve` |
| Boost holds a different measure: the sum of power^0.5. That reproduces how far the mids drop on each song, and how that moves over time, with correlation 0.93 to 0.98. | A second broadband gain for Boost alone. | `ColgateEngine::runAnalysisHop` |
| Energy reacts over 100 to 150 ms. | Band energy rises with a 120 ms time constant and falls with 40 ms. | `ModeTuning` |
| Master differs mainly in the 3 to 10 kHz area, where Recover lifts about 0.5 to 1 dB less and moves less, and in how much further Boost goes. | A separate set of constants for each mode. | `kBasic`, `kMaster` |

Earlier versions got some of this wrong. Version 1.0 used a masking model and a linear-phase filter. Version 1.1 used a single field and treated Recover and Tame as one line through it, which was fine with them equal and wrong with them apart.

## From the sources to the code

| The source says | Colgate does | Where |
| --- | --- | --- |
| Time and frequency relate like position and momentum. A distribution from the phase-space form of quantum theory is used instead of an FFT. | A smoothed pseudo Wigner-Ville distribution, a member of Cohen's class. Publicly known mathematics from Wigner (1932), Moyal (1949) and Cohen (1966). | `PhaseSpaceAnalyzer` |
| Analyse with logarithmic resolution, like the ear (patent [0027]). | Three lag lengths: about 43 ms below 500 Hz, 11 ms up to 3.5 kHz, 3 ms above. | `PhaseSpaceAnalyzer` |
| Bias classifies the range: positive gives Recover more space, negative gives Tame more. | A line moved in the field, acting on the difference between the two controls. Not measured with them apart. | `PerceptualField::compute` |
| Boost is a global parameter that simulates listening level, in dB, and keeps perceived loudness. | Works without Recover or Tame and ignores the range limiters. | `ColgateEngine::updateBoostCurve` |
| Master has fixed best quality and finer control at the same latency. | Master always runs at the Best overlap, shows a decimal more and drags slower unless Shift is down. | `wantedQuality`, `setFineMode` |

## Signal flow

```
                    +-------------------------------------------------+
 main or sidechain  |  phase-space distribution  ->  band energies    |
 ------------------>|  relative level -> Recover part + Tame part     |
                    |  Brighten tilt, broadband gain, top fade         |
                    |  Boost curve and its own broadband gain          |
                    +------------------------+------------------------+
                                             | one set of band gains
                                             v
 main ----> Hann frames ----> minimum-phase filter ----> overlap-add ----> Gain ----> out
   |                                                                    ^
   +-------------------------- delay of the same latency -------------- + crossfade for Bypass
```

The analyser never touches the audio. With every control at zero the engine crossfades to the delay line, which makes that state bit-exact. Latency is one analysis window: 256 samples at 48 kHz, 5.3 ms.

## How close it is

All numbers compare with Gullfoss. The songs are a 26-second clip and two 12-second clips.

**The fair test: songs the fit never saw.** Each song below is scored with constants fitted only on the other two, through the finished engine's actual audio. Version 1.1 had never heard songs 2 and 3, so the comparison is like for like. R² is the share of the reference's per-band gain, frame by frame, that the model explains. *Curve* is the error of the average EQ curve. *Total* is the RMS gain difference per band and frame, and *dry* is the same for no processing at all.

| Setting, songs 2 and 3 | Mode | R² 1.1 | R² 1.2 | Curve 1.1 | Curve 1.2 | Total 1.1 | Total 1.2 | Dry |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 50/50 | Basic | 0.95 | 0.96 | 0.29 dB | 0.24 dB | 0.48 dB | 0.45 dB | 2.48 dB |
| 25/25, Bias 50, Brighten 50 | Basic | 0.92 | 0.93 | 0.24 dB | 0.27 dB | 0.37 dB | 0.37 dB | 1.54 dB |
| Recover 50 alone | Basic | 0.62 | 0.93 | 0.84 dB | 0.25 dB | 0.99 dB | 0.42 dB | 1.68 dB |
| Tame 50 alone | Basic | 0.37 | 0.87 | 0.69 dB | 0.31 dB | 0.86 dB | 0.38 dB | 1.21 dB |
| 15/15, Bias 15, Brighten 15, Boost +30 | Basic | 0.43 | 0.98 | 3.45 dB | 0.38 dB | 3.39 dB | 0.66 dB | 5.13 dB |
| 50/50 | Master | 0.94 | 0.94 | 0.26 dB | 0.28 dB | 0.52 dB | 0.50 dB | 2.26 dB |
| 25/25, Bias 50, Brighten 50 | Master | 0.61 | 0.87 | 0.62 dB | 0.25 dB | 0.66 dB | 0.37 dB | 1.18 dB |
| Recover 50 alone | Master | 0.38 | 0.89 | 0.90 dB | 0.27 dB | 1.14 dB | 0.48 dB | 1.48 dB |
| Tame 50 alone | Master | 0.41 | 0.85 | 0.70 dB | 0.32 dB | 0.85 dB | 0.41 dB | 1.24 dB |
| 15/15, Bias 15, Brighten 15, Boost +30 | Master | -0.09 | 0.97 | 8.90 dB | 1.02 dB | 8.74 dB | 1.51 dB | 10.49 dB |

The gain was in the settings 1.1 had to estimate. With Recover equal to Tame on an unseen song, 1.1 was already about as good, which is a useful sign that those constants weren't just memorising the first clip.

**Song 1 under the same test** (fitted on songs 2 and 3 only): R² 0.82 to 0.88 in Basic and 0.73 to 0.83 in Master, on settings of 15/15 and 50/50. These scores are lower partly because 15/15 moves so little that small errors count for more. Master's 15/15 row is the weakest in the whole set.

**The shipped constants, fitted on all three songs,** so this flatters it a little:

| | Renders | R² | Curve | Movement correlation | Total | Dry | Null |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Basic, no Boost | 14 | 0.91 | 0.20 dB | 0.88 | 0.34 dB | 1.41 dB | -7.9 dB |
| Basic, Boost +30 | 2 | 0.98 | 0.28 dB | 0.94 | 0.57 dB | 5.13 dB | -14.8 dB |
| Master, no Boost | 13 | 0.88 | 0.21 dB | 0.85 | 0.33 dB | 1.21 dB | -7.0 dB |
| Master, Boost +30 | 2 | 0.98 | 0.61 dB | 0.93 | 1.26 dB | 10.49 dB | -4.6 dB |

*Null* is the output minus the render, relative to the render minus the dry signal, after correcting any delay of a fraction of a sample between them, so 0 dB would mean the plug-in got nothing right. Its best case is -15.6 dB and its worst +1.1 dB, on song 3's Master Boost. There the output's bass runs high enough that the peak reaches +1.9 dBFS where the render peaks at -2.2 dBFS, even though the gain curve is within about 1 dB at every band.

`tools/calibration` reproduces the fit and these scores from the audio files.

## What the tests check

All of this runs on every build, using synthetic test material so no audio files are needed: `tests/engine_tests.cpp` and `tests/plugin_tests.cpp`. Values are from x86_64 Linux and may differ slightly on Apple silicon. The test mix has drums, bass, a pad, a voice-like lead and top-end air, with stereo placement.

| Promise | Result |
| --- | --- |
| All controls at zero is no processing | Bit-exact, at 16, 44.1, 48, 96, 192 and 384 kHz, mono and stereo |
| Reported latency is true | The impulse response peaks there, and less than -59 dB of its energy comes earlier, even at Boost +40 |
| EQ is minimum phase | The anti-causal part of its cepstrum is below -120 dB |
| The graph shows the truth | Within 0.01 dB of the measured gain, and within 0.05 dB for the copy the display reads |
| Recover and Tame add up, as in the reference | 50/50 is the sum of 50/0 and 0/50 within 0.02 dB |
| The broadband gain does its job | The sum of power^0.35 holds to 0.003 dB |
| Stems add up to the processed mix (sidechain) | Null at -129 dB |
| Block size doesn't matter | Identical down to 1 sample |
| Both channels get one curve | A level difference between them passes unchanged, to -121 dB |
| Modes share a shape and a latency | Average curves correlate at 0.985, differ by at most 1 dB, and latency is equal |
| Switching mode while playing | No click: the largest step stays within 1.1 times the sine's own |
| Bias does nothing with Recover equal to Tame | 0.00 dB, as in the reference. With them apart it moves the curve |
| A masked tone is uncovered | A tone 32 dB under a neighbour 400 Hz below rises about 6.5 dB while the masker falls about 7.8 dB, at 100/100 |
| Boost makes room in the mids | -6.9 dB in Basic and -13.7 dB in Master at Boost +30 on pink noise |
| Pink noise isn't reshaped | Largest band gain 1.1 dB at 100/100, not counting the lowest two bands |
| Better resolution than a spectrogram | Valley between partials 188 Hz apart: 5.7 dB, against 1.2 dB for a Hann spectrogram of the same span |
| No allocation in the audio path | Counted, including moving every control |
| Bad input is contained | NaN and infinity never reach the output, and audio continues, in both modes |
| Runs in real time | One stereo instance on a 2.8 GHz Xeon cloud core: about 18 times real time at Normal (6% of a core), 10 times at Better, 5 to 6 times at Best and in Master (about 18%) |

The finished VST3 is also loaded through its public interface by a small host (`tools/vst3_host_check.cpp`), which checks parameters, state, audio and the editor.

## Limits worth knowing

- **Three songs is still three songs.** Two are only 12 seconds long. Leaving one out at a time gives a fair estimate for music like these, and says less about very different styles, levels or arrangements.
- **Settings nobody measured.** Bias with Recover and Tame apart. Boost at anything other than +30, and without 15/15 under it, so those values follow from the contours and the assumption that the treble correction scales with Boost. Amounts above 50. Brighten beyond +50 or below -15. Those may differ from the reference.
- **Nobody has listened to it.** The match is in numbers, and error can sit in what the numbers miss.
- **It moves too little at low settings.** At 15/15 its gain moves about 0.75 times as much as the reference in Basic and about 0.7 times in Master, though the average curve is right. This setting is the one most people will use, so it's the first thing to check by ear. Counting low settings more heavily in the fit didn't help on unseen songs.
- **The Mac build hasn't been run by the author.** The code was built and tested on Linux, and the packaging script was dry-run there with stand-ins for Apple's tools. The first GitHub Actions run is the first real Mac build, and it runs the tests, auval and pluginval there.
- **Sparse material.** The field was fitted on full mixes. At 15/15 a lone sine stays within 0.8 dB, but at 100/100 it loses 5 to 7.5 dB, partly because the analyser's leakage counts as energy in the loudness rule. At 110 Hz, sidebands sit about 36 dB below the tone at 100/100 and 65 dB below at 15/15. The reference's behaviour on this isn't known. A single guitar or vocal is the next thing worth measuring.
- **Loudness on a meter can move.** The rule that matches the reference isn't BS.1770. On the synthetic mix that means 0.6 LU at 15/15, 1.9 LU at 50/50 and 3.7 LU at 100/100. In its own renders the reference moved up to 1.6 LU at 50/50, and without Boost Colgate tracked it to within 0.4 LU on these songs.
- **Boost can raise peaks a lot.** Master at Boost +30 lifted the bass enough to raise peaks by 8 dB in the reference and 12 dB in Colgate on song 3, so watch the output. Above 40 dB per band, Boost's curve rises at a quarter of the rate.
- **The deep bass can't be shaped finely.** The filter can't form details narrower than about 45 Hz either side, so below about 100 Hz the EQ acts as a broad move. The graph shows this as it really is.

## Not included

Windows and AAX builds, and the low-latency edition that the manual describes.

## Where to change things

- **Version:** the `project(Colgate VERSION ...)` line in `CMakeLists.txt`.
- **The tuning of each mode:** `kBasic` and `kMaster` in `source/engine/PerceptualField.cpp`. `tools/calibration` prints new values from new renders.
- **Presets:** `kFactoryPresets` in `source/plugin/Parameters.h`.
- **Colours and type:** `source/plugin/Theme.h`.
- **See what the engine does to a signal:** `colgate_measure` and `colgate_render`.
