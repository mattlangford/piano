#include <CoreMIDI/CoreMIDI.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cctype>
#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <ctime>
#include <sys/select.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace {

std::atomic<bool> g_running = true;
std::atomic<int> g_preview_index = 0;

void handle_signal(int) {
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

struct CategoryStats {
  int asked = 0;
  int correct = 0;
  int wrong_attempts = 0;
  long long total_response_ms = 0;
};

struct Options {
  int level = 1;
  bool keyboard_mode = false;
  bool quiet_mode = false;
  bool analyze_mode = false;
  bool log_history = false;
  std::string log_path = ".history.csv";
};

class MidiState {
 public:
  void on_note_on(int note) {
    if (note < 0 || note >= 128) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      active_note_counts_[note] += 1;
    }
    cv_.notify_all();
  }

  void on_note_off(int note) {
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

  std::optional<std::set<int>> wait_for_attempt(const std::atomic<bool>& running) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [&] { return !running || !current_pitch_classes_locked().empty(); });
    if (!running) {
      return std::nullopt;
    }

    std::set<int> attempt_pitch_classes;
    while (running) {
      const std::set<int> current = current_pitch_classes_locked();
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

  void wait_for_all_notes_off(const std::atomic<bool>& running) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [&] { return !running || current_pitch_classes_locked().empty(); });
  }

 private:
  std::set<int> current_pitch_classes_locked() const {
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

void read_midi_packets(const MIDIPacketList* packet_list,
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
          state->on_note_on(note);
        } else {
          state->on_note_off(note);
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

std::vector<ChordPattern> patterns_for_level(int level) {
  std::vector<ChordPattern> patterns = {
      {"", {0, 4, 7}},
      {"m", {0, 3, 7}},
  };

  if (level >= 2) {
    patterns.push_back({"7", {0, 4, 7, 10}});
    patterns.push_back({"maj7", {0, 4, 7, 11}});
    patterns.push_back({"m7", {0, 3, 7, 10}});
    patterns.push_back({"m7b5", {0, 3, 6, 10}});
  }

  if (level >= 3) {
    patterns.push_back({"dim", {0, 3, 6}});
    patterns.push_back({"aug", {0, 4, 8}});
    patterns.push_back({"sus2", {0, 2, 7}});
    patterns.push_back({"sus4", {0, 5, 7}});
  }

  return patterns;
}

double question_weight(const CategoryStats& stats) {
  if (stats.asked == 0) {
    return 1.0;
  }

  const double error_rate =
      static_cast<double>(stats.wrong_attempts) / std::max(1, stats.asked);
  const double avg_response_sec =
      (static_cast<double>(stats.total_response_ms) / std::max(1, stats.correct)) / 1000.0;
  const double slow_penalty = std::max(0.0, avg_response_sec - 2.0) * 0.25;
  return 1.0 + (2.0 * error_rate) + slow_penalty;
}

std::vector<ChordQuestion> all_questions_for_level(int level) {
  static const std::array<std::vector<std::string_view>, 12> kRootNames = {
      std::vector<std::string_view>{"C"},
      std::vector<std::string_view>{"C#", "Db"},
      std::vector<std::string_view>{"D"},
      std::vector<std::string_view>{"D#", "Eb"},
      std::vector<std::string_view>{"E"},
      std::vector<std::string_view>{"F"},
      std::vector<std::string_view>{"F#", "Gb"},
      std::vector<std::string_view>{"G"},
      std::vector<std::string_view>{"G#", "Ab"},
      std::vector<std::string_view>{"A"},
      std::vector<std::string_view>{"A#", "Bb"},
      std::vector<std::string_view>{"B"},
  };

  const auto patterns = patterns_for_level(level);

  std::vector<ChordQuestion> questions;
  questions.reserve(12 * patterns.size() * 2);

  for (int root = 0; root < 12; ++root) {
    const std::vector<std::string_view>& root_name_choices = kRootNames[root];
    for (const ChordPattern& pattern : patterns) {
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

      for (std::string_view root_name : root_name_choices) {
        questions.push_back({
            .name = std::string(root_name) + pattern.suffix,
            .pitch_classes = target,
            .semitone_tuple = tuple,
        });
      }
    }
  }

  return questions;
}

ChordQuestion next_question(int level,
                           std::mt19937& rng,
                           const std::unordered_map<std::string, CategoryStats>& stats_by_chord,
                           const std::optional<std::string>& last_question_name) {
  std::vector<ChordQuestion> questions = all_questions_for_level(level);

  if (last_question_name.has_value() && questions.size() > 1) {
    questions.erase(
        std::remove_if(questions.begin(),
                       questions.end(),
                       [&](const ChordQuestion& q) { return q.name == *last_question_name; }),
        questions.end());
  }

  std::vector<double> weights;
  weights.reserve(questions.size());
  for (const ChordQuestion& question : questions) {
    const auto it = stats_by_chord.find(question.name);
    const double w = (it == stats_by_chord.end()) ? 1.0 : question_weight(it->second);
    weights.push_back(w);
  }
  std::discrete_distribution<std::size_t> dist(weights.begin(), weights.end());
  return questions[dist(rng)];
}

void update_question_stats(std::unordered_map<std::string, CategoryStats>& stats_by_chord,
                          const ChordQuestion& question,
                          bool solved,
                          int wrong_attempts,
                          long long elapsed_ms) {
  CategoryStats& stats = stats_by_chord[question.name];
  stats.asked += 1;
  stats.wrong_attempts += wrong_attempts;
  if (solved) {
    stats.correct += 1;
    stats.total_response_ms += elapsed_ms;
  }
}

void print_chord_breakdown(const std::unordered_map<std::string, CategoryStats>& stats_by_chord) {
  std::vector<std::pair<std::string, CategoryStats>> rows;
  rows.reserve(stats_by_chord.size());
  for (const auto& [chord, stats] : stats_by_chord) {
    if (stats.asked > 0) {
      rows.push_back({chord, stats});
    }
  }

  if (rows.empty()) {
    return;
  }

  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
    const int a_attempts = a.second.correct + a.second.wrong_attempts;
    const int b_attempts = b.second.correct + b.second.wrong_attempts;
    const double a_wrong_pct =
        (a_attempts > 0)
            ? (100.0 * static_cast<double>(a.second.wrong_attempts) / a_attempts)
            : 0.0;
    const double b_wrong_pct =
        (b_attempts > 0)
            ? (100.0 * static_cast<double>(b.second.wrong_attempts) / b_attempts)
            : 0.0;
    if (a_wrong_pct != b_wrong_pct) {
      return a_wrong_pct > b_wrong_pct;
    }
    return a.first < b.first;
  });

  std::cout << "Chord breakdown (sorted by wrong %):\n";
  std::size_t max_chord_width = std::string("Chord").size();
  for (const auto& [chord, _] : rows) {
    max_chord_width = std::max(max_chord_width, chord.size());
  }
  const int chord_width = static_cast<int>(max_chord_width);
  constexpr int kAttemptsWidth = 8;
  constexpr int kWrongPctWidth = 7;
  constexpr int kAccuracyWidth = 8;
  constexpr int kWrongWidth = 5;
  constexpr int kAvgWidth = 8;
  std::cout << "  " << std::left << std::setw(chord_width) << "Chord"
            << " | " << std::right << std::setw(kAttemptsWidth) << "Attempts"
            << " | " << std::setw(kWrongPctWidth) << "Wrong %"
            << " | " << std::right << std::setw(kAccuracyWidth) << "Accuracy"
            << " | " << std::setw(kWrongWidth) << "Wrong"
            << " | " << std::setw(kAvgWidth) << "Avg" << "\n";
  std::cout << "  " << std::string(max_chord_width, '-')
            << " | " << std::string(kAttemptsWidth, '-')
            << " | " << std::string(kWrongPctWidth, '-')
            << " | " << std::string(kAccuracyWidth, '-')
            << " | " << std::string(kWrongWidth, '-')
            << " | " << std::string(kAvgWidth, '-') << "\n";
  for (const auto& [chord, stats] : rows) {
    const int total_attempts = stats.correct + stats.wrong_attempts;
    const double wrong_pct =
      (total_attempts > 0)
        ? (100.0 * static_cast<double>(stats.wrong_attempts) / total_attempts)
        : 0.0;
    const double accuracy =
      (total_attempts > 0)
        ? (100.0 * static_cast<double>(stats.correct) / total_attempts)
        : 0.0;
    const double avg_s =
        (stats.correct > 0)
            ? (static_cast<double>(stats.total_response_ms) / stats.correct) / 1000.0
            : 0.0;
    std::ostringstream avg_cell;
    if (stats.correct > 0) {
      avg_cell << std::fixed << std::setprecision(2) << avg_s << "s";
    } else {
      avg_cell << "n/a";
    }
    std::cout << "  " << std::left << std::setw(chord_width) << chord
              << " | " << std::right << std::setw(kAttemptsWidth) << total_attempts
              << " | " << std::setw(kWrongPctWidth - 1) << std::fixed << std::setprecision(1)
              << wrong_pct << "%"
              << " | " << std::right << std::setw(kAccuracyWidth - 1)
              << std::fixed << std::setprecision(1)
              << accuracy << "%"
              << " | " << std::setw(kWrongWidth) << stats.wrong_attempts
              << " | " << std::setw(kAvgWidth) << avg_cell.str() << "\n";
  }
}

