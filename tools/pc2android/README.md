# pc2android

Converts Carmageddon 1 (PC) cars into Android Carmageddon vehicles and adds them to the game's roster.
Works on an unpacked game folder (`carmadroid.exe --extract-data <folder>`), which is then run with
`carmadroid.exe --game-dir <folder>`. Needs Python 3, no other packages.

## Converting a car

```
python carconv.py "<PC DATA dirs, ;-separated>" <CAR.TXT> <output folder> <template vehicle folder> <template .MTL>
```

- PC DATA dirs: the Splat Pack's `DATA` first, then the base game's, so shared files are found
  (e.g. `...\CARSPLAT\DATA;...\CARMA\DATA`).
- Template vehicle folder: an unmodified Android car folder, whose `CAR.TXT` is used as the base.
- Template MTL: any one-texture material, e.g. `DATA\CONTENT\TRACKS\LEVELS\CITY_A\1GRILLS.MTL`.

Writes `CARBODY.CNT`, `CARBODY.MDL`, the four wheel models, a material and texture per PC material, and
`CAR.TXT` with a collision box fitted to the new body.

## Adding it to the roster

```
python addcar.py <game dir> <converted folder> <NAME> "<Car name>" "<Driver name>" --template Dump
    --specs "Defence,Offence,Power,Softness,BHP/ton,TopSpeed,Mass,0-60"
```

Copies the car into `VEHICLES`, adds it to `QUICKRACECARS.TXT` and `CARSPECS.TXT` (starting from the
template car's row), adds the car and driver names to `TEXT.TXT` and `TEXT\TEXT.XML`, and copies the
template car's menu pictures and damage-HUD layout as placeholders. Empty `--specs` fields keep the
template's values.

New cars start locked in career mode. `carmadroid.exe --unlock-all-cars` unlocks every car for testing.

## Limits and findings

- The game itself has room for 40 cars; carmadroid raises that to 64 cars and 64 opponents
  (`src/roster.cpp`). The original roster is 30. The car grid scrolls, so extra cars appear in new
  columns. Cars owned beyond the save file's 40-name list are kept in `userdata\extra_cars.txt`.
- Geometry: PC units x 6.9, Z negated (front is -Z on PC, +Z on Android), triangle winding reversed.
- Textures are written as uncompressed IMG v1.0, one plane, A,R,G,B per pixel. PC palette index 0 becomes
  transparent (alpha 0), but the template material does not enable transparency yet.
- Suspension and other moving parts are merged into the body.

## Files

| File | |
| --- | --- |
| `brender.py` | PC readers: DAT (models), ACT (actor hierarchies), MAT, PIX, palette |
| `c1text.py` | Decodes C1's encrypted text files (`@` lines) |
| `stainless.py` | Android readers/writers: CNT v4.0, MDL v6.2 |
| `lol.py` | Lists constants in compiled Lua 5.1 (`.LOL`) UI files |
| `carconv.py` | PC car to Android vehicle folder |
| `addcar.py` | Adds a vehicle folder to the roster |
| `splatpack.py` | Converts and adds all 19 Splat Pack cars (`splatpack.py <Carmageddon1 install> <game dir>`) |
| `fli.py` | Decodes C1 FLI/FLC animations (driver mugshots, car animations) |
