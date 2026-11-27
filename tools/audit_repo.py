"""Offline audit of public source contents. Does not access credentials or radio."""
from pathlib import Path
import ast, hashlib, json, re, sys, subprocess

ROOT=Path(__file__).resolve().parents[1]
visible=None
if (ROOT/'.git').exists():
    names=subprocess.check_output(['git','-C',str(ROOT),'ls-files','-z','--cached','--others','--exclude-standard'])
    visible={v.decode('utf-8') for v in names.split(b'\0') if v}
bad=[];files=[]
for p in ROOT.rglob('*'):
    rel=p.relative_to(ROOT)
    if not p.is_file() or '.git' in rel.parts:continue
    if visible is not None and rel.as_posix() not in visible:continue
    if any(v in ('__pycache__','inputs','dist','local-reports','release-assets') for v in rel.parts):
        bad.append((rel.as_posix(),'local-only file is included'))
        continue
    files.append(p)
    if p.suffix.lower() in {'.img','.pak','.apk','.ipa','.elf','.so','.dll','.exe','.p12','.pem','.key','.keystore','.jks','.zip','.bin'}:
        bad.append((rel.as_posix(),'unintended binary or credential file'))
    if p.suffix.lower() not in {'.py','.ps1','.md','.txt','.json','.yml','.yaml','.h','.c','.s','.ld','.js','.cjs','.html','.conf'}:continue
    text=p.read_text(encoding='utf-8-sig')
    # Detect actual local provenance or secret material, not harmless placeholder domains.
    checks=[r'[A-Za-z]:[\\/]+Users[\\/]',r'codex-remote-attachments',r'[A-Za-z0-9.-]+\.chatgpt\.site',
            r'download\.sinjet\.cn',r'static/aliyun/upgrade',r'-----BEGIN (?:RSA |OPENSSH |EC )?PRIVATE KEY-----',
            r'gh[pousr]_[A-Za-z0-9]{20,}',r'github_pat_[A-Za-z0-9_]{30,}']
    if p.resolve()==Path(__file__).resolve():checks=[]  # Rule definitions are not captured data.
    for pat in checks:
        if re.search(pat,text,re.I):bad.append((rel.as_posix(),'private data pattern: '+pat))
    if p.suffix=='.py':
        try:ast.parse(text,filename=str(rel))
        except SyntaxError as e:bad.append((rel.as_posix(),f'Python syntax: {e.lineno} {e.msg}'))
    if p.suffix.lower() in {'.c','.h'}:
        for include in re.findall(r'^\s*#\s*include\s+"([^"]+)"',text,re.M):
            if not (p.parent/include).is_file():bad.append((rel.as_posix(),'missing include: '+include))
    if p.suffix=='.md':
        links=re.findall(r'\]\(([^)]+)\)',text)
        links+=re.findall(r'\b(?:href|src)="([^"]+)"',text)
        for link in links:
            link=link.split('#',1)[0]
            if link and not re.match(r'[a-z]+:',link,re.I) and not (p.parent/link).exists():
                bad.append((rel.as_posix(),'missing Markdown target: '+link))
manifest=json.loads((ROOT/'assets/hud-icons/manifest.json').read_text(encoding='utf-8'))
new=0
for row in manifest:
    p=ROOT/f"assets/hud-icons/{row['theme']}/turnArrow_{row['ble_id']}.png"
    if row['copied_stock'] is None:
        new+=1
        if not p.is_file() or hashlib.sha256(p.read_bytes()).hexdigest()!=row['sha256']:
            bad.append((str(p.relative_to(ROOT)),'PNG digest mismatch'))
    elif p.exists():bad.append((str(p.relative_to(ROOT)),'stock-reused PNG should be external'))
assert new==131 and len(manifest)==138
result=dict(files=len(files),python_sources=sum(p.suffix=='.py' for p in files),generated_pngs=new,
            stock_reused_slots=len(manifest)-new,errors=bad,radio_accessed=False,network_accessed=False)
print(json.dumps(result,ensure_ascii=False,indent=2))
raise SystemExit(bool(bad))
