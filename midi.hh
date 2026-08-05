#ifndef PIANO_MIDI_H_
#define PIANO_MIDI_H_

#include <CoreMIDI/CoreMIDI.h>

#include <functional>
#include <string>

namespace midi {

// Connects to every MIDI source currently visible to CoreMIDI and reports note
// messages. The callback is invoked on CoreMIDI's input thread, so consumers
// should do very little work there (or hand work to another thread/queue).
class Input {
 public:
  using NoteHandler = std::function<void(int note, int velocity, bool note_on)>;
  using ControlHandler = std::function<void(int controller, int value)>;

  Input() = default;
  Input(const Input&) = delete;
  Input& operator=(const Input&) = delete;
  ~Input();

  bool start(NoteHandler handler,
             std::string* error = nullptr,
             ControlHandler control_handler = nullptr,
             int midi_channel = 0);
  void stop();

 private:
  static void read_packets(const MIDIPacketList* packet_list,
                           void* read_proc_ref_con,
                           void* /*src_conn_ref_con*/);
  void handle_packets(const MIDIPacketList* packet_list);
  void set_error(std::string* error, const std::string& message) const;

  MIDIClientRef client_ = 0;
  MIDIPortRef input_port_ = 0;
  NoteHandler handler_;
  ControlHandler control_handler_;
  int midi_channel_ = 0;
};

}  // namespace midi

#endif  // PIANO_MIDI_H_
