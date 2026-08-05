#import <AVFAudio/AVFAudio.h>

#include "midi.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_running = true;

void handle_signal(int) {
  g_running = false;
}

class TriangleSynth {
 public:
  static constexpr std::size_t kVoiceCount = 32;

  bool start(std::string* error) {
    constexpr double kSampleRate = 44100.0;
    constexpr AVAudioChannelCount kChannels = 2;

    format_ = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:kSampleRate
                                                               channels:kChannels];
    engine_ = [[AVAudioEngine alloc] init];
    if (format_ == nil || engine_ == nil) {
      *error = "unable to create AVAudio engine";
      return false;
    }

    // AVAudioSourceNode invokes this block on the real-time audio thread. It
    // only reads atomics and performs fixed-size arithmetic: no locks,
    // allocations, or Objective-C calls occur while rendering audio.
    source_ = [[AVAudioSourceNode alloc] initWithFormat:format_
                                           renderBlock:^OSStatus(
                                               BOOL* is_silence,
                                               const AudioTimeStamp* /*timestamp*/,
                                               AVAudioFrameCount frame_count,
                                               AudioBufferList* output_data) {
      *is_silence = NO;
      render_audio(frame_count, output_data, kSampleRate);
      return noErr;
    }];
    if (source_ == nil) {
      *error = "unable to create audio source";
      return false;
    }

    [engine_ attachNode:source_];
    [engine_ connect:source_ to:engine_.mainMixerNode format:format_];

    NSError* ns_error = nil;
    if (![engine_ startAndReturnError:&ns_error]) {
      *error = ns_error ? std::string([[ns_error localizedDescription] UTF8String])
                        : "unable to start AVAudio engine";
      return false;
    }
    return true;
  }

  TriangleSynth() = default;

  ~TriangleSynth() {
    [engine_ stop];
  }

  TriangleSynth(const TriangleSynth&) = delete;
  TriangleSynth& operator=(const TriangleSynth&) = delete;

  void note_on(int midi_note, int velocity) {
    if (midi_note < 0 || midi_note > 127) return;

    Voice& voice = voices_[next_voice_++ % kVoiceCount];
    voice.note.store(midi_note, std::memory_order_relaxed);
    voice.frequency.store(static_cast<float>(midi_frequency(midi_note)),
                          std::memory_order_relaxed);
    voice.amplitude.store(std::clamp(velocity, 1, 127) / 127.0f,
                          std::memory_order_relaxed);
    voice.gate.store(true, std::memory_order_release);
  }

  void note_off(int midi_note) {
    for (Voice& voice : voices_) {
      if (voice.note.load(std::memory_order_relaxed) == midi_note) {
        voice.gate.store(false, std::memory_order_release);
      }
    }
  }

 private:
  struct Voice {
    std::atomic<bool> gate{false};
    std::atomic<int> note{-1};
    std::atomic<float> frequency{0.0f};
    std::atomic<float> amplitude{0.0f};

    // These fields belong exclusively to the audio render thread.
    float phase = 0.0f;
    float envelope = 0.0f;
    int rendered_note = -1;
    bool was_gated = false;
  };

  static double midi_frequency(int midi_note) {
    return 440.0 * std::pow(2.0, (midi_note - 69) / 12.0);
  }

  static float triangle(float phase) {
    return 4.0f * std::fabs(phase - 0.5f) - 1.0f;
  }

  void render_audio(AVAudioFrameCount frame_count,
                    AudioBufferList* output_data,
                    double sample_rate) {
    const bool interleaved = output_data->mNumberBuffers == 1 &&
                             output_data->mBuffers[0].mNumberChannels > 1;
    const AVAudioChannelCount channel_count = interleaved
                                                  ? output_data->mBuffers[0].mNumberChannels
                                                  : output_data->mNumberBuffers;

    for (UInt32 buffer_index = 0; buffer_index < output_data->mNumberBuffers; ++buffer_index) {
      std::fill_n(static_cast<float*>(output_data->mBuffers[buffer_index].mData),
                  frame_count * (interleaved ? channel_count : 1), 0.0f);
    }

    constexpr float kAttackSeconds = 0.005f;
    constexpr float kReleaseSeconds = 0.08f;
    constexpr float kVoiceGain = 0.18f;
    const float attack_step = 1.0f / static_cast<float>(sample_rate * kAttackSeconds);
    const float release_step = 1.0f / static_cast<float>(sample_rate * kReleaseSeconds);

    for (AVAudioFrameCount frame = 0; frame < frame_count; ++frame) {
      float mixed_sample = 0.0f;
      for (Voice& voice : voices_) {
        const int note = voice.note.load(std::memory_order_relaxed);
        const bool gated = voice.gate.load(std::memory_order_acquire);

        if (note != voice.rendered_note) {
          voice.rendered_note = note;
          voice.phase = 0.0f;
          voice.envelope = 0.0f;
        }

        if (gated) {
          voice.envelope = std::min(1.0f, voice.envelope + attack_step);
        } else {
          voice.envelope = std::max(0.0f, voice.envelope - release_step);
        }

        if (voice.envelope > 0.0f && note >= 0) {
          const float sample = triangle(voice.phase);
          mixed_sample += sample * voice.envelope *
                          voice.amplitude.load(std::memory_order_relaxed) * kVoiceGain;
          voice.phase += voice.frequency.load(std::memory_order_relaxed) /
                         static_cast<float>(sample_rate);
          voice.phase -= std::floor(voice.phase);
        }

        voice.was_gated = gated;
      }

      if (interleaved) {
        float* output = static_cast<float*>(output_data->mBuffers[0].mData);
        for (AVAudioChannelCount channel = 0; channel < channel_count; ++channel) {
          output[frame * channel_count + channel] = mixed_sample;
        }
      } else {
        for (UInt32 buffer_index = 0; buffer_index < output_data->mNumberBuffers;
             ++buffer_index) {
          static_cast<float*>(output_data->mBuffers[buffer_index].mData)[frame] = mixed_sample;
        }
      }
    }
  }

  AVAudioEngine* engine_ = nil;
  AVAudioFormat* format_ = nil;
  AVAudioSourceNode* source_ = nil;
  std::array<Voice, kVoiceCount> voices_;
  std::size_t next_voice_ = 0;
};

struct PendingNote {
  int note;
  int velocity;
  bool note_on;
};

}  // namespace

int main(int argc, [[maybe_unused]] char** argv) {
  std::signal(SIGINT, handle_signal);
  if (argc != 1) {
    std::cerr << "Usage: bazel run //synth:sample_synth\n";
    return 1;
  }

  TriangleSynth synth;
  std::string error;
  if (!synth.start(&error)) {
    std::cerr << error << '\n';
    return 1;
  }

  // CoreMIDI callbacks stay short. Audio scheduling and voice state changes
  // happen on this application's main loop instead.
  std::mutex notes_mutex;
  std::vector<PendingNote> pending_notes;
  midi::Input input;
  if (!input.start([&](int note, int velocity, bool note_on) {
        std::lock_guard<std::mutex> lock(notes_mutex);
        if (pending_notes.size() < 1024) {
          pending_notes.push_back({note, velocity, note_on});
        }
      }, &error)) {
    std::cerr << error << ". Connect a keyboard and try again.\n";
    return 1;
  }

  std::cout << "Triangle synth ready. Press notes; Ctrl+C to quit.\n";
  while (g_running) {
    std::vector<PendingNote> notes;
    {
      std::lock_guard<std::mutex> lock(notes_mutex);
      notes.swap(pending_notes);
    }
    for (const PendingNote& event : notes) {
      if (event.note_on) {
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
