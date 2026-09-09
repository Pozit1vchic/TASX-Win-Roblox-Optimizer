import os
folder = 'Infra'
desktop = os.path.join(os.environ['USERPROFILE'], 'Desktop')
out = os.path.join(desktop, 'infra_code.txt')
with open(out, 'w', encoding='utf-8') as f:
    for fn in sorted(os.listdir(folder)):
        fp = os.path.join(folder, fn)
        if os.path.isfile(fp):
            f.write(fn + ':\n')
            with open(fp, 'r', encoding='utf-8', errors='ignore') as src:
                f.write(src.read())
            f.write('\n\n')
print('written to', out)
