// import { Terminal } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";
import { WebLinksAddon } from "@xterm/addon-web-links";
import "./axil-tty.css";

let term_max = 0;

// TODO change this to a class
export
function create(element, options = {}) {
  const {
    proto = location.protocol === "https:" ? "wss" : "ws",
    port = window.location.port, 
    url = proto + "://" + window.location.hostname + ":" + port + '/tty',
  } = options;

  const hookDefaults = {
    onMessage: function (_ev, _arr) { return true; },
    onOpen: function (_term, _ws) {},
    onClose: function () {},
    cols: 80,
    rows: 25,
  };

  // Merged, not substituted: a caller that overrides onMessage must still get
  // the other four, or onOpen throws on connect. The instance is a copy, so
  // mutating what create() returns never reaches back into the caller's object.
  const sub = Object.assign({}, hookDefaults, options.sub);

  const fitAddon = new FitAddon();
  const resizeObserver = new ResizeObserver(() => fitAddon.fit());
  const term = new globalThis.Terminal({
    fontSize: 13,
    fontFamily: 'Consolas,Liberation Mono,Menlo,Courier,monospace',
    allowProposedApi: true,
  });

  let ws = {};

  const write = data => term.write(data);

  const terminst = term_max;
  term_max++;

  function send(text) {
    ws.send(text);
  }

  const decoder = new TextDecoder('utf-8');
  let connected = false;

  /* The server echoes for us (WILL ECHO), so assume it from the first
   * keystroke rather than echoing locally until the negotiation frame lands. */
  let will_echo = true;
  let raw = false;

  function onMessage(ev) {
    const arr = new Uint8Array(ev.data);
    if (!sub.onMessage(ev, arr))
      return;
    else if (arr[0] != 255) {
      const data = decoder.decode(arr);
      term.write(data);
    // } else if (arr[1] == 254) { // DONT
    // } else if (arr[1] == 253) { // DO
    } else if (arr[1] == 252) { // WONT
      switch (arr[2]) {
        case 1: // TELOPT_ECHO
          will_echo = false;
          if (options.debug)
            console.log("WONT ECHO");
          break;
        case 3: // TELOPT_SGA
          raw = false;
          if (options.debug)
            console.log("WONT SGA (ICANON/not raw)");
          break;
      }
    } else if (arr[1] == 251) { // WILL
      switch (arr[2]) {
        case 1: // TELOPT_ECHO
          will_echo = true;
          if (options.debug)
            console.log("WILL ECHO");
          break;
        case 3: // TELOPT_SGA
          raw = true;
          if (options.debug)
            console.log("WILL SGA (not ICANON/raw)");
          break;
      }
    } else if (arr[1] == 250) { // SB
      switch (arr[2]) {
        case 31: // TELOPT_NAWS
      }
    }
  }

  function resize(cols, rows) {
    sub.cols = cols;
    sub.rows = rows;

    if (!connected)
      return;

    const IAC = 255;
    const SB = 250;
    const NAWS = 31;
    const SE = 240;
  
    const colsHighByte = cols >> 8;
    const colsLowByte = cols & 0xFF;
    const rowsHighByte = rows >> 8;
    const rowsLowByte = rows & 0xFF;
  
    const nawsCommand = new Uint8Array([
      IAC, SB, NAWS,
      colsHighByte, colsLowByte,
      rowsHighByte, rowsLowByte,
      IAC, SE
    ]);
  
    ws.send(nawsCommand);
  }

  function open(parent) {
    parent.scrollTop = parent.scrollHeight;
    term.loadAddon(fitAddon);
    term.loadAddon(new WebLinksAddon());
    term.open(parent);
    term.perm = "";
  
    term.onResize(({ cols, rows }) => resize(cols, rows));
    resizeObserver.observe(parent);
  
    term.element.addEventListener("focusin", () => {
      term.focused = true;
    });
    term.element.addEventListener("focusout", () => {
      term.focused = false;
    });

    term.onData(data => {
      if (options.debug)
        console.log("term.onData", data, data.charAt(0), raw, will_echo);
      // The client is a pipe. The server negotiates WILL ECHO once, at connect,
      // and the PTY's line discipline does the echoing, line editing and CR/LF
      // translation -- so every keystroke goes out the moment it is typed and
      // the driver echoes it straight back. This is the whole trick: a driver
      // with ECHO on echoes each byte as it arrives while still holding the
      // line until Enter, which is what makes a terminal feel like a terminal.
      //
      // The 1.2.0 client instead buffered the line here and echoed it locally
      // when the server said WONT ECHO, so it withheld the bytes until Enter
      // and the driver had nothing to echo in the meantime: nothing appeared
      // until the line was submitted. Buffering and echoing are both the
      // driver's job, so do neither.
      ws.send(data);
    });
    return term;
  }

  open(element);

  function connect() {
    ws = new WebSocket(url, 'binary');
    ws.binaryType = 'arraybuffer';

    ws.onopen = () => {
      connected = true;
      resize(term.cols, term.rows);
      sub.onOpen(term, ws);
    };

    ws.onmessage = onMessage;

    ws.onclose = () => {
      connected = false;
      sub.onClose();
      sub.timeout = setTimeout(() => {
        clearTimeout(sub.timeout);
        connect();
      }, 3000);
    };
  };

  connect();

  sub.term = term;
  sub.send = send;
  sub.write = write;
  sub.resize = resize;
  sub.ws = ws;

  return sub;
}

window.ttyAxil = { create };
export default { create };
