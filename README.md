# Chord Trainer

A terminal-based MIDI ear/shape practice app.

## Features

- Random chord prompts by level
- MIDI input listening via CoreMIDI (MacOS Only)
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
bazel run //:practice -- --level 1
```

Replace `1` with `2` or `3` for harder levels.

Disable audio preview if needed:

```bash
bazel run //:practice -- --level 1 --quiet
```

## Terminal-Only Testing

Use keyboard mode to test without a MIDI device:

```bash
bazel run //:practice -- --level 1 --keyboard
```

Use both flags together for silent terminal-only testing:

```bash
bazel run //:practice -- --level 1 --keyboard --quiet
```

Type note names per attempt (space separated), for example:

- `C E G`
- `Bb D F`
- `F# A# C#`

If you get a chord wrong, the app prints the target semitone tuple and asks again.

## Analyze Mode

Use analyze mode to print detected chords continuously instead of running prompts:

```bash
bazel run //:practice -- --analyze
```

You can combine it with keyboard mode for terminal-only input:

```bash
bazel run //:practice -- --analyze --keyboard
```

## Notes

- Requires macOS CoreMIDI and an available MIDI input source.
- `--keyboard` bypasses CoreMIDI and uses typed note names instead.
- `--quiet` disables the audible chord preview.
- `--analyze` prints detected chord names and note content for each chord you play.
- The trainer waits until all keys are released before issuing the next prompt.
