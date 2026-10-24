import os
import librosa
import librosa.display
import numpy as np
import matplotlib.pyplot as plt

def load_and_concatenate(folder):
    """Load all WAV files from a folder and concatenate the audio."""
    audio_all = []
    sr_ref = None

    for file in sorted(os.listdir(folder)):
        if file.lower().endswith(".wav"):
            path = os.path.join(folder, file)
            print("Loading", path)
            audio, sr = librosa.load(path, sr=None)

            if sr_ref is None:
                sr_ref = sr
            elif sr != sr_ref:
                raise ValueError("All WAV files must have the same sample rate.")

            audio_all.append(audio)

    if len(audio_all) == 0:
        raise RuntimeError("No WAV files found in folder.")

    return np.concatenate(audio_all), sr_ref


def generate_chromatic_frequencies():
    """Return dict of MIDI note name → frequency (A0 to C8)."""
    notes = {}
    for midi in range(21, 109):  # A0 (21) to C8 (108)
        freq = librosa.midi_to_hz(midi)
        name = librosa.midi_to_note(midi)
        notes[name] = freq
    return notes


def plot_spectrogram_with_chromatic_lines(audio, sr):
    """Plot spectrogram and overlay chromatic note frequencies."""
    # Compute spectrogram
    S = librosa.stft(audio, n_fft=4096, hop_length=512)
    S_db = librosa.amplitude_to_db(np.abs(S), ref=np.max)

    chroma = generate_chromatic_frequencies()

    plt.figure(figsize=(16, 10))
    librosa.display.specshow(
        S_db,
        sr=sr,
        hop_length=512,
        x_axis='time',
        y_axis='log',
        cmap='magma'
    )

    # Overlay chromatic notes
    for name, freq in chroma.items():
        if freq < sr / 2:  # Only draw frequencies in Nyquist range
            plt.axhline(freq, color='white', alpha=0.35, linewidth=0.7)
            plt.text(
                0, freq, name,
                color='white',
                fontsize=7,
                alpha=0.7,
                verticalalignment='bottom'
            )
    plt.ylim(0,1000)
    plt.title("Spectrogram with Chromatic Note Frequencies")
    plt.colorbar(format="%+2.0f dB")
    plt.tight_layout()
    plt.savefig('check1.png')


def visualize_folder_spectrogram(folder):
    audio, sr = load_and_concatenate(folder)
    plot_spectrogram_with_chromatic_lines(audio, sr)


if __name__ == "__main__":
    input_folder = r"output_notes\register1"  # <-- your folder
    visualize_folder_spectrogram(input_folder)
