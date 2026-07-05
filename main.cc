#include <CoreMIDI/CoreMIDI.h>
#include <CoreMIDI/CoreMIDI.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::atomic<bool> g_running = true;

void HandleSignal(int) {
  g_running = false;
}

struct ChordPattern {
  std::string suffix;
  std::vector<int> intervals;
};

struct ChordQuestion {
  std::string name;
  std::set<int> pitch_classes;
  std::string semitone_tuple;
};

struct Options {
  int level = 1;
  bool keyboard_mode = false;
};

class MidiState {
 public:
  void OnNoteOn(int note) {
    if (note < 0 || note >= 128) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      active_note_counts_[note] += 1;
    }
    cv_.notify_all();
  }

  void OnNoteOff(int note) {
    if (note < 0 || note >= 128) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (active_note_counts_[note] > 0) {
        active_note_counts_[note] -= 1;
      }
    }
    cv_.notify_all();
  }

  std::optional<std::set<int>> WaitForAttempt(const std::atomic<bool>& running) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [&] { return !running || !CurrentPitchClassesLocked().empty(); });
    if (!running) {
      return std::nullopt;
    }

    std::set<int> attempt_pitch_classes;
    while (running) {
      const std::set<int> current = CurrentPitchClassesLocked();
      if (current.empty()) {
        if (!attempt_pitch_classes.empty()) {
          return attempt_pitch_classes;
        }
      } else {
        attempt_pitch_classes.insert(current.begin(), current.end());
      }
      cv_.wait(lock);
    }
    return std::nullopt;
  }

  void WaitForAllNotesOff(const std::atomic<bool>& running) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [&] { return !running || CurrentPitchClassesLocked().empty(); });
  }

 private:
  std::set<int> CurrentPitchClassesLocked() const {
    std::set<int> classes;
    for (int note = 0; note < static_cast<int>(active_note_counts_.size()); ++note) {
      if (active_note_counts_[note] > 0) {
        classes.insert(note % 12);
      }
    }
    return classes;
  }

  std::array<int, 128> active_note_counts_{};
  std::mutex mu_;
  std::condition_variable cv_;
};

void ReadMidiPackets(const MIDIPacketList* packet_list,
                     void* read_proc_ref_con,
                     void* /*src_conn_ref_con*/) {
  auto* state = static_cast<MidiState*>(read_proc_ref_con);
  const MIDIPacket* packet = &packet_list->packet[0];

  for (unsigned int i = 0; i < packet_list->numPackets; ++i) {
    std::size_t index = 0;
    while (index < packet->length) {
      const std::uint8_t status = packet->data[index] & 0xF0;
      if ((status == 0x80 || status == 0x90) && index + 2 < packet->length) {
        const int note = packet->data[index + 1];
        const int velocity = packet->data[index + 2];
        if (status == 0x90 && velocity > 0) {
          state->OnNoteOn(note);
        } else {
          state->OnNoteOff(note);
        }
        index += 3;
      } else {
        // Ignore unsupported events in this simple trainer.
        index += 1;
      }
    }
    packet = MIDIPacketNext(packet);
  }
}

std::vector<ChordPattern> PatternsForLevel(int level) {
  std::vector<ChordPattern> patterns = {
      {"", {0, 4, 7}},
      {"m", {0, 3, 7}},
  };

  if (level >= 2) {
    patterns.push_back({"dim", {0, 3, 6}});
    patterns.push_back({"aug", {0, 4, 8}});
    patterns.push_back({"sus2", {0, 2, 7}});
    patterns.push_back({"sus4", {0, 5, 7}});
  }

  if (level >= 3) {
    patterns.push_back({"7", {0, 4, 7, 10}});
    patterns.push_back({"maj7", {0, 4, 7, 11}});
    patterns.push_back({"m7", {0, 3, 7, 10}});
    patterns.push_back({"m7b5", {0, 3, 6, 10}});
  }

  return patterns;
}

ChordQuestion NextQuestion(int level, std::mt19937& rng) {
  static const std::array<std::string, 12> kRootNames = {
      "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
  };

  const auto patterns = PatternsForLevel(level);

  std::uniform_int_distribution<int> root_dist(0, 11);
  std::uniform_int_distribution<std::size_t> pattern_dist(0, patterns.size() - 1);

  const int root = root_dist(rng);
  const ChordPattern& pattern = patterns[pattern_dist(rng)];

  std::set<int> target;
  for (int interval : pattern.intervals) {
    target.insert((root + interval) % 12);
  }

  std::string tuple = "(";
  for (std::size_t i = 0; i < pattern.intervals.size(); ++i) {
    tuple += std::to_string(pattern.intervals[i]);
    if (i + 1 != pattern.intervals.size()) {
      tuple += ", ";
    }
  }
  tuple += ")";

  return {
      .name = kRootNames[root] + pattern.suffix,
      .pitch_classes = target,
      .semitone_tuple = tuple,
  };
}

void PrintUsage() {
  std::cerr << "Usage: bazel run //:practice -- [level 1-3] [--keyboard]\n";
}

std::optional<Options> ParseOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--keyboard") {
      options.keyboard_mode = true;
      continue;
    }
    try {
      const int parsed_level = std::stoi(arg);
      if (parsed_level < 1 || parsed_level > 3) {
        return std::nullopt;
      }
      options.level = parsed_level;
    } catch (...) {
      return std::nullopt;
    }
  }
  return options;
}

int NormalizePitchClass(int semitone) {
  int normalized = semitone % 12;
  if (normalized < 0) {
    normalized += 12;
  }
  return normalized;
}

