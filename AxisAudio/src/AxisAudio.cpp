#include "AxisAudio.h"
#include <stdio.h>
#include <stdlib.h>
#include <iostream>

#include <string>
#include <thread>
#include <filesystem>

#include "AL/al.h"
#include "AL/alext.h"
#include "alc/alcmain.h"
#include "alhelpers.h"

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "minimp3_ex.h"

#include "vorbis/codec.h"
#include "vorbis/vorbisfile.h"

namespace Axis {

	static ALCdevice* s_AudioDevice = nullptr;
	static mp3dec_t s_Mp3d;

	static uint8_t* s_AudioScratchBuffer;
	static uint32_t s_AudioScratchBufferSize = 10 * 1024 * 1024; // 10mb initially

	static bool s_DebugLog = true;

#define AXIS_LOG(x) std::cout << "[Axis Audio]  " << x << std::endl

	// Currently supported file formats
	enum class AudioFileFormat
	{
		None = 0,
		Ogg,
		MP3,
		Wav
	};

	static bool IsBigEndian()
	{
		int32_t a = 1;
		return !((char*)&a)[0];
	}

	static int32_t ConvertToInt(char* buffer, int32_t len)
	{
		int a = 0;
		if (!IsBigEndian())
			for (int32_t i = 0; i < len; i++)
				((char*)&a)[i] = buffer[i];
		else
			for (int32_t i = 0; i < len; i++)
				((char*)&a)[3 - i] = buffer[i];
		return a;
	}

	int32_t FileReadInt32(char* buffer, FILE* file)
	{
		int r = fread(buffer, sizeof(char), 4, file);
		return ConvertToInt(buffer, 4);
	}

	int32_t FileReadInt16(char* buffer, FILE* file)
	{
		int r = fread(buffer, sizeof(char), 2, file);
		return ConvertToInt(buffer, 2);
	}

	static AudioFileFormat GetFileFormat(const std::string& filename)
	{
		std::filesystem::path path = filename;
		std::string extension = path.extension().string();

		if (extension == ".ogg")  return AudioFileFormat::Ogg;
		if (extension == ".mp3")  return AudioFileFormat::MP3;
		if (extension == ".wav")  return AudioFileFormat::Wav;
		
		return AudioFileFormat::None;
	}

	static ALenum GetOpenALFormat(uint32_t channels)
	{
		// Note: sample size is always 2 bytes (16-bits) with
		// both the .mp3 and .ogg decoders that we're using
		switch (channels)
		{
			case 1:  return AL_FORMAT_MONO16;
			case 2:  return AL_FORMAT_STEREO16;
		}
		// assert
		return 0;
	}

	AudioSource Audio::LoadAudioSourceOgg(const std::string& filename)
	{
		FILE* f = fopen(filename.c_str(), "rb");

		OggVorbis_File vf;
		if (ov_open_callbacks(f, &vf, NULL, 0, OV_CALLBACKS_NOCLOSE) < 0)
			std::cout << "Could not open ogg stream\n";

		// Useful info
		vorbis_info* vi = ov_info(&vf, -1);
		auto sampleRate = vi->rate;
		auto channels = vi->channels;
		auto alFormat = GetOpenALFormat(channels);

		uint64_t samples = ov_pcm_total(&vf, -1);
		float trackLength = (float)samples / (float)sampleRate; // in seconds
		uint32_t bufferSize = 2 * channels * samples; // 2 bytes per sample (I'm guessing...)

		if (s_DebugLog)
		{
			AXIS_LOG("File Info - " << filename << ":");
			AXIS_LOG("  Channels: " << channels);
			AXIS_LOG("  Sample Rate: " << sampleRate);
			AXIS_LOG("  Expected size: " << bufferSize);
		}

		// TODO: Replace with Axis::Buffer
		if (s_AudioScratchBufferSize < bufferSize)
		{
			s_AudioScratchBufferSize = bufferSize;
			delete[] s_AudioScratchBuffer;
			s_AudioScratchBuffer = new uint8_t[s_AudioScratchBufferSize];
		}

		uint8_t* oggBuffer = s_AudioScratchBuffer;
		uint8_t* bufferPtr = oggBuffer;
		int eof = 0;
		while (!eof)
		{
			int current_section;
			long length = ov_read(&vf, (char*)bufferPtr, 4096, 0, 2, 1, &current_section);
			bufferPtr += length;
			if (length == 0)
			{
				eof = 1;
			}
			else if (length < 0)
			{
				if (length == OV_EBADLINK)
				{
					fprintf(stderr, "Corrupt bitstream section! Exiting.\n");
					exit(1);
				}
			}
		}

		uint32_t size = bufferPtr - oggBuffer;
		// assert bufferSize == size

		if (s_DebugLog)
			AXIS_LOG("  Read " << size << " bytes");

		// Release file
		ov_clear(&vf);
		fclose(f);

		ALuint buffer;
		alGenBuffers(1, &buffer);
		alBufferData(buffer, alFormat, oggBuffer, size, sampleRate);

		AudioSource result = { buffer, true, trackLength };
		alGenSources(1, &result.m_SourceHandle);
		alSourcei(result.m_SourceHandle, AL_BUFFER, buffer);

		if (alGetError() != AL_NO_ERROR)
			printf("Failed to setup sound source %s\n", filename.c_str());

		return result;
	}

