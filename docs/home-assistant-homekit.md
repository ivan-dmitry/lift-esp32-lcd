# Home Assistant and Apple HomeKit

The display talks to Home Assistant over **MQTT** with discovery: once it is
connected to the broker, a device **LIFT A** appears in HA by itself. Apple Home
gets the lift through HA's **HomeKit Bridge** integration — nothing HomeKit runs
on the ESP32 (the firmware fills 83 % of an OTA slot; a native HomeKit stack
would not fit next to OTA, and HomeKit has no "floor number" type anyway).

Home Assistant here: `http://192.168.31.8:8123`. MQTT topics of this display:
base `elevator/f79344` (the last 3 bytes of the board's MAC).

## 1. MQTT broker (once, in Home Assistant)

1. **Settings › Add-ons › Add-on store › Mosquitto broker** › Install › Start,
   and turn on *Start on boot*.
2. **Settings › Devices & services**: HA offers **MQTT** (discovered) ›
   Configure › Submit.
3. A login for the display: **Settings › People › Users › Add user** (the Users
   tab needs *Advanced mode* in your profile), e.g. user `liftdisplay` with a
   password. Mosquitto accepts HA users.

## 2. The display

MENU › **HA · MQTT** on the panel (or `/cfg` › MQTT on the web page):
host `192.168.31.8`, port `1883`, user `liftdisplay`, the password, **ON**.
The status line turns to *connected*; `/api/state` shows `"mqtt":true`.

HA then shows, under the device **LIFT A**:

| Entity | Kind |
|---|---|
| Floor, Direction, Mode, Fault code | sensors |
| Door (`door`), Moving (`moving`), Fault (`problem`), Fire service (`safety`), CAN signal | binary sensors |
| Car call, Hall call up, Hall call down | selects (pick a floor = send the call) — only while **remote commands** are on |
| Door open / close | buttons — not mapped on the Fuji bus yet, they answer "no command frame defined" |

## 3. Scripts "call the lift to floor N"

For Siri and HomeKit a switch per floor works best. Add to `scripts.yaml`
(or Settings › Automations › Scripts › ⋮ › Edit in YAML, one script each).
A hall call is what a person at the landing presses: **up** from floors 1–5,
**down** from the top floor 6. Change `cmd/up` to `cmd/dn` or `cmd/car` as you like.

```yaml
lift_call_floor_1:
  alias: "Lift to floor 1"
  icon: mdi:elevator-passenger
  sequence:
    - action: mqtt.publish
      data: { topic: "elevator/f79344/cmd/up", payload: "1" }
lift_call_floor_2:
  alias: "Lift to floor 2"
  icon: mdi:elevator-passenger
  sequence:
    - action: mqtt.publish
      data: { topic: "elevator/f79344/cmd/up", payload: "2" }
lift_call_floor_3:
  alias: "Lift to floor 3"
  icon: mdi:elevator-passenger
  sequence:
    - action: mqtt.publish
      data: { topic: "elevator/f79344/cmd/up", payload: "3" }
lift_call_floor_4:
  alias: "Lift to floor 4"
  icon: mdi:elevator-passenger
  sequence:
    - action: mqtt.publish
      data: { topic: "elevator/f79344/cmd/up", payload: "4" }
lift_call_floor_5:
  alias: "Lift to floor 5"
  icon: mdi:elevator-passenger
  sequence:
    - action: mqtt.publish
      data: { topic: "elevator/f79344/cmd/up", payload: "5" }
lift_call_floor_6:
  alias: "Lift to floor 6"
  icon: mdi:elevator-passenger
  sequence:
    - action: mqtt.publish
      data: { topic: "elevator/f79344/cmd/dn", payload: "6" }
```

The payload is the floor **label** as shown on the display; `"#2"` sends a floor
index instead (0 = lowest). The display answers on `elevator/f79344/cmd_result`
(`{"ok":true,"msg":"sent"}`, or why it refused: remote commands off, fire
service, inspection…). On HA older than 2024.8 write `service:` instead of `action:`.

## 4. HomeKit Bridge

1. **Settings › Devices & services › Add integration › HomeKit Bridge**; pick the
   domains `script` and `binary_sensor` (add `sensor` if you want the fault code).
2. Open the bridge's **Configure** › mode *include* › choose the six
   `script.lift_call_floor_*`, `binary_sensor.lift_a_door`,
   `binary_sensor.lift_a_moving`, `binary_sensor.lift_a_fault`,
   `binary_sensor.lift_a_fire_service`.
3. Scan the QR code from the HA notification with the iPhone **Home** app.

In Apple Home: *Lift to floor N* switches (Siri: "Hey Siri, lift to floor 3"),
door as a contact sensor, moving as a motion sensor, fault and fire service as
alerts. The current floor number has no HomeKit type — use an HA notification
automation for "the lift arrived at floor N" if you need it on the phone.
