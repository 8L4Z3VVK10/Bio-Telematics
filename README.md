# Bio-Telematics: An IoT-Based Plant Electrophysiology and Environmental Stress Translator

Plants generate tiny electrical signals in response to things like touch, light, water stress, and mechanical disturbance. This project is an attempt to actually pick up on that — measuring a plant's bio-potential alongside its surrounding environment (light, temperature, humidity, soil moisture), and pushing all of it to a live dashboard you can check from your phone.

The core idea: plants "talk" electrically all the time, we just don't usually have the equipment to listen. This system is that equipment, built on an ESP32.

## What it actually does

- Picks up the plant's own bio-electrical signal using two electrodes and an AD620 instrumentation amplifier (these signals are microvolts to millivolts — way too small to read directly, so they need serious amplification first)
- Digitizes everything at 16-bit resolution using an ADS1115 ADC, since the ESP32's built-in ADC isn't precise enough for signals this small
- Tracks light, temperature, humidity, and soil moisture at the same time, so you can see how environmental conditions line up with the plant's electrical activity
- Scores overall plant health (0–100) based on how far temperature, humidity, soil moisture, and light are from healthy ranges, and turns that into a plain-language status and recommendation
- Syncs real time over WiFi (NTP) so the health score knows the difference between "dark because it's night" and "dark because the plant is poorly placed" — low light after sunset doesn't get wrongly flagged as a problem
- Streams live readings, health status, and recommendations to a Blynk dashboard every couple seconds
- Flags when the bio-potential shifts noticeably from its resting baseline — a rough but real way of catching "something just happened to this plant," with a separate, higher threshold for flagging severe stress

## Hardware

| Component | What it's for |
|---|---|
| ESP32 Dev Board (38-pin) | Runs everything, handles WiFi |
| AD620 module (with ICL7660A charge pump) | Amplifies the plant's raw signal |
| ADS1115 | 16-bit ADC — reads the amplified bio-potential and the soil sensor |
| BH1750 | Light sensor |
| DHT22 | Temperature + humidity |
| Capacitive soil moisture sensor | Soil moisture |
| Ag/AgCl electrodes / ECG pads | Actually touch the plant to pick up the signal |

## Wiring

| Component | Connects to | Notes |
|---|---|---|
| ADS1115 SDA/SCL | GPIO21 / GPIO22 | Shares the I2C bus with BH1750 |
| BH1750 SDA/SCL | GPIO21 / GPIO22 | Same bus |
| DHT22 DATA | GPIO4 | |
| Soil sensor output | ADS1115 A1 | |
| AD620 output | ADS1115 A0 | |
| AD620 S+ / S− | Electrode 1 / Electrode 2 | |
| AD620 VIN | 3.3V | |

Everything runs off a shared 3.3V rail with common ground — kept it all on one voltage domain to avoid I2C headaches (learned that one the hard way).

## How it's put together

Two electrodes go on the plant a few centimeters apart. The AD620 picks up the (tiny) voltage difference between them and amplifies it somewhere in the 100–1000x range, adjustable via an onboard gain trimmer. There's also a separate offset trimmer to zero the output before you start reading anything — otherwise you're stuck reading whatever arbitrary baseline the amplifier happens to sit at.

Once amplified, the signal goes into the ADS1115 alongside the soil sensor, gets converted to a digital value, and handed to the ESP32 over I2C. The ESP32 also polls the light and temperature/humidity sensors on its own schedule, bundles everything together, and pushes it up to Blynk so it's viewable remotely.

For the "stress" side of things — the system watches the bio-potential reading against a settled baseline (captured a few seconds after startup) and flags when it deviates past a threshold. In testing, this reliably picks up on physical stimulus like pressing or bending a leaf near the electrode site. There are two tiers: a smaller deviation gets flagged as "mild stimulus" (the plant noticed something), and a larger deviation gets flagged as "severe stress" — at which point the system's recommendation shifts specifically to telling you to go check the plant for physical damage or acute stress, overriding whatever the general environmental recommendation was saying.

Separately, each environmental reading (temperature, humidity, soil moisture, light) gets scored against a healthy range and averaged into an overall 0–100 health score, which maps to a status label (Excellent, Healthy, Needs Attention, Stressed, Critical) and a plain-language recommendation pointing at whichever factor is furthest out of range. Right now this is a simple instant read of current conditions, not a time-averaged trend — a good next step would be tracking this over time instead of just reacting to the current moment.

