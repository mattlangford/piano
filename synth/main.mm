#import <AVFAudio/AVFAudio.h>

#include "midi.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

enum class Waveform {
  kTriangle = 0,
  kSquare = 1,
  kSine = 2,
  kFile = 3,
};

std::atomic<bool> g_running = true;

void handle_signal(int) {
  g_running = false;
}

std::string describe_error(const char* stage, NSError* error) {
  std::string message = stage;
  if (error == nil) {
    return message + ": unknown AVFAudio error";
  }
  message += ": " + std::string([[error localizedDescription] UTF8String]);
  message += " (domain=" + std::string([[error domain] UTF8String]) +
             ", code=" + std::to_string(error.code) + ")";
  return message;
}

class SampleSynth {
 public:
  static constexpr std::size_t kVoiceCount = 32;
  static constexpr float kAttackSeconds = 0.005f;
  // Apply the same final gain to files and synthesized waveforms. The source
  // material is loudness-normalized before this gain is applied.
  static constexpr float kVoiceGain = 0.70f;
  static constexpr float kFallbackReferenceRms = 0.50f;
  static constexpr float kReleaseSeconds = 0.12f;

  SampleSynth() = default;
  SampleSynth(const SampleSynth&) = delete;
  SampleSynth& operator=(const SampleSynth&) = delete;

  ~SampleSynth() {
    [engine_ stop];
  }

  bool start(const std::vector<std::vector<std::string>>& sample_groups,
             int root_note,
             std::string* error) {
    root_note_ = root_note;
    sample_count_ = sample_groups.size();
    if (sample_count_ == 0 || sample_count_ > 128) {
      *error = "the number of keyboard zones must be between 1 and 128";
      return false;
    }

    format_ = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:44100.0
                                                               channels:2];
    if (format_ == nil) {
      *error = "unable to create audio format";
      return false;
    }

    NSError* ns_error = nil;
    sample_waveforms_.reserve(128);
    clips_.reserve(128);
    zone_assets_.resize(sample_count_);
    for (std::size_t zone = 0; zone < sample_groups.size(); ++zone) {
      if (sample_groups[zone].empty()) {
        *error = "each keyboard zone must contain at least one sample";
        return false;
      }
      for (const std::string& path : sample_groups[zone]) {
        const std::size_t asset = sample_waveforms_.size();
        zone_assets_[zone].push_back(asset);
        if (path.empty()) {
          sample_waveforms_.push_back(Waveform::kTriangle);
          clips_.push_back(nil);
          continue;
        }
        if (path == "square") {
          sample_waveforms_.push_back(Waveform::kSquare);
          clips_.push_back(nil);
          continue;
        }
        if (path == "sine" || path == "sin") {
          sample_waveforms_.push_back(Waveform::kSine);
          clips_.push_back(nil);
          continue;
        }

        sample_waveforms_.push_back(Waveform::kFile);
        AVAudioPCMBuffer* clip = load_clip(path, &ns_error);
        if (clip == nil) {
          *error = describe_error(("loading sample " + path).c_str(), ns_error);
          return false;
        }
        clips_.push_back(clip);
      }
    }
    normalize_file_clips();

    engine_ = [[AVAudioEngine alloc] init];
    if (engine_ == nil) {
      *error = "unable to create AVAudio engine";
      return false;
    }

    has_synth_waveforms_ = std::any_of(sample_waveforms_.begin(), sample_waveforms_.end(),
                                        [](Waveform value) { return value != Waveform::kFile; });
    has_file_samples_ = std::any_of(sample_waveforms_.begin(), sample_waveforms_.end(),
                                    [](Waveform value) { return value == Waveform::kFile; });

