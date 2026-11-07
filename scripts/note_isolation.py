import os
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
import librosa
import soundfile as sf
import numpy as np

MIN_DURATION_SEC = 0.3  # Minimum duration for a note to be considered valid

def detect_note_segments(audio, sr, top_db=20):
    """Split the audio into non-silent chunks."""
    intervals = librosa.effects.split(audio, top_db=top_db)
    return intervals

def estimate_pitch(y, sr):
    """Estimate pitch using librosa's piptrack."""
    pitch_estimation = librosa.yin(y,fmin=50,fmax=5000,sr=sr)
    pitch_estimation = np.median(pitch_estimation)
    # index = magnitudes > np.median(magnitudes)
    # estimated_pitches = pitches[index] 
    # if len(estimated_pitches) == 0:
    #     return "unknown"
    # pitch_hz = np.median(estimated_pitches)
    note_name = librosa.hz_to_note(pitch_estimation,cents=True)
    return str(pitch_estimation).replace('/', '-'), note_name  # ensure valid filenames

def slice_and_save_notes(audio_path, output_dir, top_db=60):
    """Process a single WAV file and save detected note slices."""
    y, sr = librosa.load(audio_path, sr=None)
    #normalize y first
    y = y/np.max(np.abs(y))
    os.makedirs(output_dir, exist_ok=True)

    segments = detect_note_segments(y, sr, top_db=top_db)

    for i, (start, end) in enumerate(segments):
        duration_sec = (end - start) / sr
        if duration_sec < MIN_DURATION_SEC:
            continue
        
        #magic numbers in this slicing to ensure full note is on the file
        note_audio = y[start-500:end+3000]
        #normalize note_audio so that is consistent
        note_audio = note_audio/np.max(np.abs(note_audio))
        pitch_name, note_name = estimate_pitch(note_audio, sr)
        filename = f"note_{i+1}_{note_name}_{pitch_name}.wav"
        filepath = os.path.join(output_dir, filename)
        sf.write(filepath, note_audio, sr)
        print(f"Saved: {filepath}")

def process_folder(input_folder, output_root=REPO_ROOT / "scripts" / "output_notes", top_db=20):
    """Process all WAV files in a folder."""
    if not os.path.isdir(input_folder):
        raise NotADirectoryError(f"{input_folder} is not a valid folder")

    # Create root output folder
    os.makedirs(output_root, exist_ok=True)

    for file in os.listdir(input_folder):
        if file.lower().endswith(".wav"):
            input_path = os.path.join(input_folder, file)

            # Create unique output folder for this file
            file_name_no_ext = os.path.splitext(file)[0]
            out_dir = os.path.join(output_root, file_name_no_ext)

            print(f"\nProcessing {file} ...")
            slice_and_save_notes(input_path, out_dir, top_db=top_db)

    print("\n✔ All files processed!")

if __name__ == "__main__":
    input_folder = REPO_ROOT / "raw_unprocessed_files"
    process_folder(input_folder)
