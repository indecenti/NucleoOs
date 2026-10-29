/* SPDX-License-Identifier: AGPL-3.0-or-later */
// First script of the desktop.
// Never run inside another site's frame: a hidden frame of the desktop under a hostile page would turn the user's clicks
// into commands to the device (clickjacking). A frame of the same origin (an edition embedding it) can read its top
// window; a foreign one cannot, and then the page stops before the shell is even fetched.
(function () {
  if (window.top === window.self) return;
  try { void window.top.location.href; return; } catch (e) {}
  try { window.stop(); } catch (e) {}
  document.documentElement.style.display = 'none';
})();
// Keep the browser's install prompt for the Start menu's "Install" (registered before the event can fire).
window.addEventListener('beforeinstallprompt',function(e){e.preventDefault();window.__nucleoInstallPrompt=e;});
