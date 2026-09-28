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
    /* Opt-in line mode for routes with no line discipline (a game, not a
     * PTY): buffer keystrokes and submit one "<line>\n" frame per Enter
     * instead of one frame per keystroke. Off by default, and inert whenever
     * the server says WILL ECHO, so /tty and /nd-with-a-shell stay pipes. */
    lineMode = false,
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

  /* Who is echoing. Starts true because the first thing a PTY-backed server says
   * is WILL ECHO, and guessing "the client echoes" until that frame lands would
   * double-echo the first few keystrokes. Every WONT ECHO flips it back. */
  let will_echo = true;
  let raw = false;

  /* What this client has echoed locally, so erase can take a character back.
   * Only meaningful while will_echo is false; it is dropped the moment the
   * server takes over, because past that point the line discipline owns the
   * line and there is nothing here left to correct. */
  let local_line = "";

  /* Pending line for opt-in lineMode. Kept beside local_line (which tracks
   * painted cells for erase): line_buf is what gets submitted on Enter, and
   * both are dropped on any echo-owner change so a stale game line can never
   * leak into a PTY session or vice versa. */
  let line_buf = "";
  const LINE_MAX = 1024;

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
          /* Whatever we painted ourselves is now the server's business, and a
           * half-typed line in the terminal no longer matches the one the driver
           * is holding. Start over rather than try to reconcile them. */
          local_line = "";
          line_buf = "";
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
          local_line = "";
          line_buf = "";
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
      //
      // ...unless the server says WONT ECHO, which means there is no driver on
      // this socket at all: the module that owns it runs a game, and a game does
      // not echo its input line. Then the client is the only echoer there is, so
      // it echoes -- per keystroke, which is the part 1.2.0 got wrong, and not
      // buffered until Enter. This branch used to be unreachable: will_echo was
      // tracked and then never read, so a guest typing a game command saw
      // nothing at all until Enter produced a new view.
      // Line mode (opt-in, game routes only): while the server says WONT
      // ECHO there is no driver to hold a line, so hold it here and submit
      // one "<line>\n" frame per Enter -- the same frame shape sendCmd()
      // uses, which is the only shape the game command parser understands.
      // Inert the moment the server says WILL ECHO (a PTY is born: sh, man,
      // /tty), where every keystroke must reach the driver immediately.
      if (lineMode && !will_echo) {
        // Escape-led input (arrows, Alt combos, pasted ANSI) has no meaning
        // to a line buffer; swallowing it whole keeps partial sequences out
        // of both the buffer and the painted line. Movement stays on the
        // buttons and unfocused hotkeys.
        if (!data.includes("\x1b")) {
          // Collapse CRLF first so one Enter is one submit, not two.
          for (const ch of data.replace(/\r\n/g, "\n")) {
            if (ch === "\r" || ch === "\n") {
              ws.send(line_buf + "\n");
              write("\r\n");
              line_buf = "";
            } else if (ch === "\x7f" || ch === "\b") {
              if (Array.from(line_buf).length) {
                line_buf = Array.from(line_buf).slice(0, -1).join("");
                write("\b \b");
              }
            } else if (ch === " " || (ch > " " && ch < "\x7f") || ch > "\x9f") {
              // Printable only, C1 controls excluded: painting them would
              // fight the view the game sends back.
              if (Array.from(line_buf).length < LINE_MAX) {
                line_buf += ch;
                write(ch);
              }
            }
          }
        }
        return;
      }
      ws.send(data);

      if (!will_echo) {
        for (const ch of data) {
          if (ch === "\x7f" || ch === "\b") {
            // Backspace. The driver's own echo of this is BS SP BS, and so is
            // ours, so the cursor lands in the same place either way.
            if (local_line.length) {
              local_line = local_line.slice(0, -1);
              write("\b \b");
            }
          } else if (ch >= " ") {
            // Printable only. Control keys are the server's to interpret, and
            // painting a CR here would fight the view it sends back.
            local_line += ch;
            write(ch);
          }
        }
      }
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
