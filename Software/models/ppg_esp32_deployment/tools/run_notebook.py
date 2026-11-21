"""Extract a notebook's code into a readable local QA script; no training starts here.

python Software/GoogleColab/tools/run_notebook.py notebook.ipynb tmp/run_notebook.py
Then execute the script with the notebook's documented environment variables.
"""
import json
import sys
from pathlib import Path

notebook = json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))
source = '\n\n'.join(''.join(c['source']) for c in notebook['cells'] if c['cell_type'] == 'code')
compile(source, sys.argv[1], 'exec')
output = Path(sys.argv[2])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(source, encoding='utf-8')
print('Script written:', output)
