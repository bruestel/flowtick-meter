# Adding a meter

flowtick-meter works with any water meter whose register has an **optical
scanning disc** (a disc that turns with the flow and has a bright and a dark
part) and a place where a sensor can look at it, usually a bay for a
plug-in communication module. Supporting a new meter means two things: a case
that holds the sensor in the right place, and the sensor values that make the
signal clean. This page walks through both.

## 0. Check that the meter qualifies

- Is there a **disc you can see** under a window, next to the number rollers,
  with clearly bright and dark sectors? Magnetic or inductive interfaces do not
  work with this sensor.
- Is there a **module bay** or other mount where something can sit over that
  window without opening the sealed register? Never touch the seals.
- Find out the **volume per revolution** of the disc: often printed next to it
  or on the blank module (e.g. `x0,0001` m³ = 1 L per revolution). With one
  bright and one dark half there are two edges per revolution.

## 1. Measure the module bay

- Take photos straight from above and from each end, with a ruler or calliper
  in the picture. A flatbed scan of the removed blank module at 300 dpi gives
  the outline to a tenth of a millimetre.
- Note: outline, wall heights, where the module snaps or clips in, the position
  of the window over the disc **relative to the bay**, and the distance from
  the bay's front to the disc.
- The TCRT5000L reads best at about **2.5 mm** from the disc (still ~75 % at
  5 mm). Plan for the sensor to end close to the window, not at the front of the
  module.

## 2. Design the case

Create `meters/<vendor>-<model>/` (see [meters/README.md](../meters/README.md))
and start from an existing model, e.g.
[meters/allmess-evk-3-110-m/cad/housing.scad](../meters/allmess-evk-3-110-m/cad/housing.scad).

What has worked well:

- **Two parts**: an adapter that replaces the blank module and holds the
  sensor, and a cap that holds the ESP32-C6 with the USB socket reachable.
- **Black filament** (PETG). Light-coloured plastic lets IR through and lowers
  the contrast.
- A sensor sleeve with stops, so the sensor sits at the right depth by itself;
  a slight tilt (about 10–15°) reduces direct reflection from the window.
- Print small **test pieces** first (sensor slot, snap fit). They take
  minutes, the full case takes hours.
- Keep the board beside the sensor rather than above it, and leave room for the
  two resistors.

## 3. Find the resistors

The LED series resistor **R1** and the phototransistor's emitter resistor
**R2** set the operating point. Every meter differs: the distance to the disc,
the window plastic and the disc finish all change how much light comes back.
Start with the values of a supported meter and follow
[docs/hardware.md → Choosing the resistors](hardware.md#choosing-the-resistors).
The goal is a dark level well above 0 V and a bright level clearly below the
3.3 V rail, with as much swing between them as possible.

## 4. Tune the detector

With the sensor in the case on the meter, open the **Tuning** tab and let some
water run. Set the thresholds (the *Derive thresholds* button puts them at 30 %
and 70 % of the swing), check that the *Rejected* counter stays at or near
zero, and adjust dwell time and minimum gap if needed:
[docs/tuning.md](tuning.md).

## 5. Check the volume per edge

Set *Volume per edge* to the value from step 0 and compare the device with the
meter's number rollers after a few hundred litres. If they drift apart, the
volume per edge is wrong or edges are being missed or doubled. Look at the
scope during a draw.

## 6. Document and open a pull request

Your meter folder:

```
meters/<vendor>-<model>/
├── README.md
├── cad/          OpenSCAD (or other parametric) source
├── stl/          exported parts
├── images/       renders
└── photos/       reference photos, metadata stripped: exiftool -all= *.jpg
```

The folder's `README.md` should contain:

- **Meter data**: vendor, model, register type, nominal flow (Q3), which
  module bay or interface, which original modules exist for it.
- **Disc**: layout (e.g. half chrome / half black), revolutions per m³,
  litres per edge.
- **Module bay**: what was measured and how, outline, sensor position,
  snap/clip details, anything that broke or surprised you.
- **Printing and assembly**: parts, orientation, filament, screws, steps.
- **Tested values**:

| | Value |
|---|---|
| R1 (LED series) | e.g. 560 Ω |
| R2 (emitter) | e.g. 1.56 kΩ |
| Dark level | e.g. ~500 mV |
| Bright level | e.g. ~2900 mV |
| Upper / lower threshold | e.g. 2200 / 1200 mV |
| Dwell | e.g. 80 ms |
| Minimum gap | e.g. 150 ms |
| Volume per edge | e.g. 500 ml |

Then add a row to the tables in [meters/README.md](../meters/README.md) and
the main [README.md](../README.md), and open a pull request. See also
[CONTRIBUTING.md](../CONTRIBUTING.md).
