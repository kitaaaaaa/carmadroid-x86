"""Add a converted car to the Android game's roster (in an unpacked game folder, see --extract-data).

usage: addcar.py <game dir> <vehicle folder> <NAME> "<Car display name>" "<Driver name>"
                 [--template BlkEagle] [--specs "Defence,Offence,Power,Softness,BHP/ton,TopSpeed,Mass,0-60"]

Registers the car in QUICKRACECARS.TXT, CARSPECS.TXT and the text files, and gives it placeholder menu
pictures and a damage HUD layout copied from the template car. Safe to run again (updates in place).
"""
import argparse, os, re, shutil


def content_dir(game_dir):
    return os.path.join(game_dir, 'DATA', 'CONTENT')


def read_lines(path):
    return open(path, 'rb').read().decode('latin1').replace('\r\n', '\n').split('\n')


def write_lines(path, lines):
    open(path, 'wb').write('\r\n'.join(lines).encode('latin1'))


def add_to_list(path, name):
    lines = [l for l in read_lines(path)]
    if any(l.strip().lower() == name.lower() for l in lines):
        return False
    while lines and not lines[-1].strip():
        lines.pop()
    lines.append(name)
    write_lines(path, lines)
    return True


def set_opponent(path, name, strength):
    """OPPONENTLIST.TXT: car name, then its strength (1-5) on the next line."""
    lines = read_lines(path)
    names = [l.split('//')[0].strip().lower() for l in lines]
    if name.lower() in names:
        i = names.index(name.lower())
        lines[i + 1] = str(strength)
    else:
        while lines and not lines[-1].strip():
            lines.pop()
        lines += [name, str(strength)]
    write_lines(path, lines)


def set_specs(path, name, template, specs):
    lines = read_lines(path)
    header = lines[0].split('\t')
    rows = {l.split('\t')[0].lower(): i for i, l in enumerate(lines) if l.strip()}
    tmpl = lines[rows[template.lower()]].split('\t')
    row = [name] + tmpl[1:]
    if specs:
        cols = ['Defence', 'Offence', 'Power', 'Softness', 'BHP/ton', 'Top Speed', 'Mass', 'NaughtToSixty']
        for col, val in zip(cols, specs.split(',')):
            if val.strip():
                row[header.index(col)] = val.strip()
    text = '\t'.join(row)
    if name.lower() in rows:
        lines[rows[name.lower()]] = text
    else:
        while lines and not lines[-1].strip():
            lines.pop()
        lines.append(text)
    write_lines(path, lines)


def set_text_txt(path, key, value):
    lines = read_lines(path)
    tag = '[%s]' % key
    if tag in [l.strip() for l in lines]:
        i = [l.strip() for l in lines].index(tag)
        lines[i + 1] = value
    else:
        while lines and not lines[-1].strip():
            lines.pop()
        lines += ['', tag, value]
    write_lines(path, lines)


def set_text_xml(path, key, value, template_key):
    """TEXT.XML is a spreadsheet: one <Row> per key, a cell per language. Copy the template key's row."""
    s = open(path, 'rb').read().decode('utf-8')
    row_re = r'<Row>(?:(?!</Row>).)*?<Data ss:Type="String">%s</Data>.*?</Row>'
    esc = value.replace('&', '&amp;').replace('<', '&lt;')
    m = re.search(row_re % re.escape(key), s, re.S)
    if m:
        start, end = m.span()
        row = m.group()
    else:
        t = re.search(row_re % re.escape(template_key), s, re.S)
        if not t:
            print('  TEXT.XML: template key %s not found' % template_key)
            return
        start = end = t.end()
        row = t.group()
    cells = re.findall(r'<Cell[^>]*>.*?</Cell>', row, re.S)
    new_cells = []
    for i, c in enumerate(cells):
        if i == 0:
            c = re.sub(r'(<Data ss:Type="String">).*?(</Data>)', r'\g<1>%s\g<2>' % key, c)
        elif i >= 2:
            c = re.sub(r'(<Data ss:Type="String">).*?(</Data>)', lambda mm: mm.group(1) + esc + mm.group(2), c)
        new_cells.append(c)
    indent = row[len('<Row>'):row.index('<Cell')]
    new_row = '<Row>' + indent + indent.join(new_cells) + row[row.rindex('</Cell>') + 7:]
    if m:
        s = s[:start] + new_row + s[end:]
    else:
        s = s[:start] + '\r\n   ' + new_row + s[end:]
    open(path, 'wb').write(s.encode('utf-8'))


def copy_placeholders(content, name, template):
    n = 0
    for dp, dn, fn in os.walk(os.path.join(content, 'UI')):
        for f in fn:
            base, ext = os.path.splitext(f)
            if base.upper() == template.upper() and ext.upper() == '.IMG':
                dst = os.path.join(dp, name.upper() + '.IMG')
            elif base.upper() == template.upper() + '_LAYOUT' and ext.upper() == '.LOL':
                dst = os.path.join(dp, name.upper() + '_LAYOUT.LOL')
            else:
                continue
            if not os.path.exists(dst):
                shutil.copyfile(os.path.join(dp, f), dst)
                n += 1
    return n


MAX_CARS = 64  # carmadroid raises the game's 40-car tables to 64 (src/roster.cpp)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game_dir')
    ap.add_argument('vehicle')
    ap.add_argument('name')
    ap.add_argument('display')
    ap.add_argument('driver')
    ap.add_argument('--template', default='BlkEagle')
    ap.add_argument('--specs')
    ap.add_argument('--opponent', type=int, help='also race it as an AI opponent with this strength (1-5)')
    a = ap.parse_args()
    c = content_dir(a.game_dir)
    dst = os.path.join(c, 'VEHICLES', a.name.upper())
    if os.path.abspath(a.vehicle) != os.path.abspath(dst):
        shutil.rmtree(dst, ignore_errors=True)
        shutil.copytree(a.vehicle, dst)
    print('vehicle ->', dst)
    print('QUICKRACECARS:', 'added' if add_to_list(os.path.join(c, 'QUICKRACECARS.TXT'), a.name) else 'already there')
    set_specs(os.path.join(c, 'CARSPECS.TXT'), a.name, a.template, a.specs)
    print('CARSPECS: set')
    if a.opponent:
        set_opponent(os.path.join(c, 'OPPONENTLIST.TXT'), a.name, a.opponent)
        print('OPPONENTLIST: set')
    up = a.name.upper()
    set_text_txt(os.path.join(c, 'TEXT.TXT'), 'CAR_' + up, a.display.upper())
    set_text_txt(os.path.join(c, 'TEXT.TXT'), up + '_DRIVER', a.driver.upper())
    set_text_xml(os.path.join(c, 'TEXT', 'TEXT.XML'), 'CAR_' + up, a.display.upper(), 'CAR_' + a.template.upper())
    set_text_xml(os.path.join(c, 'TEXT', 'TEXT.XML'), up + '_DRIVER', a.driver.upper(), a.template.upper() + '_DRIVER')
    print('text: set')
    print('placeholder UI files copied:', copy_placeholders(c, a.name, a.template))
    cars = len([l for l in read_lines(os.path.join(c, 'QUICKRACECARS.TXT')) if l.strip()])
    if cars > MAX_CARS:
        print('WARNING: %d cars; the game only has room for %d' % (cars, MAX_CARS))
    opps = len([l for l in read_lines(os.path.join(c, 'OPPONENTLIST.TXT')) if l.split('//')[0].strip()]) // 2
    if opps > MAX_CARS:
        print('WARNING: %d opponents; the game only has room for %d' % (opps, MAX_CARS))



if __name__ == '__main__':
    main()
