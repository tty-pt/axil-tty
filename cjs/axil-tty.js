let createAxilTtyBridge;
try {
  ({ createAxilTtyBridge } = require('../htdocs/axil-tty-cap.js'));
} catch (_) {
  const fs = require('fs');
  const path = require('path');
  const src = fs.readFileSync(path.join(__dirname, '../htdocs/axil-tty-cap.js'), 'utf8')
    .replace(/^\s*export\s+function\s+createAxilTtyBridge/m, 'function createAxilTtyBridge');
  createAxilTtyBridge = new Function(src + '\nreturn createAxilTtyBridge;')();
}

let term_max = 0;

function create(element, options = {}) {
  const {
    proto = (typeof location !== 'undefined' && location.protocol === "https:") ? "wss" : "ws",
    port = (typeof window !== 'undefined' && window.location) ? window.location.port : '8080', 
    url = (typeof window !== 'undefined' && window.location) ? proto + "://" + window.location.hostname + ":" + port + '/tty' : 'ws://localhost:8080/tty',
    lineMode = false,
  } = options;

  const hookDefaults = {
    onMessage: function (_ev, _arr) { return true; },
    onOpen: function (_term, _ws) {},
    onClose: function () {},
    cols: 80,
    rows: 25,
  };

  const sub = Object.assign({}, hookDefaults, options.sub);

  const FitAddonClass = (typeof globalThis.FitAddon === 'function') ? globalThis.FitAddon : (globalThis.window && globalThis.window.FitAddon);
  const fitAddon = FitAddonClass ? new FitAddonClass() : null;
  const ResizeObserverClass = globalThis.ResizeObserver || (globalThis.window && globalThis.window.ResizeObserver);
  const resizeObserver = (ResizeObserverClass && fitAddon) ? new ResizeObserverClass(() => { try { fitAddon.fit(); } catch (_) {} }) : null;
  const TermClass = globalThis.Terminal || (globalThis.window && globalThis.window.Terminal);
  const term = TermClass ? new TermClass({
    fontSize: 13,
    fontFamily: 'Consolas,Liberation Mono,Menlo,Courier,monospace',
    allowProposedApi: true,
  }) : null;

  if (term && element && typeof term.open === 'function') {
    term.open(element);
  }

  let ws = {};
  const write = data => { if (term && term.write) term.write(data); };
  const terminst = term_max;
  term_max++;

  function send(text) {
    if (ws && ws.send) ws.send(text);
  }

  sub.term = term;
  sub.send = send;
  sub.write = write;
  sub.resize = (cols, rows) => { if (term && term.resize) term.resize(cols, rows); };
  sub.ws = ws;

  return sub;
}

class TerminalController {
  constructor(element, options = {}) {
    this.session = create(element, options);
  }

  write(data) {
    if (this.session && this.session.write) this.session.write(data);
  }

  send(data) {
    if (this.session && this.session.send) this.session.send(data);
  }

  resize(cols, rows) {
    if (this.session && this.session.resize) this.session.resize(cols, rows);
  }

  close() {
    if (this.session && this.session.ws && typeof this.session.ws.close === 'function') {
      this.session.ws.close();
    }
  }

  get terminal() {
    return this.session ? this.session.term : null;
  }
}

module.exports = {
  create,
  TerminalController,
  createAxilTtyBridge
};
module.exports.default = module.exports;
