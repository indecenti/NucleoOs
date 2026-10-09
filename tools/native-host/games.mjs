// The native games the host harness (run.mjs) and the language checker (tools/game-i18n/check.mjs) know.
// id -> the game's source, its registration function, helper sources linked beside it, and the SD folders
// copied from deploy/sd/data into the sandbox (atlases, sound packs) so the game loads its real assets.
export const GAMES = {
  brawler:   { src: 'app_brawler.cpp', reg: 'nucleo_register_brawler', extra: ['brawler_chars.cpp', 'brawler_combat.cpp', 'brawler_enemies.cpp', 'brawler_fx.cpp', 'brawler_levels.cpp', 'brawler_menu.cpp', 'brawler_net.cpp', 'brawler_scene.cpp', 'brawler_sfx.cpp'], data: ['brawler'] },
  cardler:   { src: 'app_cardler.cpp', reg: 'nucleo_register_cardler', data: ['Cardler'] },
  stelle:    { src: 'app_constellations.cpp', reg: 'nucleo_register_constellations', data: ['costellazioni'] },
  dice:      { src: 'app_dice.cpp', reg: 'nucleo_register_dice', ui: true, data: ['dice'] },
  pinball:   { src: 'app_pinball.cpp', reg: 'nucleo_register_pinball', data: ['pinball'] },
  poker:     { src: 'app_poker.cpp', reg: 'nucleo_register_poker', data: ['poker'] },
  pong:      { src: 'app_pong.cpp', reg: 'nucleo_register_pong', data: ['pong'] },
  reactor:   { src: 'app_reactor.cpp', reg: 'nucleo_register_reactor', data: ['reattore'] },
  giardino:  { src: 'app_sandgarden.cpp', reg: 'nucleo_register_sandgarden', data: ['giardino'] },
  slots:     { src: 'app_slots.cpp', reg: 'nucleo_register_slots', data: ['slots'] },
  snake:     { src: 'app_snake.cpp', reg: 'nucleo_register_snake', data: ['snake'] },
  tankd:     { src: 'app_tankduel.cpp', reg: 'nucleo_register_tankduel', data: ['tankduel'] },
  orde:      { src: 'app_vs.cpp', reg: 'nucleo_register_vs', extra: ['vs_sim.c'], data: ['Orde'] },
  yahtzee:   { src: 'app_yahtzee.cpp', reg: 'nucleo_register_yahtzee', ui: true, data: ['yahtzee'] },
  // tanks has its own deeper harness (tools/tanks-host); listed here for the language checker
  tanks:     { src: 'app_tanks.cpp', reg: 'nucleo_register_tanks', data: ['tanks'], ownHarness: true },
  // the console front-end (not a game): listed for the language checker only
  gamefront: { src: 'gamefront.cpp', ownHarness: true },
};
