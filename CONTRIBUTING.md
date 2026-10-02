# Contributing

Thanks for wanting to help!

## Support for another meter

The most useful contribution is a **new meter**: a case that fits its module
bay and the values that make the sensor read it reliably. Follow
[docs/adding-a-meter.md](docs/adding-a-meter.md) and open a pull request with a
new folder under [`meters/`](meters/).

Also welcome: measurements from meters you tried and could *not* get working:
an open issue with photos and the dark/bright levels saves the next person time.

## Firmware

- C++20 on ESP-IDF, built with PlatformIO; see [firmware/README.md](firmware/README.md).
- Format with the [`.clang-format`](firmware/.clang-format) in `firmware/`.
- Comments explain *why*, not *what*. Keep the existing tone.
- Test on hardware before opening a PR and say what you tested.

## Documentation

- Everything in English.
- No personal data in examples: use `192.168.1.x`, `flowtick-garden`,
  `mqtt.example.com` and the like.
- Strip metadata from photos (`exiftool -all= photo.jpg`).

## Licence

By contributing you agree that your contribution is licensed under the
[Apache License, Version 2.0](LICENSE), like the rest of the project.
