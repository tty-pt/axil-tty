const term = document.getElementById("term");

// ttyAxil is published by axil-tty.js, loaded just before this file.
// No options: the default endpoint is this module's /tty route, and the default
// sub carries the full hook set (onOpen/onClose must not be left undefined).
const axil = window.ttyAxil.create(term);

axil.onMessage = function onMessage(ev) {
  return true;
};
