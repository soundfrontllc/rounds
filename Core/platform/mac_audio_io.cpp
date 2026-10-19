// Core Audio HAL I/O for macOS: one AUHAL unit running input and output on
// the same device, with the engine called directly from the render callback.

#ifdef __APPLE__

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "rounds_core.h"

namespace {

AudioObjectPropertyAddress address(AudioObjectPropertySelector selector,
                                   AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return AudioObjectPropertyAddress{selector, scope, kAudioObjectPropertyElementMain};
}

template <typename T>
T deviceProperty(AudioObjectID object, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope,
                 T fallback) {
    AudioObjectPropertyAddress addr = address(selector, scope);
    T value = fallback;
    UInt32 size = sizeof(T);
    if (AudioObjectGetPropertyData(object, &addr, 0, nullptr, &size, &value) != noErr) return fallback;
    return value;
}

int32_t channelCount(AudioDeviceID device, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress addr = address(kAudioDevicePropertyStreamConfiguration, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &addr, 0, nullptr, &size) != noErr || size == 0) return 0;
    std::vector<uint8_t> storage(size);
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, list) != noErr) return 0;
    int32_t channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; i++) channels += static_cast<int32_t>(list->mBuffers[i].mNumberChannels);
    return channels;
}

std::vector<AudioDeviceID> allDevices() {
    AudioObjectPropertyAddress addr = address(kAudioHardwarePropertyDevices);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr) return {};
    std::vector<AudioDeviceID> ids(size / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, ids.data()) != noErr) return {};
    return ids;
}

// Device latency + safety offset + first stream's latency, for one direction.
int32_t directionLatency(AudioDeviceID device, AudioObjectPropertyScope scope) {
    UInt32 total = deviceProperty<UInt32>(device, kAudioDevicePropertyLatency, scope, 0);
    total += deviceProperty<UInt32>(device, kAudioDevicePropertySafetyOffset, scope, 0);
    AudioObjectPropertyAddress addr = address(kAudioDevicePropertyStreams, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &addr, 0, nullptr, &size) == noErr && size >= sizeof(AudioStreamID)) {
        std::vector<AudioStreamID> streams(size / sizeof(AudioStreamID));
        if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, streams.data()) == noErr) {
            total += deviceProperty<UInt32>(streams[0], kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal, 0);
        }
    }
    return static_cast<int32_t>(total);
}

void setError(char* error, int32_t length, const char* message, OSStatus status) {
    if (!error || length <= 0) return;
    std::snprintf(error, static_cast<size_t>(length), "%s (OSStatus %d)", message, static_cast<int>(status));
}

double ticksPerSecond() {
    mach_timebase_info_data_t info;
    mach_timebase_info(&info);
    return 1e9 * static_cast<double>(info.denom) / static_cast<double>(info.numer);
}

}  // namespace

struct RoundsAudioIO {
    AudioUnit unit = nullptr;
    RoundsEngine* engine = nullptr;
    AudioBufferList* inputList = nullptr;
    std::vector<std::vector<float>> inputBuffers;
    std::vector<float> silence;
    int32_t inputChannel = 0;
    UInt32 maxFrames = 0;
    double hostTicksPerSample = 0.0;
};

static OSStatus renderCallback(void* refCon, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* timeStamp,
                               UInt32 /*bus*/, UInt32 frames, AudioBufferList* ioData) {
    auto* io = static_cast<RoundsAudioIO*>(refCon);
    frames = std::min(frames, io->maxFrames);
    for (UInt32 i = 0; i < io->inputList->mNumberBuffers; i++) {
        io->inputList->mBuffers[i].mDataByteSize = frames * sizeof(float);
    }
    const OSStatus status = AudioUnitRender(io->unit, flags, timeStamp, 1, frames, io->inputList);
    const float* input = io->silence.data();
    if (status == noErr && io->inputChannel < static_cast<int32_t>(io->inputList->mNumberBuffers)) {
        input = static_cast<const float*>(io->inputList->mBuffers[io->inputChannel].mData);
    }
    float* left = static_cast<float*>(ioData->mBuffers[0].mData);
    float* right = ioData->mNumberBuffers > 1 ? static_cast<float*>(ioData->mBuffers[1].mData) : nullptr;
    rounds_engine_process_timed(io->engine, input, left, right, static_cast<int32_t>(frames), timeStamp->mHostTime,
                                io->hostTicksPerSample);
    for (UInt32 i = 2; i < ioData->mNumberBuffers; i++) std::memset(ioData->mBuffers[i].mData, 0, frames * sizeof(float));
    return noErr;
}

