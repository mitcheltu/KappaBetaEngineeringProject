"""Collect aggregate audit/results tables; never copies waveform or per-window data."""
import argparse
import hashlib
import json
import math
import shutil
from pathlib import Path
import pandas as pd

parser=argparse.ArgumentParser()
parser.add_argument('--pulse',type=Path,required=True)
parser.add_argument('--oxygen',type=Path,required=True)
parser.add_argument('--audit-root',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
args.output.mkdir(parents=True,exist_ok=True)
configuration=json.loads((args.pulse/'pulse_loss_configuration.json').read_text())
assert not configuration['quick_smoke_test'], 'Smoke-test results cannot become the main report'
cv=pd.read_csv(args.pulse/'cv_results.csv')
fa=pd.read_csv(args.pulse/'healthy_false_alarms.csv')
icu=pd.read_csv(args.pulse/'persistence_icu.csv')
points=['definition','high_specificity']
summary=cv[cv.operating_point.isin(points)][['model','operating_point','AUROC','sensitivity','specificity']].copy()
for config,label in [('single window','ungated'),('prior-window gate','gated')]:
    f=fa[fa.configuration.eq(config)&fa.subset.eq('all')&fa.operating_point.isin(points)]
    f=f[['model','operating_point','per_hour','flagged_fraction','monitored_fraction','hours']]
    f=f.rename(columns={k:f'{label}_{k}' for k in ['per_hour','flagged_fraction','monitored_fraction','hours']})
    summary=summary.merge(f,on=['model','operating_point'],validate='one_to_one')
g=icu[icu.configuration.eq('prior-window gate')&icu.operating_point.isin(points)]
summary=summary.merge(g[['model','operating_point','sensitivity']].rename(columns={'sensitivity':'gated_sensitivity'}),
                      on=['model','operating_point'],validate='one_to_one')
summary.to_csv(args.output/'pulse_model_comparison.csv',index=False)
# Show the sensitivity cost alongside the false-alarm metric used by the rule.
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

positions=np.arange(len(summary))
fig,axes=plt.subplots(1,2,figsize=(13,6),sharey=True)
for ax,raw,gated,scale,xlabel in [
    (axes[0],'sensitivity','gated_sensitivity',100,'ICU sensitivity (%)'),
    (axes[1],'ungated_per_hour','gated_per_hour',1,'Healthy alert starts / monitored hour'),
]:
    ax.barh(positions-.18,summary[raw]*scale,height=.34,label='Ungated',color='#356FA3')
    ax.barh(positions+.18,summary[gated]*scale,height=.34,label='Prior-window gate',color='#C96C32')
    ax.set_xlabel(xlabel)
    ax.grid(axis='x',alpha=.2)
    ax.set_axisbelow(True)
    ax.spines[['top','right']].set_visible(False)
axes[0].set_yticks(positions,summary.model)
axes[0].invert_yaxis()
axes[0].set_xlim(0,100)
axes[0].legend(loc='lower right')
coverage=summary.gated_monitored_fraction.iloc[0]
fig.suptitle('Pulse v5: inspected development comparison',fontweight='bold')
fig.text(.5,.02,
         f'Rule definition or learned high-specificity point. Gate monitors {coverage:.1%} of healthy recording time.\n'
         'ICU and healthy recordings are different populations; these results do not validate drowning detection.',
         ha='center',fontsize=9)
fig.tight_layout(rect=(0,.09,1,.95))
fig.savefig(args.output/'pulse_tradeoffs.png',dpi=150,bbox_inches='tight')
plt.close(fig)

timing=pd.read_csv(args.oxygen/'uw_real_desaturation_timing.csv')
oxygen=timing[timing.anchors.eq('deep_breath_hold')].groupby(['variant','reference_crossing']).agg(
    device_traces=('warned','size'),warned=('warned','sum'),median_warning_minus_reference_s=('warning_minus_reference_crossing_s','median'),
    latest_warning_minus_reference_s=('warning_minus_reference_crossing_s','max'))
oxygen.to_csv(args.output/'oxygen_clinical_timing.csv')
copies={
    'pulse_cv_results.csv':args.pulse/'cv_results.csv',
    'pulse_cv_per_alarm_type.csv':args.pulse/'cv_per_alarm_type.csv',
    'pulse_healthy_false_alarms.csv':args.pulse/'healthy_false_alarms.csv',
    'pulse_persistence.csv':args.pulse/'persistence_icu.csv',
    'pulse_icu_gate_pass.csv':args.pulse/'icu_gate_pass.csv',
    'pulse_gate_performance.csv':args.pulse/'gate_performance.csv',
    'pulse_polarity_check.csv':args.pulse/'polarity_check.csv',
    'firmware_selection.json':args.pulse/'firmware_selection.json',
    'pulse_run_plan.json':args.pulse/'v5_run_plan.json',
    'wildppg_source_manifest.json':args.pulse/'wildppg_source_manifest.json',
    'oxygen_fit.json':args.oxygen/'kalman_fit.json',
    'oxygen_anchor_audit.json':args.oxygen/'anchor_audit.json',
    'oxygen_control_burden.csv':args.oxygen/'heldout_control_burden.csv',
    'oxygen_noise_robustness.csv':args.oxygen/'noise_robustness.csv',
    'ecg_reference_summary.json':args.audit_root/'ecg_audit_all/ecg_reference_summary.json',
    'v4_corrected_visibility.csv':args.audit_root/'v4_recheck/v4_corrected_visibility.csv',
    'pulse_cache_verification.json':args.audit_root/'pulse_cache_verification.json',
}
for name,source in copies.items():
    shutil.copyfile(source,args.output/name)
source_artifacts={f'{label}/{p.name}':hashlib.sha256(p.read_bytes()).hexdigest()
                  for label,root in [('pulse',args.pulse),('oxygen',args.oxygen)] for p in root.iterdir() if p.is_file()}
manifest={'pulse_configuration':configuration,'oxygen_configuration':json.loads((args.oxygen/'concern_configuration.json').read_text()),
          'source_artifact_sha256':source_artifacts,
          'executed_script_sha256':{name:hashlib.sha256((args.audit_root/name).read_bytes()).hexdigest()
                                   for name in ['run04_v5.py','run03_v5_final.py']},
          'report_note':'Aggregates only; full local outputs retain models, waveforms provenance and per-window diagnostics.'}
def finite_json(value):
    if isinstance(value,dict): return {k:finite_json(v) for k,v in value.items()}
    if isinstance(value,list): return [finite_json(v) for v in value]
    if isinstance(value,float) and not math.isfinite(value): return None
    return value
for p in args.output.glob('*.json'):
    p.write_text(json.dumps(finite_json(json.loads(p.read_text(encoding='utf-8'))),indent=2,allow_nan=False)+'\n',encoding='utf-8')
(args.output/'run_manifest.json').write_text(json.dumps(finite_json(manifest),indent=2,allow_nan=False)+'\n',encoding='utf-8')
print(summary.round(4).to_string(index=False))
print(oxygen.round(2).to_string())
