# Repository utilities

Run the examples below from the repository root. Python dependencies are separate from the C++ plugin build. Sample-preparation commands write their outputs; they are not needed to compile or use the existing library.

| Script | Purpose / output |
|---|---|
| `rysum_preprocess.py` | Current preparation pipeline: `raw_unprocessed_files/` → `samples/`, including `rysum_samples.json` and analysis images |
| `analyse_sample_pitch.py` | Non-destructive final-WAV audit → `docs/pitch-analysis/` |
| `test_analyse_sample_pitch.py`, `test_pitch_metadata.py` | Synthetic estimator checks and metadata/evidence validation |
| `make_photos.py` | Current image preparation → `plugin/resources/`; reads original photos from `_archive/newpics/` and `_archive/photos/` |
| `make_fabian.py` | Converts a supplied FABIAN SOFA file → `plugin/resources/fabian_hrir.bin` |
| `make_fell.py` | Builds modified Fell fonts from supplied source fonts; currently expects DejaVu donor fonts at Linux system paths |
| `note_isolation.py` | Legacy extraction experiment → ignored `scripts/output_notes/`; does not overwrite archived extraction sets |
| `register_processing_check.py` | Legacy spectrogram inspection; reads `_archive/output_notes_1/register1/` → `scripts/img/check1.png` |

Current sample preparation (rewrites the selected output library):

```sh
python scripts/rysum_preprocess.py --raw raw_unprocessed_files --out samples
```

Pitch analysis and validation:

```sh
python -m pip install -r scripts/pitch-analysis-requirements.txt
python scripts/analyse_sample_pitch.py --samples samples --out docs/pitch-analysis
PYTHONDONTWRITEBYTECODE=1 python -m unittest discover -s scripts -p 'test_*pitch*.py'
```

Sample preparation additionally needs SciPy, SoundFile, librosa and matplotlib. Image preparation needs Pillow and NumPy; FABIAN preparation needs h5py and NumPy; font preparation needs fontTools. None is installed automatically by CMake.

Photo preparation:

```sh
python scripts/make_photos.py \
  _archive/newpics/drive-download-20261030T145157Z-1-001 \
  plugin/resources _archive/photos
```

The default input/output paths for audio preprocessing, legacy inspection and pitch analysis are anchored to the repository location. Explicit relative command-line paths are relative to your current working directory.
