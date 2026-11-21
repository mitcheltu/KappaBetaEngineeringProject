"""Re-stratify the original v4 clips without refitting any detector or threshold."""
import argparse
import ast
import json
import math
from pathlib import Path
import joblib
import numpy as np
import pandas as pd
from scipy import io, signal, stats
import torch
from torch import nn
from audit_ecg_reference import definitions

parser = argparse.ArgumentParser()
parser.add_argument('--v4', type=Path, required=True)
parser.add_argument('--v5', type=Path, required=True)
parser.add_argument('--models', type=Path, required=True, help='Trusted local v4 exports only; joblib loading executes code')
parser.add_argument('--clips', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
torch.set_num_threads(1)
cfg = json.loads((args.models/'pulse_loss_configuration.json').read_text())
ctx = dict(np=np, pd=pd, signal=signal, stats=stats, torch=torch, nn=nn, math=math, DEVICE_FS=100, WILDPPG_FS=128,
           BRADY_BEATS=5, BRADY_BPM=40., ASYSTOLE_GAP_S=4., FEATURES=cfg['features'])
ctx['SOS'] = signal.butter(3, [.5,8], fs=100, btype='bandpass', output='sos')
ctx['ECG_SOS'] = signal.butter(3, [5,20], fs=128, btype='bandpass', output='sos')
ctx['PT_SOS'] = signal.butter(2, [5,15], fs=128, btype='bandpass', output='sos')
definitions(args.v4, ['prepare','ppg_features','rule_definitions','to_device_rate','ecg_beats'], ctx)
old_count = ctx['ecg_beats']
new_count = definitions(args.v5, ['ecg_beats'], dict(ctx))['ecg_beats']
nb = json.loads(args.v4.read_text(encoding='utf-8'))
tree = ast.parse('\n'.join(''.join(c['source']) for c in nb['cells'] if c['cell_type']=='code'))
cnn_def = next(node for node in ast.walk(tree) if isinstance(node,ast.ClassDef) and node.name=='PulseCNN')
exec(compile(ast.Module(body=[cnn_def], type_ignores=[]),str(args.v4),'exec'),ctx)
cnn = ctx['PulseCNN']()
cnn.load_state_dict(torch.load(args.models/'pulse_loss_cnn_1d.pt',map_location='cpu',weights_only=True))
cnn.eval()
models = joblib.load(args.models/'pulse_loss_feature_models.joblib')
rows = []
for pid in cfg['false_alarm_data']['participants']:
    raw = io.loadmat(args.clips/f'WildPPG_Part_{pid}.mat', variable_names=['id','sternum'], simplify_cells=True)
    assert str(raw['id']).strip()==pid
    s = raw['sternum']
    waves = ctx['to_device_rate'](-np.asarray(s['ppg_ir']['v'],float),128)
    ecg = np.asarray(s['ecg']['v'],float)
    for start in range(0,len(waves)-1600+1,200):
        wave = waves[start:start+1600]
        if not np.isfinite(wave).all():
            continue
        f,_ = ctx['ppg_features'](wave)
        f = pd.DataFrame([f])
        e = ecg[int(start/100*128):int(start/100*128)+2048]
        scores = {'rule_definitions':ctx['rule_definitions'](f)[0]}
        for name,model in models.items():
            scores[name] = model.predict_proba(f[ctx['FEATURES']].to_numpy(float))[0,1]
        with torch.no_grad():
            scores['cnn_1d'] = torch.sigmoid(cnn(torch.tensor(ctx['prepare'](wave)[None,:],dtype=torch.float32))).item()
        rows.append(dict(participant=pid,start_s=start/100,ppg_beats=f.n_beats.iloc[0],old_ecg=old_count(e),new_ecg=new_count(e),**scores))
    print('Replayed frozen v4:',pid,flush=True)
d = pd.DataFrame(rows)
d.to_csv(args.output/'rechecked_windows.csv.gz',index=False)
report=[]
for reference in ['old_ecg','new_ecg']:
    ratio=d.ppg_beats/d[reference].where(d[reference]>0)
    visible=ratio.between(.8,1.2)
    for model in cfg['models']:
        point='definition' if model=='rule_definitions' else 'high_specificity'
        threshold=.5 if point=='definition' else cfg['final_thresholds'][model][point]
        flag=d[model].ge(threshold)
        previous = flag.shift(fill_value=False) & d.participant.eq(d.participant.shift()) & d.start_s.diff().eq(2)
        starts = flag & ~previous
        agreement_hours, disagreement_hours = visible.sum()*2/3600, (~visible).sum()*2/3600
        report.append(dict(reference=reference,model=model,windows=len(d),median_beat_ratio=float(ratio.median()),
                           agreement_fraction=float(visible.mean()),flagged_fraction=float(flag.mean()),
                           flagged_agreement=float(flag[visible].mean()),flagged_disagreement=float(flag[~visible].mean()),
                           agreement_hours=agreement_hours, disagreement_hours=disagreement_hours,
                           agreement_starts=int(starts[visible].sum()), disagreement_starts=int(starts[~visible].sum()),
                           agreement_starts_per_hour=starts[visible].sum()/agreement_hours,
                           disagreement_starts_per_hour=starts[~visible].sum()/disagreement_hours))
pd.DataFrame(report).to_csv(args.output/'v4_corrected_visibility.csv',index=False)
original = pd.read_csv(args.models/'healthy_false_alarms.csv')
for row in report:
    if row['reference'] != 'old_ecg':
        continue
    point = 'definition' if row['model']=='rule_definitions' else 'high_specificity'
    saved = original[(original.model==row['model']) & (original.operating_point==point) & original.subset.eq('all')]
    assert len(saved)==1
    assert np.isclose(row['flagged_fraction'], saved.flagged_fraction.iloc[0], atol=1e-12), 'Frozen v4 replay changed'
print(pd.DataFrame(report).to_string(index=False))
