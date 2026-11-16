"""Regression checks for scientific bookkeeping and causal filtering, not clinical validation.

python Software/GoogleColab/tools/test_notebook_algorithms.py --pulse <04.ipynb> --oxygen-export <replay_reference.py>
"""
import argparse
import importlib.util
import unittest
from pathlib import Path

import numpy as np
import pandas as pd
from scipy import signal
from audit_ecg_reference import definitions

parser = argparse.ArgumentParser()
parser.add_argument('--pulse', type=Path, required=True)
parser.add_argument('--oxygen-export', type=Path, required=True)
args, remaining = parser.parse_known_args()
context = {'np': np, 'WILDPPG_FS': 128, 'signal': signal, 'TRANSFER_STEP_S': 2, 'PERSIST_K': 2,
           'PT_SOS': signal.butter(2, [5., 15.], fs=128, btype='bandpass', output='sos')}
pulse = definitions(args.pulse, ['ecg_beats', 'precision_threshold', 'consecutive', 'start_mask', 'persistent'], context)
spec = importlib.util.spec_from_file_location('oxygen_v5_reference', args.oxygen_export)
oxygen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(oxygen)


class NotebookRegressions(unittest.TestCase):
    def test_ecg_repolarization_and_offset(self):
        # Synthetic beat locations are test inputs, not training/effectiveness labels.
        t = np.arange(16*128)/128
        wave = np.zeros_like(t)
        expected = np.arange(.4, 16, .8)
        for beat in expected:
            wave += np.exp(-.5*((t-beat)/.018)**2)
            wave += .8*np.exp(-.5*((t-beat-.27)/.08)**2)  # broad T wave
        for values in [wave, -wave, 1000+37*wave]:
            self.assertLessEqual(abs(pulse['ecg_beats'](values)-len(expected)), 1)
        self.assertTrue(np.isnan(pulse['ecg_beats'](np.ones(2048))))
        wave[20] = np.nan
        self.assertTrue(np.isnan(pulse['ecg_beats'](wave)))

    def test_gate_threshold_includes_entire_tie(self):
        cutoff = pulse['precision_threshold'](np.array([.8,.8,.2]), np.array([1,0,0]), .9)
        self.assertTrue(np.isinf(cutoff))
        scores, labels = np.array([.9,.8,.8,.2]), np.array([1,1,0,0])
        cutoff = pulse['precision_threshold'](scores, labels, .9)
        self.assertEqual(cutoff, .9)
        self.assertGreaterEqual(labels[scores >= cutoff].mean(), .9)
        self.assertTrue(np.isinf(pulse['precision_threshold'](np.array([]), np.array([]), .9)))

    def test_persistence_respects_persons_and_gaps(self):
        frame = pd.DataFrame({'participant':['a']*4+['b']*3, 'start_s':[0,2,10,12,0,2,4]})
        flags = np.array([True,True,True,False,True,True,False])
        np.testing.assert_array_equal(pulse['persistent'](frame, flags), [False,True,False,False,False,True,True])
        np.testing.assert_array_equal(pulse['start_mask'](frame, flags), [True,False,True,False,True,False,False])

    def test_kalman_is_causal(self):
        x = np.r_[np.full(20,98.), np.linspace(98,55,60), np.full(20,55.)]
        t = np.arange(len(x))
        a = oxygen.replay_kalman(t, x, {'start':80., 'full':60.}, processing_delay_s=1.98)
        x[60:] = 99.
        b = oxygen.replay_kalman(t, x, {'start':80., 'full':60.}, processing_delay_s=1.98)
        for key in ['concern','warning','level','slope']:
            np.testing.assert_array_equal(a[key][:60], b[key][:60])
        with self.assertRaises(ValueError):
            oxygen.replay_kalman(t, x, {'start':80., 'full':60.}, processing_delay_s=15.)

    def test_kalman_gap_does_not_clear_latch(self):
        x = np.r_[np.full(20,55.), np.full(5,np.nan), np.full(50,98.)]
        t = np.arange(len(x))
        a = oxygen.replay_kalman(t, x, {'start':80., 'full':60.})
        self.assertTrue(a['warning'][19:].all())
        self.assertTrue(np.isnan(a['concern'][20:25]).all())
        b = oxygen.replay_kalman(t, x, {'start':80., 'full':60.}, auto_clear=True)
        self.assertFalse(b['warning'][-1])
        # Lost/duplicate timestamps restart the required three seconds of confirmation.
        c = oxygen.replay_kalman(np.array([0,1,9,10,11,12]), np.full(6,55.), {'start':80., 'full':60.})
        np.testing.assert_array_equal(c['warning'], [False,False,False,False,False,True])

    def test_kalman_covariance_matches_matrix_equations(self):
        F, H = np.array([[1.,1.],[0.,1.]]), np.array([1.,0.])
        x, P = np.array([96.,-.1]), np.array([[.4,.03],[.03,.2]])
        q, r, reading = .01, .4, 94.
        predicted = F @ P @ F.T + q*np.array([[1/3,1/2],[1/2,1.]])
        K = predicted @ H / (H @ predicted @ H + r)
        expected_x = F @ x + K*(reading-H @ F @ x)
        expected_P = (np.eye(2)-np.outer(K,H)) @ predicted
        result = oxygen.kalman_step(*x,P[0,0],P[0,1],P[1,1],reading,q,r)
        np.testing.assert_allclose(result[:2], expected_x, rtol=1e-12)
        np.testing.assert_allclose(result[2:5], [expected_P[0,0],expected_P[0,1],expected_P[1,1]], rtol=1e-12)


if __name__ == '__main__':
    unittest.main(argv=['notebook_regressions', *remaining])
