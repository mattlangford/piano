#include "midi/midi_input.hh"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace midi {

Input::~Input() {
  stop();
}

void Input::set_error(std::string* error, const std::string& message) const {
  if (error != nullptr) {
    *error = message;
  }
}

bool Input::start(NoteHandler handler, std::string* error) {
  stop();
  handler_ = std::move(handler);

  OSStatus status = MIDIClientCreate(CFSTR("PianoMidiClient"), nullptr, nullptr, &client_);
  if (status != noErr) {
    set_error(error, "failed to create MIDI client (status " + std::to_string(status) + ")");
    stop();
    return false;
  }

  status = MIDIInputPortCreate(client_, CFSTR("PianoMidiInput"), &Input::read_packets,
                               this, &input_port_);
  if (status != noErr) {
    set_error(error, "failed to create MIDI input port (status " + std::to_string(status) + ")");
    stop();
    return false;
  }

  const ItemCount source_count = MIDIGetNumberOfSources();
  if (source_count == 0) {
    set_error(error, "no MIDI input sources found");
    stop();
    return false;
  }

  bool connected = false;
  for (ItemCount i = 0; i < source_count; ++i) {
    const MIDIEndpointRef source = MIDIGetSource(i);
    if (source != 0 && MIDIPortConnectSource(input_port_, source, nullptr) == noErr) {
      connected = true;
    }
  }
  if (!connected) {
    set_error(error, "failed to connect to MIDI sources");
    stop();
    return false;
  }
  return true;
}

void Input::stop() {
  if (input_port_ != 0) {
    MIDIPortDispose(input_port_);
    input_port_ = 0;
  }
  if (client_ != 0) {
    MIDIClientDispose(client_);
    client_ = 0;
  }
  handler_ = nullptr;
}

void Input::read_packets(const MIDIPacketList* packet_list,
                         void* read_proc_ref_con,
                         void* /*src_conn_ref_con*/) {
  static_cast<Input*>(read_proc_ref_con)->handle_packets(packet_list);
}

void Input::handle_packets(const MIDIPacketList* packet_list) {
  if (!handler_) {
    return;
  }

  const MIDIPacket* packet = &packet_list->packet[0];
  for (unsigned int packet_index = 0; packet_index < packet_list->numPackets; ++packet_index) {
    std::uint8_t running_status = 0;
    std::size_t index = 0;
    while (index < packet->length) {
      const std::uint8_t byte = packet->data[index++];
      if (byte >= 0xf8) {
        continue;  // MIDI realtime messages may appear between data bytes.
      }
      if (byte & 0x80) {
        if (byte < 0xf0) {
          running_status = byte;
        } else {
          running_status = 0;
        }
        if (byte >= 0xf0) {
          continue;  // System messages are not needed by this input API.
        }
      } else if (running_status == 0) {
        continue;
      } else {
        --index;  // Re-use this data byte below as the first data byte.
      }

      const std::uint8_t status = running_status & 0xf0;
      if (status != 0x80 && status != 0x90) {
        const std::size_t data_bytes = (status == 0xc0 || status == 0xd0) ? 1 : 2;
        index = std::min<std::size_t>(packet->length, index + data_bytes);
        continue;
      }
      if (index + 1 >= packet->length) {
        break;
      }
      const int note = packet->data[index++];
      const int velocity = packet->data[index++];
      handler_(note, velocity, status == 0x90 && velocity != 0);
    }
    packet = MIDIPacketNext(packet);
  }
}

}  // namespace midi
