# Display power: dim, off, wake

One manager decides when the screens dim, go dark and wake: `pm_hook()` in
`firmware/components/nucleo_app/nucleo_app.cpp`. It follows the phone/watch model (Android's
PowerManager, Wear OS): user activity resets an inactivity timer, the backlight dims a few seconds
before the timeout as a warning, and at the timeout the screen goes off.

## Where it runs

The keyboard driver calls it on every `nucleo_kbd_read()` (`nucleo_kbd_set_hook`, see
`nucleo_kbd.h`). Every input path reads the keyboard, so every path drives it: the run loop, a
game's own frame loop, a blocking player, a modal. No app has to do anything for it to work.

- **Activity**: any key event, and any key or modifier held (a game played on a held direction
  never dims). Touch on the external panel arrives as injected keys, so it counts too. The side
  button (G0) and a reminder banner wake it from the run loop (`pm_poke()`).
- **Timeout**: Settings > Display > Screensaver (`nucleo_screensaver_timeout_s()`, 0 = never).
  Games get three times the timeout (`PM_GAME_X`).
- **Dim**: in the last fifth of the timeout (2-10 s), a quarter of the backlight. A key restores it
  and is delivered.
- **Off**: backlight 0. The first key only wakes the screen and never reaches the app, so a wake
  cannot press anything. The side button press that wakes does nothing else (no torch).
  On wake the frame is repainted (`nucleo_app_force_repaint()`).
- **Control Center "screen off"** uses the same off state.

## Wake locks

The manager never dims or turns off while one is held:

| Lock | Who |
|---|---|
| `nucleo_app_set_keep_awake(true)` | an app, for its whole foreground life (presence, torch) |
| `nucleo_app_display_hold(true/false)` | code outside the app lifecycle, counted (video while a clip plays) |
| system states | torch, push-to-talk, voice listening, a web client (remote), `/api/display` off, reminder banner, Control Center |

The launcher home and GameFront are not governed here: after the same timeout the launcher opens
the screensaver app. The remote/web idle sleep (`display_sleep()`, frees the 32 KB canvas for the
web OS) is separate and takes precedence.

## External panel

The internal backlight drives the external one (`bl_follow`). When the external backlight is on
the 5 V rail and cannot be switched, the panel is painted black and the mirror is fenced
(`Panel_Mirror::set_dark`): nothing reaches it until the screen wakes, so an app that keeps
animating (a caret, a clock) cannot light it back up. It also saves the bus and the battery.