    if (has_synth_waveforms_) {
      source_ = [[AVAudioSourceNode alloc] initWithFormat:format_
                                             renderBlock:^OSStatus(
                                                 BOOL* is_silence,
                                                 const AudioTimeStamp* /*timestamp*/,
                                                 AVAudioFrameCount frame_count,
                                                 AudioBufferList* output_data) {
        *is_silence = NO;
        render_triangle(frame_count, output_data);
        return noErr;
      }];
      if (source_ == nil) {
        *error = "unable to create triangle audio source";
        return false;
      }
      [engine_ attachNode:source_];
      [engine_ connect:source_ to:engine_.mainMixerNode format:format_];
    }

    if (has_file_samples_) {
      sample_mixer_ = [[AVAudioMixerNode alloc] init];
      reverb_ = [[AVAudioUnitReverb alloc] init];
      if (sample_mixer_ == nil || reverb_ == nil) {
        *error = "unable to create sample effects graph";
        return false;
      }
      [reverb_ loadFactoryPreset:AVAudioUnitReverbPresetMediumHall];
      reverb_.wetDryMix = 35.0f;
      [engine_ attachNode:sample_mixer_];
      [engine_ attachNode:reverb_];
      for (Voice& voice : voices_) {
        voice.player = [[AVAudioPlayerNode alloc] init];
        voice.time_pitch = [[AVAudioUnitTimePitch alloc] init];
        if (voice.player == nil || voice.time_pitch == nil) {
          *error = "unable to create sample voice";
          return false;
        }
        [engine_ attachNode:voice.player];
        [engine_ attachNode:voice.time_pitch];
        [engine_ connect:voice.player to:voice.time_pitch format:format_];
        [engine_ connect:voice.time_pitch to:sample_mixer_ format:format_];
      }
      [engine_ connect:sample_mixer_ to:reverb_ format:format_];
      [engine_ connect:reverb_ to:engine_.mainMixerNode format:format_];
    }