std::string csv_escape(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size() + 2);
  escaped.push_back('"');
  for (char c : text) {
    if (c == '"') {
      escaped += "\"\"";
    } else {
      escaped.push_back(c);
    }
  }
  escaped.push_back('"');
  return escaped;
}

std::string resolve_log_path(std::string_view requested_path) {
  const std::string path = std::string(requested_path);
  if (path.empty() || path[0] == '/') {
    return path;
  }

  const char* base = std::getenv("BUILD_WORKING_DIRECTORY");
  if (base == nullptr || *base == '\0') {
    base = std::getenv("PWD");
  }
  if (base == nullptr || *base == '\0') {
    return path;
  }

  std::string resolved(base);
  if (!resolved.empty() && resolved.back() != '/') {
    resolved.push_back('/');
  }
  resolved += path;
  return resolved;
}

std::ofstream open_history_file(std::string_view history_path) {
  bool file_exists = false;
  {
    std::ifstream in{std::string(history_path)};
    file_exists = static_cast<bool>(in);
  }

  std::ofstream out(std::string(history_path), std::ios::app);
  if (!out) {
    return out;
  }

  if (!file_exists) {
    out << "timestamp,chord_name,got_right,seconds_to_right\n";
  }
  return out;
}

std::string current_timestamp_local() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);

  std::tm local_tm{};
  if (localtime_r(&now_time, &local_tm) == nullptr) {
    return "";
  }

  char buffer[20] = {};
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local_tm) == 0) {
    return "";
  }
  return std::string(buffer);
}

