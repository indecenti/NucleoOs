/* SPDX-License-Identifier: AGPL-3.0-or-later */
// The small-screen notice: "continue anyway" is remembered in this browser.
(function () {
  var ph = document.getElementById('mobile-placeholder');
  if (!ph) return;
  // The hide rules are gated on body.mobile-ok; setting only the placeholder's
  // inline display cannot beat the `!important` desktop-hide rules, so toggle
  // the body class instead (that reveals BOTH the desktop and hides the notice).
  var go = function () { document.body.classList.add('mobile-ok'); };
  // localStorage can throw (private mode, blocked storage): the button must still work.
  try { if (localStorage.getItem('nucleo.mobileOk') === '1') { go(); return; } } catch (e) {}
  var b = document.getElementById('mobile-continue');
  if (b) b.onclick = function () { try { localStorage.setItem('nucleo.mobileOk', '1'); } catch (e) {} go(); };
})();
