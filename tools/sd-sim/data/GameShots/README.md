# Default covers of the native games

Shipped with the SD pack so the Games gallery shows every native game from the first boot. Each is the game's own
compact screen (240x135) rendered on the PC from its real source by the app gallery (`node tools/ui-host/apps.mjs`,
scene noted below), not a photo or a painting. A cover captured on the device (Fn+P / in-game, saved as
`<id>.bmp`) takes precedence. Cardler and Orde draw from sprite atlases the PC gallery does not load: they keep their
poster until a cover is captured.

stelle: constellations.combat, pong: pong.play, pinball: pinball.play, tanks: tanks.play, poker: poker.hold,
brawler: brawler.play, reactor: reactor.play, giardino: sandgarden.play, slots: slots.spin, dice: dice.rolled,
snake: snake.play (the HUD cut: its player name is the device's), tankd: tankduel.play, yahtzee: yahtzee.rolled.
