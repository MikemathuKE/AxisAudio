# AxisAudio

Axis Audio is an audio library written for the [Axis Engine](https://github.com/MikemathuKE/Axis.git), built on top of [OpenAL Soft](https://openal-soft.org/).
This mimics [HazelAudio](https://github.com/TheCherno/HazelAudio.git) written by theCherno.

## Currently Supports
- .ogg files
- .mp3 files
- .wav files
- 3D spatial playback of audio sources

## TODO
- Unload audio sources

## Example
Check out the `AxisAudio-Examples` project for more, but it's super simple:
```cpp
// Initialize the audio engine
Axis::Audio::Init();
// Load audio source from file, bool is for whether the source
// should be in 3D space or not
auto source = Axis::AudioSource::LoadFromFile(filename, true);
// Play audio source
Axis::Audio::Play(source);
```
and you can set various attributes on a source as well:
```cpp
source.SetPosition(x, y, z);
source.SetGain(2.0f);
source.SetLoop(true);
```

## Acknowledgements
- [OpenAL Soft](https://openal-soft.org/)
- [minimp3](https://github.com/lieff/minimp3)
- [libogg and Vorbis](https://www.xiph.org/)