"""Compare notebook ECG heuristics on recorded data; autocorrelation is NOT ground truth.

Run from the repository root. Writes only aggregate summaries, selected trace plots,
and private local per-window diagnostics. No physiological outcome labels are created.
"""
import argparse
import ast
import json
from pathlib import Path

import numpy as np
import pandas as pd
from scipy import io, signal


def definitions(path, names, context):
    notebook = json.loads(Path(path).read_text(encoding='utf-8'))
    source = '\n'.join(''.join(c['source']) for c in notebook['cells'] if c['cell_type'] == 'code')
    tree = ast.parse(source)
    selected = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in names]
    assert {node.name for node in selected} == set(names)
    exec(compile(ast.Module(body=selected, type_ignores=[]), str(path), 'exec'), context)
    return context


def periodic_hr(ecg, fs=128):
    """Independent repetition heuristic with an explicit confidence measure; may choose harmonics."""
    if not np.isfinite(ecg).all() or np.ptp(ecg) < 1e-10:
        return np.nan, np.nan
    z = signal.sosfiltfilt(signal.butter(3, [5, 20], fs=fs, btype='bandpass', output='sos'), ecg)
    z -= z.mean()
    ac = signal.correlate(z, z, mode='full', method='fft')[len(z)-1:]
    ac /= np.arange(len(z), 0, -1)
    ac /= max(ac[0], 1e-20)
    low, high = int(.33 * fs), min(int(1.5 * fs), len(z)-1)
    peaks = signal.find_peaks(ac[low:high+1])[0] + low
    if not len(peaks):
        return np.nan, np.nan
    # Earliest local maximum at least 90% as strong as the strongest candidate.
    # This avoids choosing two repeating RR intervals solely for a slightly higher correlation.
    peak = peaks[ac[peaks] >= .9 * ac[peaks].max()][0]
    return 60 * fs / peak, float(ac[peak])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--v5', type=Path, required=True)
    parser.add_argument('--ids', nargs='+', default=['an0', 'e61', 'p5d'])
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    base = Path(__file__).resolve().parents[1]
    context = {'np': np, 'signal': signal, 'REF_FS': 128, 'WILDPPG_FS': 128}
    context['ECG_SOS'] = signal.butter(3, [5., 20.], fs=128, btype='bandpass', output='sos')
    old_path = base/'archive/04_pulse_loss_detector_v4.ipynb'
    if not old_path.exists():
        old_path = base/'04_pulse_loss_detector.ipynb'
    old = definitions(old_path, ['ecg_beats'], context.copy())['ecg_beats']
    context['PT_SOS'] = signal.butter(2, [5., 15.], fs=128, btype='bandpass', output='sos')
    corrected = definitions(args.v5, ['ecg_beats'], context.copy())['ecg_beats']
    nb02 = definitions(base/'02_hr_reliability.ipynb', ['ecg_peaks', 'reference_hr'], context.copy())
    rows, plot_examples = [], []
    for pid in args.ids:
        data = io.loadmat(args.input/f'WildPPG_Part_{pid}.mat', variable_names=['sternum'], simplify_cells=True)
        ecg = np.asarray(data['sternum']['ecg']['v'], float)
        del data
        # Fixed 5-minute blocks at beginning/middle/end, never chosen by detector success.
        n = 300 * 128
        for block, start in enumerate([0, (len(ecg)-n)//2, len(ecg)-n]):
            segment = ecg[start:start+n]
            first, second, valid = nb02['ecg_peaks'](segment)
            for pos in range(128, n-16*128, 3*128):
                wave = segment[pos:pos+16*128]
                auto, strength = periodic_hr(wave)
                h02, status = nb02['reference_hr'](first, second, valid, pos, pos+4*128)
                auto4, strength4 = periodic_hr(segment[pos:pos+4*128])
                rows.append(dict(participant=pid, block=block, start_s=(start+pos)/128,
                                 v4_bpm=old(wave)*60/16, v5_bpm=corrected(wave)*60/16,
                                 periodic_bpm=auto, periodic_strength=strength,
                                 nb02_bpm=h02, nb02_status=status, periodic4_bpm=auto4, periodic4_strength=strength4))
            if block == 0 and len(plot_examples) < 3:
                plot_examples.append((pid, segment[128:9*128], first[(first>=128)&(first<9*128)]-128,
                                      second[(second>=128)&(second<9*128)]-128))
        print('Audited', pid, flush=True)
    frame = pd.DataFrame(rows)
    frame.to_csv(args.output/'ecg_reference_windows.csv.gz', index=False)
    summary = []
    for name, target, strength in [('v4_bpm','periodic_bpm','periodic_strength'),
                                   ('v5_bpm','periodic_bpm','periodic_strength'),
                                   ('nb02_bpm','periodic4_bpm','periodic4_strength')]:
        for cutoff in [0., .5]:
            good = frame[name].notna() & frame[target].notna() & frame[strength].ge(cutoff)
            e = (frame.loc[good,name]-frame.loc[good,target]).abs()
            summary.append(dict(detector=name, min_periodicity=cutoff, windows=int(good.sum()),
                                median_abs_disagreement_bpm=float(e.median()),
                                fraction_within_10_bpm=float(e.le(10).mean()),
                                median_detector_bpm=float(frame.loc[good,name].median()),
                                median_periodic_bpm=float(frame.loc[good,target].median())))
    report = {'caveat': 'Two automatic estimates, not annotated ECG ground truth; cannot establish accuracy.',
              'ids': args.ids, 'windows': len(frame), 'nb02_reference_coverage': float(frame.nb02_bpm.notna().mean()),
              'summary': summary}
    (args.output/'ecg_reference_summary.json').write_text(json.dumps(report, indent=2)+'\n')
    print(pd.DataFrame(summary).to_string(index=False))
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(len(plot_examples), 1, figsize=(12, 3*len(plot_examples)), squeeze=False)
    for ax, (pid, wave, a, b) in zip(axes[:,0], plot_examples):
        ax.plot(np.arange(len(wave))/128, wave, lw=.8, color='black')
        ax.scatter(a/128, wave[a], label='02 amplitude detector', marker='o', s=25, facecolors='none', edgecolors='red')
        ax.scatter(b/128, wave[b], label='02 energy detector', marker='x', s=20, color='blue')
        ax.set(title=f'{pid}: fixed first example, both ECG heuristics', xlabel='Seconds', ylabel='ECG units')
        ax.legend(loc='upper right')
    fig.tight_layout()
    fig.savefig(args.output/'ecg_reference_examples.png', dpi=150)


if __name__ == '__main__':
    main()