    if (![engine_ startAndReturnError:&ns_error]) {
      *error = describe_error("starting AVAudio engine", ns_error);
      return false;
    }
    return true;
  }

  void note_on(int midi_note, int velocity) {
    if (midi_note < 0 || midi_note > 127) return;

    Voice& voice = voices_[next_voice_++ % kVoiceCount];
    const std::size_t zone = zone_index_for_note(midi_note);
    const auto& assets = zone_assets_[zone];
    std::uniform_int_distribution<std::size_t> pick(0, assets.size() - 1);
    const std::size_t sample_index = assets[pick(random_)];
    // Each successive keyboard zone is centered one octave lower. Thus the
    // second/upper zone transposes C4 to C3, while the first zone is unchanged.
    const int pitch_note = midi_note - static_cast<int>(12 * zone);
    voice.note.store(midi_note, std::memory_order_relaxed);
    voice.pitch_note.store(pitch_note, std::memory_order_relaxed);
    // MIDI note-on velocity is 1-127. Use a gentle response curve so quiet
    // playing remains audible while hard strikes still sound stronger.
    const float normalized_velocity = std::clamp(velocity, 1, 127) / 127.0f;
    const float velocity_gain = std::sqrt(normalized_velocity);
    voice.amplitude.store(velocity_gain, std::memory_order_relaxed);
    voice.waveform.store(static_cast<int>(sample_waveforms_[sample_index]),
                         std::memory_order_release);
    voice.key_held.store(true, std::memory_order_release);
    voice.gate.store(true, std::memory_order_release);
    voice.generation.fetch_add(1, std::memory_order_release);

    if (sample_waveforms_[sample_index] == Waveform::kFile) {
      [voice.player stop];
      voice.player.volume = voice.amplitude.load(std::memory_order_relaxed) * kVoiceGain;
      // TimePitch changes pitch in cents while preserving the clip duration.
      voice.time_pitch.pitch = static_cast<float>(100.0 * (pitch_note - root_note_));
      [voice.player scheduleBuffer:clips_[sample_index] completionHandler:nil];
      [voice.player play];
    }
  }

  void set_sustain(bool sustain) {
    sustain_.store(sustain, std::memory_order_release);
    if (!sustain) {
      for (Voice& voice : voices_) {
        if (voice.deferred_note_off.load(std::memory_order_acquire) &&
            !voice.key_held.load(std::memory_order_acquire)) {
          voice.gate.store(false, std::memory_order_release);
          if (static_cast<Waveform>(voice.waveform.load(std::memory_order_acquire)) ==
                  Waveform::kFile &&
              voice.player != nil) {
            // A pedal-held file clip is allowed to finish naturally. If the
            // pedal is released before it finishes, stop its dry source and
            // leave the reverb tail in place.
            [voice.player stop];
          }
          voice.deferred_note_off.store(false, std::memory_order_release);
        }
      }
    }
  }

  void note_off(int midi_note) {
    for (std::size_t i = 0; i < kVoiceCount; ++i) {
      const std::size_t index = (next_voice_ + kVoiceCount - 1 - i) % kVoiceCount;
      Voice& voice = voices_[index];
      if (voice.note.load(std::memory_order_relaxed) != midi_note ||
          !voice.gate.load(std::memory_order_acquire)) {
        continue;
      }

      voice.key_held.store(false, std::memory_order_release);
      const Waveform waveform = static_cast<Waveform>(
          voice.waveform.load(std::memory_order_acquire));
      if (sustain_.load(std::memory_order_acquire)) {
        // Keep all voices alive while the pedal is down. File clips are not
        // stopped here, so the scheduled buffer can reach its end.
        voice.deferred_note_off.store(true, std::memory_order_release);
      } else {
        voice.gate.store(false, std::memory_order_release);
        if (waveform == Waveform::kFile) {
          [voice.player stop];
        }
      }
      return;
    }
  }

 private:
  struct Voice {
    std::atomic<bool> gate{false};
    std::atomic<bool> key_held{false};
    std::atomic<bool> deferred_note_off{false};
    std::atomic<int> waveform{static_cast<int>(Waveform::kTriangle)};
    std::atomic<int> note{-1};
    std::atomic<int> pitch_note{-1};
    std::atomic<float> amplitude{0.0f};
    std::atomic<std::uint32_t> generation{0};

    // Triangle state is only accessed by the audio render thread.
    std::uint32_t rendered_generation = 0;
    double phase = 0.0;
    float envelope = 0.0f;

    AVAudioPlayerNode* player = nil;
    AVAudioUnitTimePitch* time_pitch = nil;
  };

  AVAudioPCMBuffer* load_clip(const std::string& path, NSError** error) {
    std::string expanded_path = path;
    if (!path.empty() && path[0] == '~' &&
        (path.size() == 1 || path[1] == '/')) {
      const char* home = std::getenv("HOME");
      if (home != nullptr && *home != '\0') {
        expanded_path = std::string(home) + path.substr(1);
      }
    }

    std::ifstream file_check(expanded_path);
    if (!file_check) {
      if (error != nullptr) {
        *error = [NSError errorWithDomain:@"SampleSynth" code:1 userInfo:@{
            NSLocalizedDescriptionKey: [NSString stringWithFormat:@"file not found: %s",
                                                                  expanded_path.c_str()] }];
      }
      return nil;
    }

    NSString* ns_path = [NSString stringWithUTF8String:expanded_path.c_str()];
    AVAudioFile* file = [[AVAudioFile alloc]
        initForReading:[NSURL fileURLWithPath:ns_path] error:error];
    if (file == nil || file.length == 0) return nil;

    AVAudioFormat* file_format = file.processingFormat;
    if (file_format == nil || file_format.channelCount == 0 || file_format.sampleRate <= 0) {
      if (error != nullptr) {
        *error = [NSError errorWithDomain:@"SampleSynth" code:2 userInfo:@{
            NSLocalizedDescriptionKey: @"sample has no usable PCM format" }];
      }
      return nil;
    }

    AVAudioPCMBuffer* input = [[AVAudioPCMBuffer alloc]
        initWithPCMFormat:file_format
        frameCapacity:static_cast<AVAudioFrameCount>(file.length)];
    if (input == nil || ![file readIntoBuffer:input error:error]) return nil;

    AVAudioConverter* converter = [[AVAudioConverter alloc]
        initFromFormat:file_format toFormat:format_];
    if (converter == nil) return nil;

    const AVAudioFrameCount capacity = static_cast<AVAudioFrameCount>(
        std::ceil(input.frameLength * format_.sampleRate / file_format.sampleRate) + 1);
    AVAudioPCMBuffer* output = [[AVAudioPCMBuffer alloc]
        initWithPCMFormat:format_ frameCapacity:capacity];
    if (output == nil) return nil;

    __block bool supplied = false;
    const AVAudioConverterOutputStatus status = [converter
        convertToBuffer:output
        error:error
        withInputFromBlock:^AVAudioBuffer*(AVAudioPacketCount /*packets*/,
                                            AVAudioConverterInputStatus* input_status) {
          if (supplied) {
            *input_status = AVAudioConverterInputStatus_EndOfStream;
            return nil;
          }
          supplied = true;
          *input_status = AVAudioConverterInputStatus_HaveData;
          return input;
        }];
    if (status == AVAudioConverterOutputStatus_Error || output.frameLength == 0) return nil;

    return output;
  }

  std::size_t zone_index_for_note(int midi_note) const {
    return std::min<std::size_t>(sample_count_ - 1,
                                 (static_cast<std::size_t>(midi_note) * sample_count_) / 128);
  }

  static float waveform_sample(Waveform waveform, double phase) {
    switch (waveform) {
      case Waveform::kSquare:
        return phase < 0.5 ? 1.0f : -1.0f;
      case Waveform::kSine:
        return static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * phase));
      case Waveform::kTriangle:
        return 1.0f - 4.0f * static_cast<float>(std::abs(phase - 0.5));
      case Waveform::kFile:
        return 0.0f;
    }
    return 0.0f;
  }

  float waveform_gain(Waveform waveform) const {
    // Unit-peak RMS values are 1, 1/sqrt(2), and 1/sqrt(3). Scale each
    // oscillator to the measured RMS reference from the supplied files.
    switch (waveform) {
      case Waveform::kSquare:
        return sample_reference_rms_;
      case Waveform::kSine:
        return sample_reference_rms_ * 1.41421356f;
      case Waveform::kTriangle:
        return sample_reference_rms_ * 1.73205081f;
      case Waveform::kFile:
        return 0.0f;
    }
    return sample_reference_rms_;
  }

  static float buffer_rms(AVAudioPCMBuffer* buffer) {
    if (buffer == nil || buffer.floatChannelData == nullptr || buffer.frameLength == 0) {
      return 0.0f;
    }
    double sum = 0.0;
    const std::size_t count = static_cast<std::size_t>(buffer.frameLength) *
                              buffer.format.channelCount;
    for (AVAudioChannelCount channel = 0; channel < buffer.format.channelCount; ++channel) {
      const float* samples = buffer.floatChannelData[channel];
      for (AVAudioFrameCount frame = 0; frame < buffer.frameLength; ++frame) {
        sum += static_cast<double>(samples[frame]) * samples[frame];
      }
    }
    return count == 0 ? 0.0f : static_cast<float>(std::sqrt(sum / count));
  }

  static float buffer_peak(AVAudioPCMBuffer* buffer) {
    if (buffer == nil || buffer.floatChannelData == nullptr) return 0.0f;
    float peak = 0.0f;
    for (AVAudioChannelCount channel = 0; channel < buffer.format.channelCount; ++channel) {
      const float* samples = buffer.floatChannelData[channel];
      for (AVAudioFrameCount frame = 0; frame < buffer.frameLength; ++frame) {
        peak = std::max(peak, std::abs(samples[frame]));
      }
    }
    return peak;
  }

  static void scale_buffer(AVAudioPCMBuffer* buffer, float scale) {
    if (buffer == nil || buffer.floatChannelData == nullptr) return;
    for (AVAudioChannelCount channel = 0; channel < buffer.format.channelCount; ++channel) {
      float* samples = buffer.floatChannelData[channel];
      for (AVAudioFrameCount frame = 0; frame < buffer.frameLength; ++frame) {
        samples[frame] *= scale;
      }
    }
  }

  void normalize_file_clips() {
    double rms_sum = 0.0;
    std::size_t rms_count = 0;
    for (std::size_t i = 0; i < sample_waveforms_.size(); ++i) {
      if (sample_waveforms_[i] != Waveform::kFile) continue;
      const float rms = buffer_rms(clips_[i]);
      if (rms > 0.000001f) {
        rms_sum += rms;
        ++rms_count;
      }
    }
    sample_reference_rms_ = rms_count == 0
                                 ? kFallbackReferenceRms
                                 : static_cast<float>(rms_sum / rms_count);

    // Make each supplied file match the average supplied-file loudness. The
    // common final voice gain and mixer headroom handle aggregate peaks.
    for (std::size_t i = 0; i < sample_waveforms_.size(); ++i) {
      if (sample_waveforms_[i] != Waveform::kFile) continue;
      const float rms = buffer_rms(clips_[i]);
      if (rms <= 0.000001f) continue;
      const float rms_scale = sample_reference_rms_ / rms;
      scale_buffer(clips_[i], rms_scale);
    }
  }

  void clear_output(AudioBufferList* output_data, AVAudioFrameCount frame_count) const {
    for (UInt32 buffer = 0; buffer < output_data->mNumberBuffers; ++buffer) {
      const AVAudioChannelCount channels = output_data->mBuffers[buffer].mNumberChannels;
      std::fill_n(static_cast<float*>(output_data->mBuffers[buffer].mData),
                  frame_count * channels, 0.0f);
    }
  }

  void add_output(AudioBufferList* output_data,
                  AVAudioFrameCount frame,
                  AVAudioChannelCount channel,
                  float value) const {
    if (output_data->mNumberBuffers == 1 &&
        output_data->mBuffers[0].mNumberChannels > 1) {
      const AVAudioChannelCount channels = output_data->mBuffers[0].mNumberChannels;
      static_cast<float*>(output_data->mBuffers[0].mData)[frame * channels + channel] += value;
    } else {
      static_cast<float*>(output_data->mBuffers[channel].mData)[frame] += value;
    }
  }

  void render_triangle(AVAudioFrameCount frame_count, AudioBufferList* output_data) {
    const AVAudioChannelCount output_channels =
        output_data->mNumberBuffers == 1
            ? output_data->mBuffers[0].mNumberChannels
            : output_data->mNumberBuffers;
    const double sample_rate = format_.sampleRate;
    const float attack_step = 1.0f / (sample_rate * kAttackSeconds);
    const float release_step = 1.0f / (sample_rate * kReleaseSeconds);

    clear_output(output_data, frame_count);
    for (AVAudioFrameCount frame = 0; frame < frame_count; ++frame) {
      for (Voice& voice : voices_) {
        if (static_cast<Waveform>(voice.waveform.load(std::memory_order_acquire)) ==
            Waveform::kFile) continue;

        const std::uint32_t generation = voice.generation.load(std::memory_order_acquire);
        if (generation != voice.rendered_generation) {
          voice.rendered_generation = generation;
          voice.phase = 0.0;
          voice.envelope = 0.0f;
        }

        const bool gated = voice.gate.load(std::memory_order_acquire);
        if (gated) {
          voice.envelope = std::min(1.0f, voice.envelope + attack_step);
        } else {
          voice.envelope = std::max(0.0f, voice.envelope - release_step);
        }
        if (voice.envelope <= 0.0f) continue;

        const Waveform waveform = static_cast<Waveform>(
            voice.waveform.load(std::memory_order_relaxed));
        const int pitch_note = voice.pitch_note.load(std::memory_order_relaxed);
        const double frequency = 440.0 * std::pow(2.0, (pitch_note - 69) / 12.0);
        const float value = waveform_sample(waveform, voice.phase) *
                            waveform_gain(waveform) * voice.envelope *
                            voice.amplitude.load(std::memory_order_relaxed) * kVoiceGain;
        for (AVAudioChannelCount channel = 0; channel < output_channels; ++channel) {
          add_output(output_data, frame, channel, value);
        }
        voice.phase += frequency / sample_rate;
        voice.phase -= std::floor(voice.phase);
      }
    }
  }

  int root_note_ = 60;
  std::size_t sample_count_ = 0;
  bool has_synth_waveforms_ = false;
  bool has_file_samples_ = false;
  std::atomic<bool> sustain_{false};
  float sample_reference_rms_ = kFallbackReferenceRms;
  AVAudioEngine* engine_ = nil;
  AVAudioFormat* format_ = nil;
  AVAudioSourceNode* source_ = nil;
  AVAudioMixerNode* sample_mixer_ = nil;
  AVAudioUnitReverb* reverb_ = nil;
  std::vector<AVAudioPCMBuffer*> clips_;
  std::vector<Waveform> sample_waveforms_;
  std::vector<std::vector<std::size_t>> zone_assets_;
  std::mt19937 random_{std::random_device{}()};
  std::array<Voice, kVoiceCount> voices_;
  std::size_t next_voice_ = 0;
};

