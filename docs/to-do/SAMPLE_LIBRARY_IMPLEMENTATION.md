# Other sample sets and replaceable registers

Status — planned. Reviewed against `plainsound/sample-tuning` on 11 November 2026.
The first import is one single-rank register from the Norrfjärden Church Organ
24-bit sample set. Later stages support replacing a whole organ.

## Existing foundation

The current loader reads `rysum-samples/rysum_samples.json` into eight register
slots with 41 physical keys. Seven registers have their own recordings;
Praestant 16′ derives from Praestant 8′. The loader can decode stereo and 24-bit
source files, but `readMono` averages the channels into a mono emitter.

`SamplePitch.h` provides native/corrected rate calculations. The engine separates
native transposition from sample-rate conversion and applies manifest calibration
per pipe in retuned modes. The separate versioned pitch-metadata sidecar remains
a [draft](../in-progress/PITCH_METADATA_V1.md).

The first reusable interface should connect prepared audio assets and register
mappings to this engine. Preserve the current Rysum loader as a legacy adapter.

## Data model

| Definition | Required information |
| --- | --- |
| Library | Stable ID, format version, revision, name, author, licence and relative asset root |
| Instrument | Recorded keyboard addresses, native tuning reference, register definitions and default layout |
| Audio asset | File/hash, source rate, frames, channel count, raw pitch evidence and frame regions |
| Pipe mapping | Asset ID, original playback ratio, target rank ratio, original key and physical source identity |
| Register | Stable ID, label, footage/ranks, default gain, supported range, transposition limits and pipe mappings |
| Release | Embedded region or separate asset, transition region, own pitch reference and optional selection rules |
| Slot assignment | Host slot ID, library/register ID, revision and player overrides |

Use immutable prepared definitions. Decode, validate and resolve references in a
background job, then publish a complete result at an audio-block boundary.
Retire replaced resources after the engine acknowledges adoption.

Keep eight host slots for the pilot import. Automation IDs such as `stop0` belong
to the slot. The imported register supplies its displayed label and physical
properties. Room motion and 3D rendering currently use `kStopFootage` and register
indices; those dimensions and the stopped/open-pipe distinction must come from
the assigned definition as replacement registers are introduced.

A general instrument manifest can reference the same asset/pitch definitions as
the draft sidecar. Its schema and filename should be agreed during the first
adapter milestone. Record original metadata alongside the interpreted values so
import decisions can be reproduced.

## Tuning and source selection

Originalstimmung uses each source's native playback ratio and recorded range.
Other tunings resolve a target keyboard frequency, then apply the register's rank
ratio and the actual source recording's measured frequency. Sample-rate
conversion is included once. Derived 16′ playback and neighbouring-source
substitution retain the raw asset's pitch evidence.

For an imported organ, read the original pitch reference and key/rank mapping
from its definition. A = 444.5 Hz and the Rysum meantone table are legacy defaults.
Make the imported reference available to Originalstimmung and Testton. Testton's
nominal and measured-reference choices must use the active instrument's compass.

Support explicit per-register sounding bounds and maximum source transposition.
Select a source within those limits and retain its physical location. Wider
virtual extensions should be a deliberate option with their own layout policy.
The existing optional Rysum extended range is a separate player feature.

Compound stops need identified ranks and breakbacks. A combined WAV has one
playback speed; independent rank correction requires suitable separate assets or
an explicitly evaluated decomposition. Start the import with a single rank.

## Source rate, stereo and releases

Preserve 24-bit source resolution through decoding into float. Store loop,
attack and release positions in source frames at the file's own rate. Replace
remaining fixed-48-kHz region calculations along the imported playback path.

The pilot may explicitly use the current mono emitter while retaining the source
stereo files and channel metadata. Optional stereo playback follows the
[stereo signal-path plan](STEREO_SIGNAL_PATH.md).

Represent an embedded tail and a separate release asset through the same region
interface. Inspect whether the source selects releases by note duration, velocity
or another rule. Keep those rules distinct in metadata. The
[release-velocity plan](RELEASE_VELOCITY.md) supplies the note lifecycle and
sustain-to-release transition.

## Import procedure

Build an offline importer for the selected register. It should:

1. Verify package version/checksum, author and licence, and calculate a local SHA-256.
2. Read the instrument definition and inspect actual audio formats, MIDI/key maps,
   embedded pitch, loops, cues and definition-level tuning corrections.
3. Preserve original source metadata and express each applied interpretation once.
4. Produce relative asset references, prepared pipe/register definitions and a
   validation report with unresolved fields.
