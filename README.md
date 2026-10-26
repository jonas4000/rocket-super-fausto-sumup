# Pay-per-dose coffee grinder

ESP32 firmware that turns a coffee grinder into a self-service one: pick a single or double shot on the touchscreen, pay by card on a SumUp Solo reader, and the grinder runs for exactly the paid dose.

It was built for a Rocket Espresso Super Fausto in a shared office kitchen: the grinder pays for its own upkeep, and nobody has to collect cash. The firmware only switches the motor on and off for a set time, so it should work with any grinder that grinds by time and can be switched by a relay. The 3D-printed parts are made for the Super Fausto.

- Touch UI (LVGL, 320×240 landscape): dose choice, payment and grinding progress, info and stats pages, PIN-protected settings for prices and grind times.
- Card payments through the SumUp Cloud API, straight from the device (no backend). Abandoned or late payments are cancelled and refunded automatically.
- Every 25th, 50th, 100th … shot is free (paid, then refunded right away).
- Optional sales log in Supabase.
- **Flappy Beans**: an optional screensaver game. A top 5 score with 10+ points earns a free shot. It can be switched off in Settings, and the dose screen then becomes the start screen.

## Hardware

| Part | Notes |
| --- | --- |
| [Waveshare ESP32-S3-Touch-LCD-2.8](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8) | ESP32-S3, 16 MB flash, 8 MB PSRAM, ST7789 display, capacitive touch (board V1 and V2 both work) |
| SumUp Solo | Paired to your SumUp merchant account for the Cloud API |
| 3D-printed parts | Display mount, cover and cross struts in [`hardware/stl/`](hardware/stl) |
| 4× M2.5 threaded inserts | Heat-set into the printed parts |
| 8× M2.5 screws, 10 mm | |
| 10× M2.5 washers | |

Pins are in [`include/pins.h`](include/pins.h).

> **Safety:** this switches a mains-powered motor. Have the mains side wired by someone qualified. The firmware turns the relay off first thing at boot, caps every run at a hard maximum and uses a watchdog. The grinder only starts after a confirmed payment.

## Getting started

1. Install [PlatformIO](https://platformio.org/) (e.g. `uv tool install platformio`).
2. Copy `include/secrets.example.h` to `include/secrets.h` and fill in Wi-Fi, the SumUp API key, merchant code, reader ID, affiliate key, Supabase URL and key, and your settings PIN. `secrets.h` is gitignored. Without it, the firmware still builds with Wi-Fi off and a fake payment over serial (`a` = approve, `d` = decline).
3. Pair the Solo: on the reader, *Connections > API > Connect*, then register the pairing code with your merchant account (SumUp dashboard or `POST /v0.1/merchants/{merchant_code}/readers`).
4. Optional: run [`supabase/schema.sql`](supabase/schema.sql) in your Supabase project for the sales log.
5. Build and flash:

   ```sh
   pio run -e esp32s3 -t upload -t monitor
   ```

> **Security:** the SumUp secret API key is stored unencrypted in the ESP32's flash. Anyone with physical access to the board's USB port can read out the flash and take the key, which gives full access to your SumUp merchant account (transactions, checkouts, refunds). To reduce the risk:
>
> - make sure the USB port can't be reached once the board is mounted;
> - optionally turn on [ESP32 flash encryption](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/security/flash-encryption.html);
> - revoke the key in SumUp immediately if the device is ever lost or opened.

## Development without the device

```sh
pio test -e native                               # unit tests (state machine, game, SumUp client)
pio run -e ui-sim && .pio/build/ui-sim/program   # desktop simulator, needs `brew install sdl2`
pio run -e sim && .pio/build/sim/program         # terminal simulator (type `help`)
pio run -e sumup-cli && .pio/build/sumup-cli/program readers  # talk to the real SumUp API
```

The desktop simulator shows the screen next to a fake card reader (Approve/Decline), a motor LED and a Wi-Fi switch. The settings PIN in the simulators is `1234`. `ui-sim --sumup` and `sumup-cli pay` take **real card payments**.

[`CLAUDE.md`](CLAUDE.md) documents the architecture, the payment flow and the UI in detail.

## Disclaimer

This is a hobby project, shared as is and without any warranty (see [LICENSE](LICENSE)). Building or using it is entirely at your own risk.

- **Mains electricity and motors are dangerous.** Wiring a relay into a grinder means working on mains voltage. Only do it if you are qualified, or have it done by a qualified electrician. Unplug the grinder before opening it.
- **Modifying your grinder will probably void its warranty** and can damage it. The authors are not responsible for damage to grinders, other property or people, or for lost money from failed, duplicate or unrefunded payments.
- **If you operate it, you are the operator:** electrical safety, hygiene, taxes, your payment provider's terms and any other rules that apply where you run it are your responsibility.
- This project is not affiliated with or endorsed by Rocket Espresso, SumUp, Supabase or Waveshare. Product names are trademarks of their owners.

## License

MIT, see [LICENSE](LICENSE), except for these third-party assets, which keep their own licences:

- Game art: [Flappy Beans](https://opengameart.org/content/flappy-beans) by [1UP.capsule](https://opengameart.org/users/1upcapsule), [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). Cropped, scaled and converted for this screen, see [`assets/flappy/LICENSE.txt`](assets/flappy/LICENSE.txt).
- Fonts in `assets/fonts/`: Montserrat and Press Start 2P (SIL OFL 1.1), Font Awesome Free (see the licence files there).