enum class PendingEventType { kNote, kSustain };

struct PendingEvent {
  PendingEventType type;
  int note = 0;
  int velocity = 0;
  bool note_on = false;
  bool sustain = false;
};

struct Options {
  std::vector<std::vector<std::string>> sample_groups = {{""}};
  int root_note = 60;
  int midi_channel = 0;
};

void print_usage(const char* program) {
  std::cerr << "Usage: " << program << " [options]\n\n"
            << "MIDI sample player. The 128 MIDI notes are divided into N equal\n"
            << "contiguous ranges, one range per --sample option. Comma-separated\n"
            << "values within one option are random alternates in that range.\n\n"
            << "Options:\n"
            << "  -s, --sample VALUE      Add a file or waveform (triangle, square, sine);\n"
            << "                          repeat for more keyboard zones (default: triangle)\n"
            << "  -r, --root-note NOTE    Root MIDI note for every sample (default: 60)\n"
            << "  -c, --channel CHANNEL   MIDI channel 1-16, or 0 for all (default: 0)\n"
            << "  -h, --help              Show this help\n\n"
            << "Use --sample triangle, --sample square, or --sample sine for\n"
            << "continuously synthesized waveforms.\n";
}

std::vector<std::string> split_samples(std::string_view value) {
  std::vector<std::string> samples;
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t comma = value.find(',', start);
    const std::size_t end = comma == std::string_view::npos ? value.size() : comma;
    if (end == start) return {};
    std::string sample(value.substr(start, end - start));
    if (sample == "builtin") sample = "triangle";
    samples.push_back(sample == "triangle" ? "" : sample);
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return samples;
}

