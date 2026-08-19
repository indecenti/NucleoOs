NucleoOS - ROM library (Arcade app)
===================================

Drop game files into the folder for their system. The Arcade app lists whatever
is here; empty folders show as 'empty'. Zip archives work too - they are opened
in the browser, so you can keep the card small.

  nes/        .nes .unf .zip          snes/       .sfc .smc .fig .swc .zip
  gb/         .gb .zip                gbc/        .gbc .gb .zip
  gba/        .gba .zip               gg/         .gg .zip
  sms/        .sms .zip               megadrive/  .md .gen .smd .bin .zip
  pcengine/   .pce .zip               ngpc/       .ngc .ngp .zip
  linx/       .lnx .zip               neogeo/     .zip  (needs neogeo.zip BIOS)
  cps1/       .zip                    cps2/       .zip

Emulation runs in the BROWSER, not on the Cardputer: the device only serves the
files, so a big game costs the device nothing but card space.

Neo Geo and CPS need arcade romsets that match the core (FB Alpha 0.2.97.x).
Put the neogeo.zip BIOS in neogeo/ alongside the games.

Ship only games you own. No ROMs are distributed with NucleoOS.