	AudioSource Audio::LoadAudioSourceMP3(const std::string& filename)
	{
		mp3dec_file_info_t info;
		int loadResult = mp3dec_load(&s_Mp3d, filename.c_str(), &info, NULL, NULL);
		uint32_t size = info.samples * sizeof(mp3d_sample_t);

		auto sampleRate = info.hz;
		auto channels = info.channels;
		auto alFormat = GetOpenALFormat(channels);
		float lengthSeconds = size / (info.avg_bitrate_kbps * 1024.0f);

		ALuint buffer;
		alGenBuffers(1, &buffer);
		alBufferData(buffer, alFormat, info.buffer, size, sampleRate);

		AudioSource result = { buffer, true, lengthSeconds };
		alGenSources(1, &result.m_SourceHandle);
		alSourcei(result.m_SourceHandle, AL_BUFFER, buffer);

		if (s_DebugLog)
		{
			AXIS_LOG("File Info - " << filename << ":");
			AXIS_LOG("  Channels: " << channels);
			AXIS_LOG("  Sample Rate: " << sampleRate);
			AXIS_LOG("  Size: " << size << " bytes");

			auto [mins, secs] = result.GetLengthMinutesAndSeconds();
			AXIS_LOG("  Length: " << mins << "m" << secs << "s");
		}

		if (alGetError() != AL_NO_ERROR)
			printf("Failed to setup sound source %s\n", filename.c_str());

		return result;
	}

	AudioSource Audio::LoadAudioSourceWav(const std::string& filename)
	{
		char readBuffer[5];
		readBuffer[4] = '\0';

		FILE* file = fopen(filename.c_str(), "rb");
		memset(readBuffer, 0, sizeof(readBuffer));
		if (fread(readBuffer, sizeof(char), 4, file) != 4 || strcmp(readBuffer, "RIFF") != 0)
			std::cout << "this is not a valid WAVE file - RIFF missing" << std::endl;
		auto totalByteSize = FileReadInt32(readBuffer, file);
		if (fread(readBuffer, sizeof(char), 4, file) != 4 || strcmp(readBuffer, "WAVE") != 0)
			std::cout << "this is not a valid WAVE file - WAVE missing" << std::endl;
		if (fread(readBuffer, sizeof(char), 4, file) != 4 || strcmp(readBuffer, "fmt ") != 0)
			std::cout << "this is not a valid WAVE file - fmt missing" << std::endl;
		fread(readBuffer, sizeof(char), 4, file);
		fread(readBuffer, sizeof(char), 2, file);
		auto channels = FileReadInt16(readBuffer, file);
		auto sampleRate = FileReadInt32(readBuffer, file);
		auto byteRate = FileReadInt32(readBuffer, file);
		fread(readBuffer, sizeof(char), 2, file);
		auto bitsPerSample = FileReadInt16(readBuffer, file);
		fread(readBuffer, sizeof(char), 4, file);
		fread(readBuffer, sizeof(char), 4, file);

		auto size = totalByteSize - 36; // 36 is size of WAVE file header in bytes
		char* data = new char[size];
		fread(data, sizeof(char), size, file);
		
		float lengthSeconds = float(byteRate) / (float)sampleRate;

		ALuint buffer, format;
		alGenBuffers(1, &buffer);
		if (channels == 1)
		{
			if (bitsPerSample == 8)
				format = AL_FORMAT_MONO8;
			else
				format = AL_FORMAT_MONO16;
		}
		else {
			if (bitsPerSample == 8)
				format = AL_FORMAT_STEREO8;
			else
				format = AL_FORMAT_STEREO16;
		}
		alBufferData(buffer, format, data, size, sampleRate);

		AudioSource result = { buffer, true, lengthSeconds };
		alGenSources(1, &result.m_SourceHandle);
		alSourcei(result.m_SourceHandle, AL_BUFFER, buffer);

		free(data);
		fclose(file);

		if (s_DebugLog)
		{
			AXIS_LOG("File Info - " << filename << ":");
			AXIS_LOG("  Channels: " << channels);
			AXIS_LOG("  Sample Rate: " << sampleRate);
			AXIS_LOG("  Size: " << size << " bytes");

			auto [mins, secs] = result.GetLengthMinutesAndSeconds();
			AXIS_LOG("  Length: " << mins << "m" << secs << "s");
		}

		if (alGetError() != AL_NO_ERROR)
			printf("Failed to setup sound source %s\n", filename.c_str());

		return result;
	}

