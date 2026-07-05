# Chord Trainer (Bazel + C++)

A terminal-based MIDI ear/shape practice app.

## Features

- Random chord prompts by level
- MIDI input listening via CoreMIDI (macOS)
- Exact chord matching by pitch class
- Reports correctness and response time
- Continuous rounds until Ctrl+C

## Levels

- Level 1: major, minor
- Level 2: + diminished, augmented, sus2, sus4
- Level 3: + 7, maj7, m7, m7b5

## Build

```bash
cd piano
bazel build //:practice
```

## Run

```bash
bazel run //:practice -- 1
```

Replace `1` with `2` or `3` for harder levels.

## Terminal-Only Testing

Use keyboard mode to test without a MIDI device:

```bash
bazel run //:practice -- 1 --keyboard
```

Type note names per attempt (space or comma separated), for example:

- `C E G`
- `Bb,D,F`
- `F# A# C#`

If you get a chord wrong, the app prints the target semitone tuple and asks again.

## Notes

- Requires macOS CoreMIDI and an available MIDI input source.
- `--keyboard` bypasses CoreMIDI and uses typed note names instead.
- The trainer waits until all keys are released before issuing the next prompt.
