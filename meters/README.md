# Meters

One folder per meter model. Each folder holds what is specific to that meter:
the case that fits its module bay and the sensor values that make it read
reliably. Everything else (circuit, firmware, tuning) is shared and lives in
[`docs/`](../docs/) and [`firmware/`](../firmware/).

## Supported

| Folder | Meter | Volume per edge | R1 / R2 | Status |
|---|---|---|---|---|
| [allmess-evk-3-110-m](allmess-evk-3-110-m/) | Allmess EVK 3/110 +m (cold water) | 0.5 L | 560 Ω / 1.56 kΩ | working |

## Folder structure

```
meters/<vendor>-<model>/
├── README.md     meter data, module bay, measurements, tested values
├── cad/          parametric source of the case (OpenSCAD preferred)
├── stl/          exported parts, ready to print
├── images/       renders of the case
└── photos/       reference photos (metadata stripped)
```

Folder names are lower case, words separated by hyphens:
`<vendor>-<model>`, e.g. `allmess-evk-3-110-m`.

How to add one: [docs/adding-a-meter.md](../docs/adding-a-meter.md).