std::optional<int> ParseSingleNoteToken(const std::string& token) {
  if (token.empty()) {
    return std::nullopt;
  }

  const char letter = static_cast<char>(std::toupper(token[0]));
  int semitone = 0;
  switch (letter) {
    case 'C':
      semitone = 0;
      break;
    case 'D':
      semitone = 2;
      break;
    case 'E':
      semitone = 4;
      break;
    case 'F':
      semitone = 5;
      break;
    case 'G':
      semitone = 7;
      break;
    case 'A':
      semitone = 9;
      break;
    case 'B':
      semitone = 11;
      break;
    default:
      return std::nullopt;
  }

  std::size_t i = 1;
  while (i < token.size() && (token[i] == '#' || token[i] == 'b' || token[i] == 'B')) {
    semitone += (token[i] == '#') ? 1 : -1;
    ++i;
  }

  while (i < token.size()) {
    const char c = token[i];
    if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-' && c != '+') {
      return std::nullopt;
    }
    ++i;
  }

  return NormalizePitchClass(semitone);
}

std::optional<std::set<int>> ParseKeyboardAttemptLine(const std::string& line) {
  std::string normalized = line;
  for (char& c : normalized) {
    if (c == ',') {
      c = ' ';
    }
  }

  std::istringstream iss(normalized);
  std::set<int> pitch_classes;
  std::string token;
  while (iss >> token) {
    const auto maybe_pc = ParseSingleNoteToken(token);
    if (!maybe_pc.has_value()) {
      return std::nullopt;
    }
    pitch_classes.insert(*maybe_pc);
  }

  if (pitch_classes.empty()) {
    return std::nullopt;
  }
  return pitch_classes;
}

std::optional<std::set<int>> ReadKeyboardAttempt(std::atomic<bool>& running) {
  if (!running) {
    return std::nullopt;
  }

  std::cout << "Notes> " << std::flush;
  std::string line;
  if (!std::getline(std::cin, line)) {
    running = false;
    return std::nullopt;
  }
  return ParseKeyboardAttemptLine(line);
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, HandleSignal);

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      PrintUsage();
      return 0;
    }
  }

  const auto maybe_options = ParseOptions(argc, argv);
  if (!maybe_options.has_value()) {
    PrintUsage();
    return 1;
  }
  const Options options = *maybe_options;
  const int level = options.level;

  MidiState midi_state;
  MIDIClientRef client = 0;
  MIDIPortRef input_port = 0;

  if (!options.keyboard_mode) {
    const OSStatus client_status =
        MIDIClientCreate(CFSTR("ChordTrainerClient"), nullptr, nullptr, &client);
    if (client_status != noErr) {
      std::cerr << "Failed to create MIDI client (status " << client_status << ")\n";
      return 1;
    }

    const OSStatus port_status =
        MIDIInputPortCreate(client, CFSTR("ChordTrainerIn"), ReadMidiPackets, &midi_state, &input_port);
    if (port_status != noErr) {
      std::cerr << "Failed to create MIDI input port (status " << port_status << ")\n";
      MIDIClientDispose(client);
      return 1;
    }

    const ItemCount source_count = MIDIGetNumberOfSources();
    if (source_count == 0) {
      std::cerr << "No MIDI input sources found. Connect a keyboard and try again, or use --keyboard.\n";
      MIDIPortDispose(input_port);
      MIDIClientDispose(client);
      return 1;
    }

    bool connected_any_source = false;
    for (ItemCount i = 0; i < source_count; ++i) {
      MIDIEndpointRef source = MIDIGetSource(i);
      if (source == 0) {
        continue;
      }
      const OSStatus connect_status = MIDIPortConnectSource(input_port, source, nullptr);
      if (connect_status == noErr) {
        connected_any_source = true;
      }
    }

    if (!connected_any_source) {
      std::cerr << "Failed to connect to MIDI sources.\n";
      MIDIPortDispose(input_port);
      MIDIClientDispose(client);
      return 1;
    }
  }

  std::cout << "Chord Trainer (level " << level << ")\n";
  if (options.keyboard_mode) {
    std::cout << "Keyboard mode: type notes like C E G or F# A# C#. Ctrl+C to quit.\n\n";
  } else {
    std::cout << "Press chords exactly (no extra pitch classes). Ctrl+C to quit.\n\n";
  }

  std::random_device rd;
  std::mt19937 rng(rd());

  int rounds = 0;
  while (g_running) {
    if (!options.keyboard_mode) {
      midi_state.WaitForAllNotesOff(g_running);
      if (!g_running) {
        break;
      }
    }

    ChordQuestion question = NextQuestion(level, rng);
    std::cout << "Play: " << question.name << '\n';

    const auto start = std::chrono::steady_clock::now();
    bool solved = false;
    while (g_running && !solved) {
      std::optional<std::set<int>> attempt;
      if (options.keyboard_mode) {
        attempt = ReadKeyboardAttempt(g_running);
      } else {
        attempt = midi_state.WaitForAttempt(g_running);
      }

      if (!attempt.has_value()) {
        if (options.keyboard_mode && g_running) {
          std::cout << "Invalid input. Use note names like C E G or Bb D F.\n";
          continue;
        }
        break;
      }

      if (*attempt == question.pitch_classes) {
        solved = true;
        break;
      }

      std::cout << "Incorrect. Semitone tuple: " << question.semitone_tuple
                << ". Try again.\n";
    }

    if (!g_running || !solved) {
      break;
    }

    const auto end = std::chrono::steady_clock::now();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "Correct in " << (elapsed / 1000.0) << "s\n\n";
    ++rounds;
  }

  std::cout << "Session ended after " << rounds << " chord"
            << (rounds == 1 ? "" : "s") << ".\n";

  if (!options.keyboard_mode) {
    MIDIPortDispose(input_port);
    MIDIClientDispose(client);
  }
  return 0;
}