void write_history_row(std::ofstream& out,
                       const ChordQuestion& question,
                       bool solved,
                       long long elapsed_ms) {
  if (!out) {
    return;
  }

  const std::string timestamp = current_timestamp_local();
  out << csv_escape(timestamp) << ","
      << csv_escape(question.name) << "," << (solved ? "true" : "false") << ",";
  if (solved) {
    out << std::fixed << std::setprecision(3)
        << (static_cast<double>(elapsed_ms) / 1000.0);
  }
  out << "\n";
  out.flush();
}

void print_usage() {
  std::cerr << "Usage: bazel run //:practice -- [--level 1-3] [--keyboard] [--quiet] [--analyze] [--log[=path]]\n";
}

std::optional<Options> parse_options(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--level") {
      if (i + 1 >= argc) {
        return std::nullopt;
      }
      try {
        const int parsed_level = std::stoi(argv[++i]);
        if (parsed_level < 1 || parsed_level > 3) {
          return std::nullopt;
        }
        options.level = parsed_level;
      } catch (...) {
        return std::nullopt;
      }
      continue;
    }
    if (arg == "--keyboard") {
      options.keyboard_mode = true;
      continue;
    }
    if (arg == "--quiet") {
      options.quiet_mode = true;
      continue;
    }
    if (arg == "--analyze") {
      options.analyze_mode = true;
      continue;
    }
    if (arg.rfind("--log=", 0) == 0) {
      options.log_history = true;
      const std::string value = std::string(arg.substr(6));
      if (value.empty()) {
        return std::nullopt;
      }
      options.log_path = value;
      continue;
    }
    if (arg == "--log") {
      options.log_history = true;
      if (i + 1 < argc) {
        const std::string_view next = argv[i + 1];
        if (!next.empty() && next[0] != '-') {
          options.log_path = std::string(next);
          ++i;
        }
      }
      continue;
    }
    return std::nullopt;
  }
  return options;
}