extern "C" {

int32_t rounds_audio_device_count(void) { return static_cast<int32_t>(allDevices().size()); }

int32_t rounds_audio_device_info(int32_t index, RoundsAudioDevice* out) {
    const std::vector<AudioDeviceID> ids = allDevices();
    if (index < 0 || index >= static_cast<int32_t>(ids.size())) return 0;
    const AudioDeviceID device = ids[static_cast<size_t>(index)];
    std::memset(out, 0, sizeof(*out));
    out->deviceID = device;
    CFStringRef name = nullptr;
    AudioObjectPropertyAddress addr = address(kAudioObjectPropertyName);
    UInt32 size = sizeof(name);
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &name) == noErr && name) {
        CFStringGetCString(name, out->name, sizeof(out->name), kCFStringEncodingUTF8);
        CFRelease(name);
    }
    out->inputChannels = channelCount(device, kAudioObjectPropertyScopeInput);
    out->outputChannels = channelCount(device, kAudioObjectPropertyScopeOutput);
    out->sampleRate = deviceProperty<Float64>(device, kAudioDevicePropertyNominalSampleRate,
                                              kAudioObjectPropertyScopeGlobal, 0.0);
    return 1;
}

uint32_t rounds_audio_default_input_device(void) {
    return deviceProperty<AudioDeviceID>(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultInputDevice,
                                         kAudioObjectPropertyScopeGlobal, kAudioObjectUnknown);
}