One thing that tripped me up early on: light naturally drops to zero every night, and my first version of the health score treated that exactly like a plant sitting in a dark closet — which meant the status would flip to "Stressed" every evening for no real reason. Fixed it by having the ESP32 sync real time over WiFi (NTP) once it connects, then checking the current hour before scoring light: between 6 AM and 7 PM, low light is scored normally and can genuinely pull the health score down; outside that window, it's treated as expected and doesn't penalize anything. Also means a plant kept somewhere too dark *during the day* still gets flagged properly — the fix only ignores light at night, it doesn't ignore bad daytime placement.

## Getting it running

1. Install the ESP32 board package in Arduino IDE
2. Install these libraries via Library Manager: `Blynk`, `Adafruit ADS1X15`, `BH1750`, `DHT sensor library`
3. Drop in your own credentials near the top of the sketch:
   ```cpp
   #define BLYNK_TEMPLATE_ID "your_template_id"
   #define BLYNK_TEMPLATE_NAME "your_template_name"
   #define BLYNK_AUTH_TOKEN "your_auth_token"
   char ssid[] = "YOUR_WIFI_NAME";
   char pass[] = "YOUR_WIFI_PASSWORD";
   ```
4. Recalibrate `DRY_VALUE` / `WET_VALUE` for your own soil sensor and soil — these numbers are specific to my exact sensor and setup, yours will read differently
5. Upload, open Serial Monitor at 115200 baud, check everything's initializing properly
6. Check your Blynk dashboard for live data

## Things I ran into (and what I learned)

- Bio-potential readings are strongest right near the electrode site and fade out fast with distance — makes sense once you think about how the signal actually has to travel through the plant's tissue to reach the electrodes
- The baseline shifts a bit every time you reconnect the electrodes — this is apparently a known thing with electrode-tissue contact (it needs a moment to "settle" after being disturbed), not a fault in the circuit
- My LCD wouldn't display anything readable at 3.3V no matter what I did to the contrast — turned out that specific panel just needs 5V logic to work properly
- I tried building a resistor-divider circuit to safely bring the LCD's 5V I2C lines down for the ESP32, since I didn't have a proper level-shifter module on hand. It didn't work — the LCD showed solid blocks instead of text, and an I2C scan couldn't even detect it. Turns out a plain resistor divider isn't a good fit for I2C specifically: I2C relies on fast, clean rising edges, and the extra resistance in a divider chain slows those edges down enough that the bus stops working entirely, even though the same trick works fine for simple analog signals. Ended up leaving the LCD out of this version rather than force a fix that wasn't holding up — a proper BSS138-based logic level shifter is the correct fix, just didn't have one in time.
- Electrical noise is a real problem at this signal level — twisting the electrode leads together helped noticeably
- Insertion depth matters a lot for the capacitive soil sensor — just the tip in soil gave a wildly dry-looking reading; it needed to go in close to its marked line before the numbers started making sense
- Clipping electrodes onto the snap stud of a gel ECG pad (instead of clamping the alligator clip straight onto the leaf) gives much more stable contact — direct clamping on leaf tissue adds its own pressure-related noise that's easy to mistake for a real signal
- Averaging all 4 environmental scores together can hide a real problem — if soil, temp, and humidity are all fine but light is at 0%, the average still comes out looking "Healthy." Had to add a separate rule that lets one genuinely bad factor override the average instead of getting diluted by the good ones.
- Related to the above: the moment I added that override rule, it broke every night, since light legitimately goes to 0 after sunset and isn't a real problem then. That's what led to adding the NTP time sync — the fix needed actual context (is it day or night right now), not just a smarter formula
- Also had to rethink light scoring itself, twice. First version scored light against a range meant for outdoor sun, so any normal indoor room looked "critically dark" even in good daytime light — the scale was just wrong for the actual use case. Fixed the range, but that created the opposite problem: taking the plant outside into real sunlight would now score as "too bright" and get penalized the same way as too dim, since the scoring was symmetric. Ended up making light scoring asymmetric on purpose — strict about darkness (a real, common problem), lenient about brightness (rarely a real problem until it's extreme, direct sun). Temperature/humidity/soil still use a simple symmetric range since "too much" and "too little" are both genuinely bad for those

## What's next

- Try stem-based electrode placement instead of leaf-based — probably picks up more of the plant's overall signal instead of just what's happening locally
- Add proper shielding (foil-wrapped, single-point grounded) on the electrode leads
- Get the LCD working with a proper logic level shifter for local, no-internet-needed display
- Log data over time instead of just showing live values, so trends are actually visible
- Eventually: some kind of real stress classification instead of a simple threshold check
- Add a soil pH sensor and NPK (nitrogen-phosphorus-potassium) sensor for a fuller picture of soil health, not just moisture
- Add an automated water pump, so the system can act on what it senses (e.g. trigger irrigation when soil moisture drops below a threshold) instead of just reporting data
- Possibly extend to multiple plants at once, comparing bio-potential and environmental data across them