std::string pitch_class_name(int pitch_class) {
  static const std::array<const char*, 12> kNames = {
      "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B",
  };
  int normalized = pitch_class % 12;
  if (normalized < 0) {
    normalized += 12;
  }
  return kNames[normalized];
}

std::string format_pitch_classes(const std::set<int>& pitch_classes) {
  std::string out;
  bool first = true;
  for (int pitch_class : pitch_classes) {
    if (!first) {
      out += " ";
    }
    first = false;
    out += pitch_class_name(pitch_class);
  }
  return out;
}

std::vector<std::string> analyze_chord_names(const std::set<int>& pitch_classes) {
  if (pitch_classes.empty()) {
    return {};
  }

  static const std::array<std::vector<std::string_view>, 12> kRootNames = {
      std::vector<std::string_view>{"C"},
      std::vector<std::string_view>{"C#", "Db"},
      std::vector<std::string_view>{"D"},
      std::vector<std::string_view>{"D#", "Eb"},
      std::vector<std::string_view>{"E"},
      std::vector<std::string_view>{"F"},
      std::vector<std::string_view>{"F#", "Gb"},
      std::vector<std::string_view>{"G"},
      std::vector<std::string_view>{"G#", "Ab"},
      std::vector<std::string_view>{"A"},
      std::vector<std::string_view>{"A#", "Bb"},
      std::vector<std::string_view>{"B"},
  };

  const std::vector<ChordPattern> patterns = patterns_for_level(3);
  std::vector<std::string> matches;
  std::set<std::string> seen;

  for (int root = 0; root < 12; ++root) {
    for (const ChordPattern& pattern : patterns) {
      std::set<int> target;
      for (int interval : pattern.intervals) {
        target.insert((root + interval) % 12);
      }
      if (target != pitch_classes) {
        continue;
      }
      for (std::string_view root_name : kRootNames[root]) {
        const std::string name = std::string(root_name) + pattern.suffix;
        if (seen.insert(name).second) {
          matches.push_back(name);
        }
      }
    }
  }

  return matches;
}

int normalize_pitch_class(int semitone) {
  int normalized = semitone % 12;
  if (normalized < 0) {
    normalized += 12;
  }
  return normalized;
}

std::optional<int> parse_single_note_token(std::string_view token) {
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

  return normalize_pitch_class(semitone);
}

std::optional<std::set<int>> parse_keyboard_attempt_line(const std::string& line) {
  std::istringstream iss(line);
  std::set<int> pitch_classes;
  std::string token;
  while (iss >> token) {
    const auto maybe_pc = parse_single_note_token(token);
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

std::optional<std::set<int>> read_keyboard_attempt(std::atomic<bool>& running) {
  if (!running) {
    return std::nullopt;
  }

  std::cout << "Notes> " << std::flush;
  while (running) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(STDIN_FILENO, &read_fds);

    timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;

    const int ready = select(STDIN_FILENO + 1, &read_fds, nullptr, nullptr, &timeout);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      running = false;
      return std::nullopt;
    }
    if (ready == 0) {
      continue;
    }

    std::string line;
    if (!std::getline(std::cin, line)) {
      running = false;
      return std::nullopt;
    }
    return parse_keyboard_attempt_line(line);
  }

  return std::nullopt;
}

