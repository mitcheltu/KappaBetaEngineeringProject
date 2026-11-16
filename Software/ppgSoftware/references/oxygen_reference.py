# Frozen oxygen concern replay; research reference, not a validated controller.
import math
import statistics
import numpy as np

ANCHOR_SETS = {'clinical_hypoxemia': {'start': 95.0, 'full': 85.0}, 'moderate': {'start': 90.0, 'full': 75.0}, 'deep_breath_hold': {'start': 80.0, 'full': 60.0}}
ENTER_SCORE = 0.5
EXIT_SCORE = 0.25
SMOOTH_S = 3
TAU_S = 3.0
CONFIRM_S = 3
RECOVER_S = 5
REFERENCE_RESTING_SPO2 = 97.0
TREND_HORIZON_S = 15.0
ALPHA = 0.5
BETA = 0.1
DOSE_SECONDS = 10.0
VARIANTS = {'threshold': {'family': 'threshold', 'confirm': 0, 'recover': 0}, 'threshold_persist': {'family': 'threshold', 'confirm': 3, 'recover': 5}, 'concern_level': {'family': 'concern', 'baseline': False, 'trend': False}, 'concern_baseline': {'family': 'concern', 'baseline': True, 'trend': False}, 'concern_trend': {'family': 'concern', 'baseline': True, 'trend': True}, 'deficit_dose': {'family': 'dose'}}

def ramp(u):
    # Linear 0-1 ramp; clipped. Kept linear so every anchor has a direct SpO2 meaning.
    return min(1.0, max(0.0, float(u)))


def contiguous_mask(t, oxygen):
    valid = np.isfinite(t) & np.isfinite(oxygen)
    contiguous = np.r_[False, (np.diff(t) == 1) & valid[1:] & valid[:-1]]
    return valid, contiguous


def replay(t, oxygen, baseline, anchors, variant, auto_clear=False):
    '''Causal replay of one session. Returns per-second concern, warning flag and alert id.
    auto_clear=False (device behaviour): an open alert stays latched until acknowledged outside this function.
    auto_clear=True (evaluation): it clears after RECOVER_S s at or below EXIT_SCORE.'''
    cfg = VARIANTS[variant]
    start, full = anchors['start'], anchors['full']
    midpoint = (start + full) / 2
    valid, contiguous = contiguous_mask(t, oxygen)
    n = len(t)
    concern = np.full(n, np.nan)
    warning = np.zeros(n, bool)
    alert_id = np.full(n, -1)
    window, state, level, slope, dose = [], None, None, 0.0, 0.0
    active, high_since, low_since, sequence, last_t = False, None, None, -1, None
    confirm = cfg.get('confirm', CONFIRM_S)
    recover = cfg.get('recover', RECOVER_S)
    for i in range(n):
        if not contiguous[i]:
            # A gap restarts smoothing/tracking and timers; an open alert stays latched.
            window, level, slope, last_t = [], None, 0.0, None
            high_since = low_since = None
        if not valid[i]:
            warning[i] = active
            alert_id[i] = sequence if active else -1
            continue
        x = float(oxygen[i])
        window = (window + [x])[-SMOOTH_S:]
        if len(window) < SMOOTH_S:
            warning[i] = active
            alert_id[i] = sequence if active else -1
            continue
        smoothed = float(statistics.median(window))   # same value as np.median, much faster on 3 values
        dt = 1.0 if last_t is None else float(t[i] - last_t)
        last_t = t[i]
        if level is None:
            level, slope = smoothed, 0.0
        else:  # alpha-beta tracker on the smoothed signal
            predicted = level + slope * dt
            residual = smoothed - predicted
            level = predicted + ALPHA * residual
            slope = slope + BETA * residual / dt
        if cfg['family'] == 'threshold':
            value = 1.0 if smoothed <= midpoint else 0.0
        elif cfg['family'] == 'dose':
            dose = max(0.0, dose + max(0.0, start - smoothed) - 0.5)   # 0.5 pp/s drift allowance
            if smoothed >= start:
                dose *= 0.8                                              # decay once back above start
            value = min(1.0, dose / ((start - full) * DOSE_SECONDS))
        else:
            severity = ramp((start - smoothed) / (start - full))
            if cfg['baseline'] and baseline is not None:
                shifted_start = start + (baseline - REFERENCE_RESTING_SPO2)   # same anchors, person-relative
                severity = max(severity, ramp((shifted_start - smoothed) / (start - full)))
            if cfg['trend'] and slope < 0:
                projected = level + slope * TREND_HORIZON_S
                severity = max(severity, ramp((start - projected) / (start - full)))
            state = severity if state is None else state + (1 - math.exp(-dt / TAU_S)) * (severity - state)
            value = state
        concern[i] = value
        high, low = value >= ENTER_SCORE, value <= EXIT_SCORE
        if not active:
            low_since = None
            high_since = (t[i] if high_since is None else high_since) if high else None
            if high_since is not None and t[i] - high_since >= confirm:
                active, high_since, sequence = True, None, sequence + 1
        else:
            high_since = None
            low_since = (t[i] if low_since is None else low_since) if low else None
            if auto_clear and low_since is not None and t[i] - low_since >= recover:
                active, low_since = False, None
        warning[i] = active
        alert_id[i] = sequence if active else -1
    return {'concern': concern, 'warning': warning, 'alert_id': alert_id, 'valid': valid}
