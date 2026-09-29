"""Convert every Splat Pack car the Android game lacks and add it to the roster (and the opponent list).

usage: splatpack.py <Carmageddon1 install dir> <unpacked game dir> [--convert-only] [--only NAME,...]
"""
import argparse, os, re, subprocess, sys, traceback
sys.path.insert(0, os.path.dirname(__file__))
import c1text, carconv, addcar

# Splat Pack cars that are not in the Android game, with display names
CARS = {
    '333': '333', 'BUGGIT': 'BUGGIT', 'DOOZER': 'DOOZER', 'JAQUES': 'JAQUES', 'JEEPY': 'JEEPY',
    'MONSTER': 'MONSTER', 'MUSCLE': 'MUSCLE', 'NEWANNIE': 'NEW ANNIE', 'NEWEAGLE': 'NEW EAGLE',
    'PARAMED': 'PARAMED', 'PORK': 'PORK', 'ROADHOG': 'ROADHOG', 'SEMI': 'SEMI', 'SLED': 'SLED',
    'SPAGHETI': 'SPAGHETTI', 'SUBFRAME': 'SUBFRAME', 'TOOHORSE': 'TWO HORSE', 'V6SHAME': 'V6 SHAME',
    'VLAD2': 'VLAD 2',
}


def opponents(splat_data):
    """car file -> dict(driver, strength, mph, tons, sixty) from OPPONENT.TXT."""
    L = [l.split('//')[0].rstrip() for l in c1text.read_text(os.path.join(splat_data, 'OPPONENT.TXT'))]
    res = {}
    for i, l in enumerate(L):
        if not l.strip().upper().endswith('.TXT') or i < 6:
            continue
        car = l.strip().upper()[:-4]
        driver = L[i - 6].strip()
        try:
            strength = int(L[i - 3].strip())
        except ValueError:
            strength = 3
        info = {'driver': driver, 'strength': strength}
        for l2 in L[i:i + 14]:
            m = re.search(r'TOP SPEED:\s*([\d.]+)', l2)
            if m: info['mph'] = float(m.group(1))
            m = re.search(r'WEIGHT:\s*([\d.]+)', l2)
            if m: info['tons'] = float(m.group(1))
            m = re.search(r'0-60MPH:\s*([\d.]+)', l2)
            if m: info['sixty'] = float(m.group(1))
        res[car] = info
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('install')
    ap.add_argument('game_dir')
    ap.add_argument('--convert-only', action='store_true')
    ap.add_argument('--only')
    ap.add_argument('--work', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'splat_out'))
    a = ap.parse_args()
    splat = os.path.join(a.install, 'CARSPLAT', 'DATA')
    base = os.path.join(a.install, 'CARMA', 'DATA')
    content = os.path.join(a.game_dir, 'DATA', 'CONTENT')
    opp = opponents(splat)
    cars = [c for c in CARS if not a.only or c in a.only.upper().split(',')]
    ok = []
    for car in cars:
        out = os.path.join(a.work, car)
        info = opp.get(car, {})
        tons = info.get('tons', 1.5)
        template = 'Dump' if tons >= 2.5 else 'BlkEagle'
        tdir = os.path.join(content, 'VEHICLES', template.upper())
        try:
            if os.path.exists(out):
                import shutil
                shutil.rmtree(out)
            carconv.convert([splat, base], car + '.TXT', out, tdir,
                            os.path.join(content, 'TRACKS', 'LEVELS', 'CITY_A', '1GRILLS.MTL'))
        except Exception as e:
            print('!! %s failed: %s' % (car, e))
            traceback.print_exc(limit=3)
            continue
        ok.append((car, info, template))
    print('converted %d of %d' % (len(ok), len(cars)))
    if a.convert_only:
        return
    for car, info, template in ok:
        name = car.capitalize() if not car[0].isdigit() else 'Car' + car
        mph, tons, sixty = info.get('mph'), info.get('tons'), info.get('sixty')
        strength = info.get('strength', 3)
        is_opponent = strength >= 1  # player cars (New Eagle, New Annie) have strength -1
        if not is_opponent:
            strength = 3
        defence = max(1, min(5, int(round((tons or 1.5) * 1.2))))
        specs = '%d,%d,,,,%s,%s,%s' % (defence, strength, round(mph * 1.609) if mph else '',
                                       int(tons * 1000) if tons else '', sixty if sixty else '')
        args = [sys.executable, os.path.join(os.path.dirname(__file__), 'addcar.py'), a.game_dir,
                os.path.join(a.work, car), name, CARS[car], info.get('driver', name), '--template', template,
                '--specs', specs] + (['--opponent', str(strength)] if is_opponent else [])
        r = subprocess.run(args, capture_output=True, text=True)
        print(car, '->', name, 'OK' if r.returncode == 0 else 'FAILED\n' + r.stderr[-800:])


if __name__ == '__main__':
    main()
