"""Verify cached ICU windows against the pinned original zip, without fitting models."""
import argparse
import hashlib
import io
import json
import math
import zipfile
from pathlib import Path
import numpy as np
from scipy import io as sio, signal
from audit_ecg_reference import definitions

parser=argparse.ArgumentParser()
parser.add_argument('--notebook',type=Path,required=True)
parser.add_argument('--cache',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
source=args.cache/'training.zip'
digest=hashlib.md5(source.read_bytes()).hexdigest()
assert digest=='99a51fefcd72b584a8122ebd7f78dacf', 'Original training archive checksum mismatch'
ctx=definitions(args.notebook,['read_header','to_device_rate'],dict(np=np,math=math,signal=signal,DEVICE_FS=100))
main=np.load(args.cache/'c2015_ppg_windows.npz',allow_pickle=False)
extra=np.load(args.cache/'c2015_ppg_windows_v5_extra.npz',allow_pickle=False)
np.testing.assert_array_equal(main['record'],extra['record'])
maximum_error=0.
with zipfile.ZipFile(source) as z:
    for i,record in enumerate(main['record']):
        signals=ctx['read_header'](z,record)
        channel=[name for name,_ in signals].index('PLETH')
        raw=sio.loadmat(io.BytesIO(z.read(f'training/{record}.mat')))['val'][channel].astype(float)/signals[channel][1]
        for key,end in [('X',300),('m4',296),('m2',298),('pre',284)]:
            expected=ctx['to_device_rate'](raw[(end-16)*250:end*250],250)[:1600].astype(np.float32)
            cached=(main if key=='X' else extra)[key][i]
            maximum_error=max(maximum_error,float(np.max(np.abs(expected-cached))))
            np.testing.assert_array_equal(expected,cached,err_msg=f'{record}/{key}')
report={'original_zip_md5':digest,'records':len(main['record']),'windows_verified':len(main['record'])*4,
        'maximum_absolute_difference':maximum_error,'exact_match':True}
args.output.parent.mkdir(parents=True,exist_ok=True)
args.output.write_text(json.dumps(report,indent=2)+'\n')
print(report)
