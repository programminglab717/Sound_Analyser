# 01 — Product Brief

## 1. The problem

Most audio problems are not creative problems, they are *damage* problems. A
recording has hum from a bad ground, a click from a dropped packet, a room that
sounds like a bathroom, a lavalier rubbing on a shirt, a voice buried under air
conditioning. The people with these problems are podcasters, video editors,
YouTubers, archivists, students, field recordists, musicians recording at home,
and small post houses.

The tools available to them split badly:

- **Audacity** is free and everywhere, but its analysis is primitive and its
  repair tools are 2005-era. You cannot see a click; you cannot select a
  harmonic; its noise reduction is a single spectral-subtraction algorithm.
- **iZotope RX** is the professional answer and it is excellent. It also starts
  around $400 and reaches $1200 — more than most of its would-be users earn from
  audio in a year.
- **Adobe Audition** requires a Creative Cloud subscription and drags in an
  ecosystem commitment.
- **Sonic Visualiser** analyses beautifully and edits nothing.

**There is no good free spectral repair tool.** That gap is the product.

## 2. The thesis

> Make the analysis the editing surface.

Every editor draws a waveform and buries analysis in a panel. A waveform cannot
show you a 60 Hz hum, a 12 kHz whine, a lossy-codec cutoff, or a sibilant peak.
A spectrogram shows all four at a glance. If the spectrogram is the canvas — if
you can select a region of *time and frequency* and act on it — then diagnosis
and repair become the same gesture.

Three commitments follow from this:

1. **The spectrogram must be as fast as the waveform.** Pan and zoom at 60 fps
   over a two-hour file, or the thesis collapses. This is a hard engineering
   constraint, not a nice-to-have, and it drives the architecture (see
   [03 — Architecture](03-architecture.md)).
2. **The measurements must be correct.** An "analyser" whose LUFS reading is
   wrong is worse than useless — it is actively harmful. We conform to
   ITU-R BS.1770 / EBU R128 / IEC 61260 and we test against their published
   vectors in CI.
3. **It runs on your machine.** People edit confidential audio: legal
   recordings, unreleased music, medical dictation, private interviews. Local
   inference is a feature, not an implementation detail.

## 3. Who it is for

### Primary — "The Restorer"
A dialogue editor, podcast producer, archivist, or video editor handed a
recording that is already broken. They cannot re-record it. They need hum, hiss,
clicks, plosives, reverb and mouth noise gone, and they need it to still sound
like a human being afterwards.
*Success looks like:* a two-minute fix that used to take forty minutes of manual
work, or was simply impossible.

### Primary — "The Compliance Deliverer"
A creator who has to hit a number: −14 LUFS for Spotify, −16 for Apple Music,
−23 for EBU broadcast, −1 dBTP true peak. They currently guess, or use a web
tool, or get their upload normalised in a way they did not choose.
*Success looks like:* a single panel that says pass/fail against every target
platform, and a one-click conform.

### Secondary — "The Measurer"
Acousticians, AV installers, hardware QA engineers, hobbyist speaker builders,
bioacoustics researchers. They need RT60, octave bands, impulse responses,
THD+N, and a report they can hand to a client.
*Why they matter:* we get 80% of their needs almost free from the analysis
engine the Restorer already requires. Low marginal cost, high willingness to pay,
and they are credibility-building users.

### Explicitly **not** the target (for v1)
Music producers wanting a DAW. We are not competing with Reaper, Ableton or
Pro Tools. Multitrack arrangement, MIDI, virtual instruments and mixing consoles
are out of scope. We host plugins and we edit audio extremely well; we do not
sequence music. Saying no to this is what makes v1 shippable.

## 4. Competitive landscape

| Product | Price | Strength | Weakness we exploit |
| --- | --- | --- | --- |
| iZotope RX | $400–1200 | Best-in-class algorithms | Price; module sprawl; not a full editor |
| Adobe Audition | Subscription | Solid editor + spectral | Subscription-only; ecosystem lock-in |
| Steinberg SpectraLayers | ~$300 | True spectral layers | Price; narrow; weak metering |
| Audacity | Free | Ubiquity, familiarity | Primitive analysis; dated UX; trust damage |
| Sonic Visualiser | Free | Excellent analysis | Cannot edit |
| Web tools (Auphonic etc.) | Freemium | Zero install | Upload required; privacy; no fine control |

**Nobody occupies "free + professional analysis + spectral repair."** That is the
position we take.

## 5. Why free-first is strategically right here

The repair market is gated by price, not by demand. Every Audacity user has a
recording they gave up on. Free removes the acquisition barrier entirely, and
the product is inherently demonstrable — a before/after spectrogram is the best
marketing asset in this category and it costs nothing to produce.

Two conditions make it work:

1. **The free tier must be genuinely excellent**, not crippled. It is the
   marketing budget. A hobbled free tier buys resentment instead of advocacy.
2. **The freemium intent must be stated from day one.** Retrofitting paywalls
   onto a product users believed was free forever is how you generate a backlash.
   Announce the plan in the first release notes, and grandfather early users into
   the Pro tier permanently. Grandfathering costs almost nothing and buys the
   loudest advocates you will ever have.

## 6. Business model phasing

### Stage 1 — Free (launch → ~12 months)
Everything unlocked. Objective is usage, feedback and a corpus of real-world
problem audio. Revenue $0, which keeps us inside the JUCE Starter tier at no
cost (see [05 — Licensing](05-licensing-and-dependencies.md)).

### Stage 2 — Freemium
Proposed split. **Gate on capability, never on who the user is** — "commercial
use licences" for a desktop tool are unenforceable and merely insult honest
users.

| Free forever | Pro |
| --- | --- |
| Full editor, undo, sessions | Batch processing & watch folders |
| Waveform + GPU spectrogram | Spectral layers (multi-layer editing) |
| **All** metering & analysis | Advanced ML: de-reverb, ML de-noise, separation |
| Loudness compliance check + conform | VST3 / CLAP plugin hosting |
| Basic repair: de-hum, de-click, noise profile | Measurement & reporting module (RT60, IR, PDF reports) |
| All format import/export | Scripting / CLI automation |
| Speech transcription (CPU) | Surround & multichannel |

Rationale: the free column is everything an individual needs to fix one file.
The Pro column is everything you need to fix *a hundred* files, or to bill a
client for the result. That is a boundary users find fair, because it tracks the
point at which the tool starts earning them money.

### Pricing note
Do not decide the number now — decide it from Stage 1 telemetry (opt-in) and
user interviews. Anchor expectation against RX Elements (~$129) rather than RX
Standard. A perpetual licence with 12 months of updates tends to be better
received in this market than a pure subscription, and it is what the
anti-Adobe sentiment in the audience is asking for.

## 7. Positioning on trust

Audacity's 2021 telemetry episode is instructive: an audio tool's users are
unusually privacy-sensitive and unusually willing to organise against a
perceived betrayal. Concrete commitments to make and keep:

- Telemetry is **opt-in**, off by default, with a visible plain-language
  description of every field collected.
- Audio never leaves the machine. No cloud inference in the default path.
- Crash reports require explicit per-crash consent and never include audio.
- No account required to use the free tier.

These are cheap to honour now and enormously expensive to retrofit.

## 8. Naming

`Sound Analyser` is a working title — descriptive, unprotectable, and hard to
search for. Worth resolving before any public beta, since the name lands in the
binary, the installer, the file associations and the domain. Candidate
directions: a coined single word (Sonoscope, Auralis, Resonant), or a plain
compound that owns the category (Spectral Studio). Requirement: check the
trademark register and the .com before falling in love with one.
