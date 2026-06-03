# Hardware

The bridge needs only an **ESP32**. The one wired peripheral is a **magnetic reed
switch** exposed as a HomeKit Contact Sensor. This doc covers that wiring and how
to add more GPIO/I²C sensors.

## Magnetic reed switch (Contact Sensor)

```
   Reed switch
   ┌────────┐
   │        ├──────── GPIO 23      (REED_PIN)
   │        ├──────── GND
   └────────┘
```

- One leg to **GPIO 23**, the other to **GND**. No external resistor — the sketch
  enables the ESP32's **internal pull-up**.
- **Magnet present** (door/window closed) → reed closes → pin LOW → Home shows
  **Closed** (contact detected).
- **Magnet away** (opened) → pin floats HIGH via pull-up → Home shows **Open**.
- Debounced ~30 ms in firmware; changes appear in Home within a moment. Serial
  logs each change: `[reed] pin 23 -> OPEN`.

Config (top of the sketch):
```c
#define REED_PIN          23
#define REED_NAME         "Door Sensor"
#define REED_ACTIVE_LOW   true   // false if your reed is normally-closed / inverted
```
If Home shows Open when the door is closed, flip `REED_ACTIVE_LOW`.

## Choosing a GPIO (important)

| Pins | Use for input? |
|------|----------------|
| 13, 14, 16, 17, 18, 19, 21, 22, **23**, 25, 26, 27, 32, 33 | ✅ safe, have internal pull-ups |
| 34, 35, 36(VP), 39(VN) | ⚠️ input-only, **no internal pull-up** — needs an external 10 kΩ pull-up |
| 0, 2, 12, 15 | ⚠️ strapping pins — can block/alter boot if pulled wrong |
| 6–11 | ❌ wired to the SPI flash — never use |
| 1, 3 | ⚠️ UART0 TX/RX — used by the serial monitor |

GPIO **23** is the safe default used here.

## Adding more sensors

Every HomeKit sensor type is a `Service::` in HomeSpan and follows the same
pattern as the reed switch: read the pin/bus in the service's `loop()`, push
changes with `setVal()`. Common additions:

| Hardware | HomeKit service | Notes |
|----------|-----------------|-------|
| PIR motion module | `MotionSensor` | digital GPIO, like the reed |
| DHT22 / DS18B20 | `TemperatureSensor` + `HumiditySensor` | needs the sensor's library |
| LDR / BH1750 | `LightSensor` | analog/I²C |
| Water probe | `LeakSensor` | digital GPIO |
| Reed/button | `StatelessProgrammableSwitch` | single/double/long press → automations |

See [CUSTOMIZATION.md](CUSTOMIZATION.md) for how a service is declared and
[LIMITATIONS.md](LIMITATIONS.md) for the full list of HomeKit service types.

## Power

- Use a quality USB cable/supply. Weak USB power can brown-out the ESP32 during
  WiFi transmit, causing random reboots.
- The reed switch draws negligible current; no separate supply needed.
