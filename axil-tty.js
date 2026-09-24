const term = document.getElementById("term");

const axilMaker = window.ttyAxil;
const proto = location.protocol === "https:" ? "wss" : "ws";
const axil = axilMaker.create(term, { url: proto + "://" + location.hostname + ":" + location.port + "/tty" });

axil.onMessage = function onMessage(ev) {
  return true;
};