bool write_chord_preview_wav(const std::set<int>& pitch_classes,
                             const std::string& output_path,
                             double seconds) {
  if (pitch_classes.empty()) {
    return false;
  }

  constexpr int kSampleRate = 44100;
  constexpr int kBitsPerSample = 16;
  constexpr int kNumChannels = 1;
  const int num_samples = static_cast<int>(kSampleRate * seconds);
  if (num_samples <= 0) {
    return false;
  }

  std::vector<double> freqs_hz;
  freqs_hz.reserve(pitch_classes.size());
  for (int pc : pitch_classes) {
    const int midi_note = 60 + pc;
    const double frequency = 440.0 * std::pow(2.0, (midi_note - 69) / 12.0);
    freqs_hz.push_back(frequency);
  }

  std::vector<int16_t> pcm_samples;
  pcm_samples.reserve(num_samples);
  constexpr double kPi = 3.14159265358979323846;
  for (int i = 0; i < num_samples; ++i) {
    const double t = static_cast<double>(i) / kSampleRate;
    double value = 0.0;
    for (double f : freqs_hz) {
      value += std::sin(2.0 * kPi * f * t);
    }

    value /= static_cast<double>(freqs_hz.size());

    const double attack = 0.01;
    const double fade_start = 0.5;
    const double fade_end = 1.0;
    double env = 1.0;
    if (t < attack) {
      env = t / attack;
    }
    if (t >= fade_start) {
      env *= std::max(0.0, (fade_end - t) / (fade_end - fade_start));
    }
    value *= env;

    constexpr double kGain = 0.6;
    value *= kGain;
    const int sample = static_cast<int>(std::round(value * 32767.0));
    const int clamped = std::max(-32768, std::min(32767, sample));
    pcm_samples.push_back(static_cast<int16_t>(clamped));
  }

  std::ofstream out(output_path, std::ios::binary);
  if (!out) {
    return false;
  }

  const uint32_t data_size = static_cast<uint32_t>(pcm_samples.size() * sizeof(int16_t));
  const uint32_t riff_chunk_size = 36 + data_size;
  const uint16_t audio_format = 1;
  const uint16_t num_channels = kNumChannels;
  const uint32_t sample_rate = kSampleRate;
  const uint16_t bits_per_sample = kBitsPerSample;
  const uint32_t byte_rate = sample_rate * num_channels * (bits_per_sample / 8);
  const uint16_t block_align = num_channels * (bits_per_sample / 8);
  const uint32_t fmt_chunk_size = 16;

  out.write("RIFF", 4);
  out.write(reinterpret_cast<const char*>(&riff_chunk_size), sizeof(riff_chunk_size));
  out.write("WAVE", 4);

  out.write("fmt ", 4);
  out.write(reinterpret_cast<const char*>(&fmt_chunk_size), sizeof(fmt_chunk_size));
  out.write(reinterpret_cast<const char*>(&audio_format), sizeof(audio_format));
  out.write(reinterpret_cast<const char*>(&num_channels), sizeof(num_channels));
  out.write(reinterpret_cast<const char*>(&sample_rate), sizeof(sample_rate));
  out.write(reinterpret_cast<const char*>(&byte_rate), sizeof(byte_rate));
  out.write(reinterpret_cast<const char*>(&block_align), sizeof(block_align));
  out.write(reinterpret_cast<const char*>(&bits_per_sample), sizeof(bits_per_sample));

  out.write("data", 4);
  out.write(reinterpret_cast<const char*>(&data_size), sizeof(data_size));
  out.write(reinterpret_cast<const char*>(pcm_samples.data()), data_size);

  return out.good();
}

