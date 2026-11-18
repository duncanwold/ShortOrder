# Third-party notices

ShortOrder's own code is MIT-licensed (see `LICENSE`). It is built with, or ships with, the following third-party components.

## JUCE 8

- **What:** the framework ShortOrder is built on (the plugin wrappers, the editor, `juce::dsp::FFT` in the pitch detector).
- **Version:** 8.0.12.
- **Licence:** used under the JUCE licence (https://juce.com/legal/juce-8-licence/).
- **Building from source:** you need your own JUCE licence, or you must use JUCE under the GNU AGPLv3 (https://www.gnu.org/licenses/agpl-3.0.en.html). The MIT licence on ShortOrder's code doesn't grant you a JUCE licence.
- **JUCE's own dependencies:** JUCE bundles third-party code, some of which is compiled into the plug-in binaries (among them zlib, pnglib, jpeglib, HarfBuzz, FLAC and Ogg Vorbis, under the zlib, IJG, Old MIT and BSD licences). Their licences are listed in JUCE's `LICENSE.md` and kept beside each library in the JUCE source tree.

## VST3 SDK (Steinberg)

- **What:** the VST3 plug-in interfaces, as bundled with JUCE.
- **Licence:** MIT (Steinberg Media Technologies GmbH; see `LICENSE.txt` in the SDK).
- VST is a registered trademark of Steinberg Media Technologies GmbH.

## AAX SDK (Avid)

- **What:** the AAX plug-in interfaces, for the Pro Tools version.
- **Licence:** Avid's AAX SDK licence. It is **not included** in this repository.
- **Building AAX:** you need your own Avid developer account to download the SDK, and a PACE (iLok) signing account to sign an AAX plug-in that Pro Tools will load. Without the SDK at `~/AAX_SDK`, the build simply leaves AAX out.
- Pro Tools and AAX are trademarks of Avid Technology, Inc.

## Didact Gothic (font)

- **What:** the faceplate's typeface, `Resources/Fonts/DidactGothic-Regular.ttf`, embedded in the plug-in.
- **Copyright:** Copyright (c) 2009, 2010, 2011 Daniel Johnson (as stated in `OFL.txt`).
- **Licence:** SIL Open Font License 1.1; the full text is in `Resources/Fonts/OFL.txt` (also shipped as `OFL.txt` beside this file in the installer and the zips).

## Published methods (no third-party code)

ShortOrder implements these from their published descriptions. No code was copied from anyone's implementation.

- **YIN pitch detection:** A. de Cheveigné and H. Kawahara, "YIN, a fundamental frequency estimator for speech and music", JASA 111(4), 2002.
- **PolyBLEP and PolyBLAMP band-limiting:** the oscillators' anti-aliasing (V. Välimäki et al.; J. Pekonen et al.).
- **ITU-R BS.1770 loudness:** used in the tests to match the waves' levels.
- **Karplus–Strong plucked string:** used in the tests and tools as a synthetic guitar.