	static void PrintAudioDeviceInfo()
	{
		if (s_DebugLog)
		{
			AXIS_LOG("Audio Device Info:");
			AXIS_LOG("  Name: " << s_AudioDevice->DeviceName);
			AXIS_LOG("  Sample Rate: " << s_AudioDevice->Frequency);
			AXIS_LOG("  Max Sources: " << s_AudioDevice->SourcesMax);
			AXIS_LOG("    Mono: " << s_AudioDevice->NumMonoSources);
			AXIS_LOG("    Stereo: " << s_AudioDevice->NumStereoSources);
		}
	}

	void Audio::Init()
	{
		if (InitAL(s_AudioDevice, nullptr, 0) != 0)
			std::cout << "Audio device error!\n";

		PrintAudioDeviceInfo();

		mp3dec_init(&s_Mp3d);

		s_AudioScratchBuffer = new uint8_t[s_AudioScratchBufferSize];

		// Init listener
		ALfloat listenerPos[] = { 0.0,0.0,0.0 };
		ALfloat listenerVel[] = { 0.0,0.0,0.0 };
		ALfloat listenerOri[] = { 0.0,0.0,-1.0, 0.0,1.0,0.0 };
		alListenerfv(AL_POSITION, listenerPos);
		alListenerfv(AL_VELOCITY, listenerVel);
		alListenerfv(AL_ORIENTATION, listenerOri);
	}

	AudioSource Audio::LoadAudioSource(const std::string& filename)
	{
		auto format = GetFileFormat(filename);
		switch (format)
		{
			case AudioFileFormat::Ogg:  return LoadAudioSourceOgg(filename);
			case AudioFileFormat::MP3:  return LoadAudioSourceMP3(filename);
			case AudioFileFormat::Wav:  return LoadAudioSourceWav(filename);
		}

		// Loading failed or unsupported file type
		return { 0, false, 0 };
	}
	
	void Audio::Play(const AudioSource& audioSource)
	{
		// Play the sound until it finishes
		alSourcePlay(audioSource.m_SourceHandle);

		// TODO: current playback time and playback finished callback
		// eg.
		// ALfloat offset;
		// alGetSourcei(audioSource.m_SourceHandle, AL_SOURCE_STATE, &s_PlayState);
		// ALenum s_PlayState;
		// alGetSourcef(audioSource.m_SourceHandle, AL_SEC_OFFSET, &offset);
	}

	void Audio::SetDebugLogging(bool log)
	{
		s_DebugLog = log;
	}

	AudioSource::AudioSource(uint32_t handle, bool loaded, float length)
		: m_BufferHandle(handle), m_Loaded(loaded), m_TotalDuration(length)
	{
	}

	AudioSource::~AudioSource()
	{
		// TODO: free openal audio source?
	}

	void AudioSource::SetPosition(float x, float y, float z)
	{
		//alSource3f(source, AL_VELOCITY, 0, 0, 0);

		m_Position[0] = x;
		m_Position[1] = y;
		m_Position[2] = z;

		alSourcefv(m_SourceHandle, AL_POSITION, m_Position);
	}

	void AudioSource::SetGain(float gain)
	{
		m_Gain = gain;

		alSourcef(m_SourceHandle, AL_GAIN, gain);
	}

	void AudioSource::SetPitch(float pitch)
	{
		m_Pitch = pitch;

		alSourcef(m_SourceHandle, AL_PITCH, pitch);
	}

	void AudioSource::SetSpatial(bool spatial)
	{
		m_Spatial = spatial;

		alSourcei(m_SourceHandle, AL_SOURCE_SPATIALIZE_SOFT, spatial ? AL_TRUE : AL_FALSE);
		alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
	}

	void AudioSource::SetLoop(bool loop)
	{
		m_Loop = loop;

		alSourcei(m_SourceHandle, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);
	}

	std::pair<uint32_t, uint32_t> AudioSource::GetLengthMinutesAndSeconds() const
	{
		return { (uint32_t)(m_TotalDuration / 60.0f), (uint32_t)m_TotalDuration % 60 };
	}

	AudioSource AudioSource::LoadFromFile(const std::string& file, bool spatial)
	{
		AudioSource result = Audio::LoadAudioSource(file);
		result.SetSpatial(spatial);
		return result;
	}

}