void play_chord_preview(const std::set<int>& pitch_classes) {
  const int preview_index = g_preview_index.fetch_add(1);
  const std::string wav_path =
      "/tmp/practice_chord_preview_" + std::to_string(preview_index) + ".wav";
  if (!write_chord_preview_wav(pitch_classes, wav_path, 1.0)) {
    return;
  }
  std::thread([wav_path] {
    const std::string cmd = "afplay -q 1 " + wav_path;
    const int rc = std::system(cmd.c_str());
    (void)rc;
    std::remove(wav_path.c_str());
  }).detach();
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, handle_signal);

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_usage();
      return 0;
    }
  }

  const auto maybe_options = parse_options(argc, argv);
  if (!maybe_options.has_value()) {
    print_usage();
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
        MIDIInputPortCreate(client, CFSTR("ChordTrainerIn"), read_midi_packets, &midi_state, &input_port);
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
  if (options.quiet_mode) {
    std::cout << "Quiet mode enabled: chord preview audio is off.\n\n";
  }

  std::ofstream history_file;
  std::string resolved_log_path;
  if (options.log_history) {
    resolved_log_path = resolve_log_path(options.log_path);
    history_file = open_history_file(resolved_log_path);
    if (!history_file) {
      std::cerr << "Warning: unable to open " << resolved_log_path
                << " for writing. Continuing without history logging.\n";
    } else {
      std::cout << "History logging enabled: " << resolved_log_path << "\n";
    }
  }

  if (options.analyze_mode) {
    std::cout << "Analyze mode: play chords and this tool will print what it hears. Ctrl+C to quit.\n";
    while (g_running) {
      std::optional<std::set<int>> attempt;
      if (options.keyboard_mode) {
        attempt = read_keyboard_attempt(g_running);
      } else {
        attempt = midi_state.wait_for_attempt(g_running);
      }

      if (!attempt.has_value()) {
        if (options.keyboard_mode && g_running) {
          std::cout << "Invalid input. Use note names like C E G or Bb D F.\n";
          continue;
        }
        break;
      }

      const std::vector<std::string> matches = analyze_chord_names(*attempt);
      std::cout << "Heard: " << format_pitch_classes(*attempt);
      if (matches.empty()) {
        std::cout << " -> (no chord match in trainer set)\n";
      } else {
        std::cout << " -> ";
        for (std::size_t i = 0; i < matches.size(); ++i) {
          if (i > 0) {
            std::cout << ", ";
          }
          std::cout << matches[i];
        }
        std::cout << "\n";
      }
    }

    if (!options.keyboard_mode) {
      MIDIPortDispose(input_port);
      MIDIClientDispose(client);
    }
    return 0;
  }

  std::random_device rd;
  std::mt19937 rng(rd());

  int rounds = 0;
  int total_attempts = 0;
  long long total_response_ms = 0;
  std::unordered_map<std::string, CategoryStats> stats_by_chord;
  std::optional<std::string> last_question_name;
  while (g_running) {
    if (!options.keyboard_mode) {
      midi_state.wait_for_all_notes_off(g_running);
      if (!g_running) {
        break;
      }
    }

    ChordQuestion question = next_question(level, rng, stats_by_chord, last_question_name);
    last_question_name = question.name;
    std::cout << "Play: " << question.name << '\n';
    if (!options.quiet_mode) {
      play_chord_preview(question.pitch_classes);
    }

    const auto start = std::chrono::steady_clock::now();
    bool solved = false;
    int question_wrong_attempts = 0;
    while (g_running && !solved) {
      std::optional<std::set<int>> attempt;
      if (options.keyboard_mode) {
        attempt = read_keyboard_attempt(g_running);
      } else {
        attempt = midi_state.wait_for_attempt(g_running);
      }

      if (!attempt.has_value()) {
        if (options.keyboard_mode && g_running) {
          std::cout << "Invalid input. Use note names like C E G or Bb D F.\n";
          continue;
        }
        break;
      }

      ++total_attempts;

      if (*attempt == question.pitch_classes) {
        solved = true;
        break;
      }

      ++question_wrong_attempts;

      std::cout << "Incorrect. Semitone tuple: " << question.semitone_tuple
                << ". Try again.\n";
    }

    const auto end = std::chrono::steady_clock::now();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    write_history_row(history_file, question, solved, elapsed);
    update_question_stats(stats_by_chord, question, solved, question_wrong_attempts, elapsed);

    if (!g_running || !solved) {
      break;
    }

    std::cout << "Correct in " << (elapsed / 1000.0) << "s\n\n";
    total_response_ms += elapsed;
    ++rounds;
  }

  std::cout << "\nSession ended after " << rounds << " chord"
            << (rounds == 1 ? "" : "s") << ".\n";
  std::cout << "Score: " << rounds << "\n";
  if (rounds > 0) {
    const double avg_seconds = (static_cast<double>(total_response_ms) / rounds) / 1000.0;
    std::cout << "Average time to respond: " << std::fixed << std::setprecision(2)
              << avg_seconds << "s\n";
  } else {
    std::cout << "Average time to respond: n/a\n";
  }
  if (total_attempts > 0) {
    const double accuracy_pct = (100.0 * rounds) / static_cast<double>(total_attempts);
    std::cout << "Accuracy: " << std::fixed << std::setprecision(1)
              << accuracy_pct << "%\n";
  }
  print_chord_breakdown(stats_by_chord);

  if (!options.keyboard_mode) {
    MIDIPortDispose(input_port);
    MIDIClientDispose(client);
  }

  return 0;
}