RoundsAudioIO* rounds_audio_open(uint32_t deviceID, int32_t inputChannel, int32_t bufferFrames, double maxLoopSeconds,
                                 RoundsAudioInfo* info, char* error, int32_t errorLength) {
    const int32_t inputs = channelCount(deviceID, kAudioObjectPropertyScopeInput);
    const int32_t outputs = channelCount(deviceID, kAudioObjectPropertyScopeOutput);
    if (inputs == 0 || outputs == 0) {
        setError(error, errorLength,
                 "This device needs both inputs and outputs. Pick your audio interface, or make an aggregate device "
                 "in Audio MIDI Setup",
                 0);
        return nullptr;
    }
    if (inputChannel < 0 || inputChannel >= inputs) inputChannel = 0;

    AudioComponentDescription desc{kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    if (!component) {
        setError(error, errorLength, "Couldn't find the HAL output unit", 0);
        return nullptr;
    }

    auto* io = new RoundsAudioIO();
    io->inputChannel = inputChannel;
    OSStatus status = AudioComponentInstanceNew(component, &io->unit);
    if (status != noErr) {
        setError(error, errorLength, "Couldn't create the audio unit", status);
        delete io;
        return nullptr;
    }

    auto fail = [&](const char* message) {
        setError(error, errorLength, message, status);
        rounds_audio_close(io);
        return static_cast<RoundsAudioIO*>(nullptr);
    };

    UInt32 one = 1;
    status = AudioUnitSetProperty(io->unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &one, sizeof(one));
    if (status != noErr) return fail("Couldn't enable input");
    status = AudioUnitSetProperty(io->unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &one, sizeof(one));
    if (status != noErr) return fail("Couldn't enable output");
    AudioDeviceID device = deviceID;
    status = AudioUnitSetProperty(io->unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device,
                                  sizeof(device));
    if (status != noErr) return fail("Couldn't select the device");

    if (bufferFrames > 0) {
        AudioObjectPropertyAddress addr = address(kAudioDevicePropertyBufferFrameSize);
        UInt32 frames = static_cast<UInt32>(bufferFrames);
        AudioObjectSetPropertyData(device, &addr, 0, nullptr, sizeof(frames), &frames);  // best effort
    }

    const Float64 sampleRate =
        deviceProperty<Float64>(device, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, 48000.0);

    AudioStreamBasicDescription format{};
    format.mSampleRate = sampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    format.mBytesPerPacket = sizeof(float);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float);
    format.mBitsPerChannel = 32;

    format.mChannelsPerFrame = static_cast<UInt32>(inputs);
    status = AudioUnitSetProperty(io->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &format,
                                  sizeof(format));
    if (status != noErr) return fail("Couldn't set the input format");
    format.mChannelsPerFrame = 2;
    status = AudioUnitSetProperty(io->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format,
                                  sizeof(format));
    if (status != noErr) return fail("Couldn't set the output format");

    UInt32 maxFrames = 4096;
    UInt32 size = sizeof(maxFrames);
    AudioUnitSetProperty(io->unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, size);
    AudioUnitGetProperty(io->unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, &size);
    io->maxFrames = maxFrames;

    const size_t listSize = offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer) * static_cast<size_t>(inputs);
    io->inputList = static_cast<AudioBufferList*>(std::calloc(1, listSize));
    io->inputList->mNumberBuffers = static_cast<UInt32>(inputs);
    io->inputBuffers.resize(static_cast<size_t>(inputs));
    for (int32_t i = 0; i < inputs; i++) {
        io->inputBuffers[static_cast<size_t>(i)].assign(maxFrames, 0.0f);
        io->inputList->mBuffers[i].mNumberChannels = 1;
        io->inputList->mBuffers[i].mDataByteSize = maxFrames * sizeof(float);
        io->inputList->mBuffers[i].mData = io->inputBuffers[static_cast<size_t>(i)].data();
    }
    io->silence.assign(maxFrames, 0.0f);

    io->engine = rounds_engine_create(sampleRate, maxLoopSeconds, static_cast<int32_t>(maxFrames));
    io->hostTicksPerSample = ticksPerSecond() / sampleRate;

    AURenderCallbackStruct callback{renderCallback, io};
    status = AudioUnitSetProperty(io->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback,
                                  sizeof(callback));
    if (status != noErr) return fail("Couldn't set the render callback");

    status = AudioUnitInitialize(io->unit);
    if (status != noErr) return fail("Couldn't start the audio unit");

    const UInt32 buffer = deviceProperty<UInt32>(device, kAudioDevicePropertyBufferFrameSize,
                                                 kAudioObjectPropertyScopeGlobal, 512);
    const int32_t inputLatency = directionLatency(device, kAudioObjectPropertyScopeInput) + static_cast<int32_t>(buffer);
    const int32_t outputLatency = directionLatency(device, kAudioObjectPropertyScopeOutput) + static_cast<int32_t>(buffer);
    rounds_engine_set_latency(io->engine, inputLatency, outputLatency);

    if (info) {
        info->sampleRate = sampleRate;
        info->bufferFrames = static_cast<int32_t>(buffer);
        info->inputLatency = inputLatency;
        info->outputLatency = outputLatency;
    }
    return io;
}

RoundsEngine* rounds_audio_engine(RoundsAudioIO* io) { return io ? io->engine : nullptr; }

int32_t rounds_audio_start(RoundsAudioIO* io) { return io && AudioOutputUnitStart(io->unit) == noErr ? 1 : 0; }

void rounds_audio_stop(RoundsAudioIO* io) {
    if (io && io->unit) AudioOutputUnitStop(io->unit);
}

void rounds_audio_close(RoundsAudioIO* io) {
    if (!io) return;
    if (io->unit) {
        AudioOutputUnitStop(io->unit);
        AudioUnitUninitialize(io->unit);
        AudioComponentInstanceDispose(io->unit);
    }
    if (io->engine) rounds_engine_destroy(io->engine);
    std::free(io->inputList);
    delete io;
}

uint64_t rounds_host_time_now(void) { return mach_absolute_time(); }

double rounds_host_time_to_seconds(uint64_t hostTime) {
    static const double perSecond = ticksPerSecond();
    return static_cast<double>(hostTime) / perSecond;
}

}  // extern "C"

#endif  // __APPLE__
