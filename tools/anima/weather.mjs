// Re-export: the weather NLU + live forecast now live with the ANIMA web app, which runs them in the
// browser (apps/anima/www/local/weather.js). This path stays for the simulator, the tests and the
// firmware mirror comment (nucleo_anima_online.c), so there is still ONE implementation.
export * from '../../apps/anima/www/local/weather.js';