std::optional<Options> parse_options(int argc, char** argv) {
  Options options;
  options.sample_groups.clear();
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") return std::nullopt;

    auto next_value = [&](const char* option) -> std::optional<std::string> {
      if (i + 1 >= argc) {
        std::cerr << option << " requires a value\n";
        return std::nullopt;
      }
      return std::string(argv[++i]);
    };

    if (arg == "-s" || arg == "--sample") {
      const auto value = next_value(arg.c_str());
      if (!value || value->empty()) return std::nullopt;
      const auto group = split_samples(*value);
      if (group.empty()) {
        std::cerr << "--sample contains an empty comma-separated value\n";
        return std::nullopt;
      }
      options.sample_groups.push_back(group);
    } else if (arg.rfind("--sample=", 0) == 0) {
      const auto group = split_samples(arg.substr(9));
      if (group.empty()) {
        std::cerr << "--sample contains an empty comma-separated value\n";
        return std::nullopt;
      }
      options.sample_groups.push_back(group);
    } else if (arg == "-r" || arg == "--root-note") {
      const auto value = next_value(arg.c_str());
      if (!value) return std::nullopt;
      try { options.root_note = std::stoi(*value); }
      catch (...) { std::cerr << "--root-note must be an integer\n"; return std::nullopt; }
    } else if (arg == "-c" || arg == "--channel") {
      const auto value = next_value(arg.c_str());
      if (!value) return std::nullopt;
      try { options.midi_channel = std::stoi(*value); }
      catch (...) { std::cerr << "--channel must be an integer\n"; return std::nullopt; }
    } else if (!arg.empty() && arg[0] != '-') {
      const auto group = split_samples(arg);
      if (group.empty()) return std::nullopt;
      options.sample_groups.push_back(group);
    } else {
      std::cerr << "unknown option: " << arg << "\n";
      return std::nullopt;
    }
  }

  if (options.sample_groups.empty()) options.sample_groups.push_back({""});
  if (options.sample_groups.size() > 128) {
    std::cerr << "at most 128 --sample zones are supported\n";
    return std::nullopt;
  }
  if (options.root_note < 0 || options.root_note > 127) {
    std::cerr << "--root-note must be between 0 and 127\n";
    return std::nullopt;
  }
  if (options.midi_channel < 0 || options.midi_channel > 16) {
    std::cerr << "--channel must be 0 (all) or between 1 and 16\n";
    return std::nullopt;
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, handle_signal);
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "-h" || std::string_view(argv[i]) == "--help") {
      print_usage(argv[0]);
      return 0;
    }
  }

  const std::optional<Options> maybe_options = parse_options(argc, argv);
  if (!maybe_options) return 1;
  const Options& options = *maybe_options;

  SampleSynth synth;
  std::string error;
  if (!synth.start(options.sample_groups, options.root_note, &error)) {
    std::cerr << error << '\n';
    return 1;
  }

  std::mutex events_mutex;
  std::vector<PendingEvent> pending_events;
  midi::Input input;
  if (!input.start([&](int note, int velocity, bool note_on) {
        std::lock_guard<std::mutex> lock(events_mutex);
        if (pending_events.size() < 1024) {
          pending_events.push_back({PendingEventType::kNote, note, velocity, note_on, false});
        }
      }, &error, [&](int controller, int value) {
        if (controller == 64) {
          std::lock_guard<std::mutex> lock(events_mutex);
          if (pending_events.size() < 1024) {
            pending_events.push_back(
                {PendingEventType::kSustain, 0, 0, false, value >= 64});
          }
        }
      }, options.midi_channel)) {
    std::cerr << error << ". Connect a keyboard and try again.\n";
    return 1;
  }

  std::cout << "Sample synth ready with " << options.sample_groups.size()
            << " keyboard zone" << (options.sample_groups.size() == 1 ? "" : "s")
            << ". Key release fades; sustain holds active voices.\n";
  while (g_running) {
    std::vector<PendingEvent> events;
    {
      std::lock_guard<std::mutex> lock(events_mutex);
      events.swap(pending_events);
    }
    for (const PendingEvent& event : events) {
      if (event.type == PendingEventType::kSustain) {
        synth.set_sustain(event.sustain);
      } else if (event.note_on) {
        synth.note_on(event.note, event.velocity);
      } else {
        synth.note_off(event.note);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  input.stop();
  return 0;
}
