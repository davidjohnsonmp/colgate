# Calibration

How Basic and Master were tuned, so it can be done again with more material. No audio is kept in this repository. Keep it that way, since neither the mix nor the reference renders are ours to publish.

## Make the renders

Bounce each song through the reference plug-in at 48 kHz, once per setting, and keep them in one folder per song. Names and subfolders say what a render is. A name or folder with `master` in it counts as Master mode, and everything else as Basic:

```
ref 1/original.wav
ref 1/basic/recover 50 tame 50.wav
ref 1/basic/recover 15 tame 15 bias 15 brighten 15 boost 30.wav
ref 1/master/tame 50.wav
```

Delay between the files doesn't matter, because it's measured. Leave the plug-in's Gain at 0 dB and everything you aren't testing at its default. Check that the plug-in is really processing during each bounce: a render identical to its original gets reported and left out.

## Fit and score

Build the tools (`cmake -S . -B build -DCOLGATE_BUILD_PLUGIN=OFF && cmake --build build`), install numpy and scipy, then:

```sh
# Fit, and print new constants for source/engine/PerceptualField.cpp
python3 tools/calibration/calibrate.py "ref 1" "ref 2" "ref 3" --build build/tests

# Compare the engine's actual audio with every render
python3 tools/calibration/calibrate.py "ref 1" "ref 2" "ref 3" --build build/tests --score
```

Fitting first scores each song with constants fitted only on the others, so those scores are a fair guess at how a new song would do. Then it refits on everything for the constants. Recover and Tame set apart are part of the fit, and so is Boost, with one curve fitted for each mode.

## What more renders would answer

Today's numbers come from three songs: 26, 12 and 12 seconds. These would help most, in this order:

1. Boost at other values, such as +10, -15 and -30, on its own. Only +30 has been measured, and always with 15/15.
2. Recover and Tame apart with Bias, such as 50/0 with Bias +50 and 50/20 with Bias -50. Bias has never been measured with them apart.
3. Recover and Tame at 100 and 200. Nothing above 50 has been measured.
4. Negative Brighten at 50/50, such as -50, and Brighten +100. So far the only negative value is -15.
5. Single sources rather than full mixes, such as one guitar track and one iPhone vocal. Sparse material is the least tested.