5. Provide an audition list covering low, middle and high notes, attack, loop,
   release and corrected tuning.

The first importer targets the relevant GrandOrgue fields for one register.
Retain additional releases, subsemitone identities and source channels for later
support. Keep downloaded archives and extracted recordings outside this Git
repository, for example `~/Dev/sample-libraries/norrfjarden/`.

## Staged implementation

| Stage | Deliverable | Completion check |
| --- | --- | --- |
| 1 | Legacy Rysum adapter and shared asset/register types | Existing recorded and retuned playback matches the baseline, including substitutions and 16′ |
| 2 | Instrument-aware source regions and rate handling | Native 44.1/48/96 kHz assets keep their timing and pitch at several host rates |
| 3 | Inspect Norrfjärden and prepare one register | Package provenance, codec, mapping, pitch, loops and releases recorded |
| 4 | Assign the register to one existing slot | Replacement plays with correct labels, rank, gain, source positions and tuning |
| 5 | Save/restore assignments and replace resources live | Old projects load Rysum, imported projects resolve exact library/register IDs, missing assets are reported |
| 6 | General instrument compass and defaults | Whole-organ replacement drives note availability, native tuning, Testton and layouts from metadata |
| 7 | Extend stereo and release selection | Channels, release rules and spatial output pass the dedicated plans' checks |

Hauptwerck Principal 8′ is the initial candidate because it permits a direct
comparison with Rysum Praestant. Hauptwerck Trommeten 8′ is an alternative for
comparing reed response. Confirm the actual register names and mappings when
inspecting the archive.

## State and validation

Store assignments and library revisions in host projects. Decide explicitly
whether sound presets also change assignments; keep this choice separate from
loading tuning with a preset. Resolve a missing library through a folder picker
without silently substituting another organ. Existing projects without assignment
metadata use the Rysum adapter.

Validate relative paths, IDs, asset references, hashes, finite positive frequencies,
frame bounds and permitted transpositions before publication. Define how a project
adopts revised calibration so an updated file cannot silently change recalled tuning.

Tests should cover mixed file rates, single ranks, derived footage, substitutions,
Scala/MTS/MPE, held notes, rapid retriggers, register changes with active tails,
paused audio during library replacement, missing files and older projects. Use
synthetic signals for strict pitch assertions and real recordings for region and
listening checks.

## Source research

The following source notes were collected on 8 November 2026. Full archives have
yet to be downloaded and inspected. Verify their current release and checksum
before the import milestone.

### Located distributions

Use Lars Palo's full GrandOrgue release as the reference source. His [instrument page](https://familjenpalo.se/vpo/about-my-samplesets/norrfjarden-church/) specifies 44.1 kHz, 24-bit stereo audio, multiple loops/releases and CC BY-SA 4.0. The [download listing](https://familjenpalo.se/vpo/download/) identifies version 20230618, approximately 2 GB, with published MD5 `0936959d9901c3726ce106e3eb119c61`. The direct package is [NorrfjardenChurch.orgue](https://familjenpalo.se/sites/default/files/sampleset/packages/NorrfjardenChurch.orgue).

The author describes corrected embedded sample-pitch information, temperament and some release cues in his [June 2023 update](https://github.com/GrandOrgue/grandorgue/discussions/1379). Avoid an unidentified older copy bearing the same filename. Published checksum verification identifies the expected package; also calculate a local SHA-256 for our import provenance.

[Augustine's Hauptwerk adaptation](https://hauptwerk-augustine.info/Norrfjarden.php) is a second distribution of Lars's recordings, not an independent recording session. It provides a [download page](https://kdrive.infomaniak.com/app/share/669203/9e6df9b9-8774-4bca-91f4-4c48cca2f6c6/files/28929). Its publisher describes multiple releases and loops, and simplified D-sharp/E-flat mapping in the Hauptwerk definition. This can be useful for comparison, but the original GrandOrgue definition is preferable for preserving source mapping.

A [2012 message by Lars Palo](https://sourceforge.net/p/ourorgan/mailman/message/29225802/) describes an unfinished multi-attack Norrfjärden demo at `http://www.palohb.com/musik/test/norrfjarden.7z`. This is historical context, not the recommended full set; current availability is not verified.

The full archives have not been downloaded or inspected in this review. The direct GrandOrgue link was located, but its roughly 2 GB contents exceed the web reader's limit. Do not assume the files' actual codecs, embedded metadata, ODF adjustments or release-selection rules until inspected. Multiple releases are not automatically release-velocity layers.
