"""Replace build outputs in a matching 0.82 runtime template; keep dependencies intact."""
from pathlib import Path
import argparse
import hashlib
import zipfile

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--template',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--asi',type=Path,default=Path(__file__).resolve().parents[1]/'RDR2VulkanFG.asi')
parser.add_argument('--interposer',type=Path)
parser.add_argument('--diagnostics',type=Path)
args=parser.parse_args()
if args.output.exists() or args.output.resolve()==args.template.resolve():
    parser.error('Choose a new output path; the template and existing archives are preserved.')
replacements={'RDR2VulkanFG.asi':args.asi.read_bytes()}
if args.interposer:replacements['RDR2VulkanFG/Streamline/sl.interposer.dll']=args.interposer.read_bytes()
if args.diagnostics:replacements['RDR2VulkanFG/RDR2VulkanFGDiagnostics.dll']=args.diagnostics.read_bytes()
with zipfile.ZipFile(args.template) as original:
    names=original.namelist()
    if 'README-0.82.txt' not in names or len(names)!=len(set(names)):
        parser.error('Expected a matching 0.82 runtime template with unique entries.')
    ini=original.read('RDR2VulkanFG.ini').decode('utf-8-sig')
    for value in ('DebugLogging=0','Diagnostics=0','IntervalLogging=0','EnableExperimentalRTX20=0'):
        if value not in ini:parser.error('Runtime template has unexpected default settings.')
    for line in ini.splitlines():
        if not line.lstrip().startswith(';') and line.strip().startswith(('Developer','DevTest','Simulate')):
            parser.error('Runtime template contains developer or simulation keys.')
    if any(n.endswith(('.exe','.pdb','.log','.jsonl')) for n in names):
        parser.error('Runtime template contains development artifacts.')
    with zipfile.ZipFile(args.output,'x',zipfile.ZIP_DEFLATED) as result:
        for name in names:result.writestr(name,replacements.get(name,original.read(name)))
with zipfile.ZipFile(args.output) as result:
    assert result.testzip() is None
    for name,content in replacements.items():assert result.read(name)==content
print(args.output)
print('SHA256:',hashlib.sha256(args.output.read_bytes()).hexdigest())
