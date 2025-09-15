# fmod-dsp-introspector
This is a small tool I quickly wrote up which allows you to dump DSP parameter information about any FMOD built-in DSP or DSPs in a plug-in. It doesn't support codecs or outputs, although that wouldn't be super hard to add.

The tool takes as input either a built-in DSP name, or `-p` can be used to indicate that all future arguments are plug-ins to be loaded. The binary is just called `dspinfo`; I figured that `fmod-dsp-introspector` was too lengthy for something like a CLI tool.

Currently vcpkg is required to build (it requires `stringzilla`). I may fully integrate the single-header version of this library if people want a completely self-contained app. Obviously, you also need FMOD installed. At this time, this is windows-only, since the library paths are hard-coded (as xmake lacks the ability to read the windows registry). However, this may change in the future.

I'd appreciate any contributions! As noted previously, this was something I just quickly wrote up for my own usage and decided to publish it, so it's quite bear-bones. But it's quite useful even for how minimal it is, and getting it to build on other platforms shouldn't be overly difficult to do.