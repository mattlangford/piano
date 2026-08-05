# Chord Trainer

A terminal-based MIDI ear/shape practice app.

## Example

```text
$ bazel run //tutor:practice -- --level 2 --keyboard --log
Chord Trainer (level 2)
Keyboard mode: type notes like C E G or F# A# C#. Ctrl+C to quit.

History logging enabled: /Users/you/Documents/code/piano/.history.csv
Play: Dmaj7
Notes> D F# A C#
Correct in 3.42s

Play: C7
Notes> C E G B
Incorrect. Semitone tuple: (0, 4, 7, 10). Try again.
Notes> C E G Bb
Correct in 11.08s

^C
Session ended after 2 chords.
Score: 2
Average time to respond: 7.25s
Accuracy: 66.7%
Chord breakdown (sorted by wrong %):
	Chord | Attempts | Wrong % | Accuracy | Wrong |      Avg
	----- | -------- | ------- | -------- | ----- | --------
	C7    |        2 |   50.0% |    50.0% |     1 |   11.08s
	Dmaj7 |        1 |    0.0% |   100.0% |     0 |    3.42s
```

## Features

- Random chord prompts by level
- MIDI input listening via CoreMIDI (MacOS Only)
- Exact chord matching by pitch class
- Reports correctness and response time
- Adaptive weighting and stats tracked per exact chord name
- Prevents immediate back-to-back repeats of the same prompted chord
- Continuous rounds until Ctrl+C


## Levels

- Level 1: major, minor
- Level 2: + diminished, augmented, sus2, sus4
- Level 3: + 7, maj7, m7, m7b5

## Build

```bash
cd piano
bazel build //tutor:practice
```

## Run

```bash
bazel run //tutor:practice -- --level 1
```

Replace `1` with `2` or `3` for harder levels.

Disable audio preview if needed:

```bash
bazel run //tutor:practice -- --level 1 --quiet
```

Enable per-question CSV history logging (opt-in):

```bash
bazel run //tutor:practice -- --level 1 --log
```

Use a custom output path if you want to store logs elsewhere:

```bash
bazel run //tutor:practice -- --level 1 --log session_logs/my_practice.csv
```

Equivalent form:

```bash
bazel run //tutor:practice -- --level 1 --log=session_logs/my_practice.csv
```

## Terminal-Only Testing

Use keyboard mode to test without a MIDI device:

```bash
bazel run //tutor:practice -- --level 1 --keyboard
```

Use both flags together for silent terminal-only testing:

```bash
bazel run //tutor:practice -- --level 1 --keyboard --quiet
```

Type note names per attempt (space separated), for example:

- `C E G`
- `Bb D F`
- `F# A# C#`

If you get a chord wrong, the app prints the target semitone tuple and asks again.

## Analyze Mode

Use analyze mode to print detected chords continuously instead of running prompts:

```bash
bazel run //tutor:practice -- --analyze
```

You can combine it with keyboard mode for terminal-only input:

```bash
bazel run //tutor:practice -- --analyze --keyboard
```


## Triangle Synth

The `synth` target currently generates a triangle-wave voice for each MIDI note:

```bash
bazel run //synth:sample_synth
```

The synth has defaults for every option. By default it uses a continuously
synthesized triangle, uses MIDI note 60 as the root, and listens on all MIDI
channels. Each `--sample` adds one equal keyboard zone; the 128 MIDI notes are
divided into those zones in the order provided. Comma-separated values within
one `--sample` are random alternates for that zone:

```bash
bazel run //synth:sample_synth -- \
  --sample bass.mp3 \
  --sample piano.mp3 \
  --sample triangle,square \
  --root-note 60 \
  --channel 1
```

With three samples, notes 0-42 use `bass.mp3`, 43-85 use `piano.mp3`, and
86-127 randomly use the synthesized triangle or square wave. Built-in
waveforms can be selected with `triangle`, `square`, or `sine`/`sin`. For
example, `--sample triangle,square` randomly chooses one for each note in that
zone. Each zone after the first is
transposed one octave down, so C4 in the upper/second zone maps to C3. Holding
a key does not change pitch or playback speed. Releasing a key while
the sustain pedal is down lets a file clip continue until its buffer ends;
releasing the pedal early stops the dry source and leaves its reverb tail. The
active voices sustain while the pedal is held.

Run `bazel run //synth:sample_synth -- --help` for all options. A positional
sample path is also accepted.

Synthesized waveforms and decoded files are peak-normalized to make square,
triangle, sine, and recorded clips play at more comparable levels. A little
headroom is retained for overlapping notes and reverb.

It connects to all available CoreMIDI input sources, uses MIDI note frequency
(A4/MIDI 69 = 440 Hz), responds to velocity, supports overlapping notes, and
releases voices when notes are released. The audio is generated in real time;
no sample file is required.

## Notes

- Requires macOS CoreMIDI and an available MIDI input source.
- `--keyboard` bypasses CoreMIDI and uses typed note names instead.
- `--quiet` disables the audible chord preview.
- `--analyze` prints detected chord names and note content for each chord you play.
- `--log` appends practice results to CSV (chord name, correctness, and seconds to solve).
- `--log` without a path writes to `.history.csv`.
- `--log <path>` writes to the path you provide.
- The trainer waits until all keys are released before issuing the next prompt.
