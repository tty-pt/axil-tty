/*
 * axil-tty-bridge.js — Browser WebAssembly bridge for terminal rendering.
 * Provides the "tty" WASM capability module using xterm.js.
 */

function readWasmStr(memory, ptr, len) {
	if (!memory || !ptr || len <= 0) return '';
	return new TextDecoder().decode(new Uint8Array(memory.buffer, ptr, len));
}

function getExport(exports, names) {
	if (!exports) return null;
	for (const name of names) {
		if (typeof exports[name] === 'function') return exports[name];
	}
	return null;
}

export function createAxilTtyBridge(root, getWasm) {
	let term = null;
	let busUnsub = null;
	let will_echo = true;
	let line_buf = "";
	const LINE_MAX = 1024;

	function attachTerm(termEl, TerminalClass) {
		if (term) return;
		const lineMode = termEl.getAttribute('data-line-mode') !== 'false';
		will_echo = !lineMode;
		term = new TerminalClass({
			cursorBlink: true,
			theme: { background: '#0a0a0c', foreground: '#e0e0e0' }
		});

		function sendNaws() {
			if (!term || !window.__axil_ws || !window.__axil_ws.send) return;
			const cols = term.cols || 80;
			const rows = term.rows || 25;
			const naws = new Uint8Array([
				255, 250, 31,
				(cols >> 8) & 0xff, cols & 0xff,
				(rows >> 8) & 0xff, rows & 0xff,
				255, 240
			]);
			window.__axil_ws.send(naws);
		}

		function sendOut(text) {
			const wasm = getWasm();
			const fn = getExport(wasm, ['wasm_term_on_input']);
			const malloc = getExport(wasm, ['malloc']);
			const free = getExport(wasm, ['free']);
			if (fn && malloc && free && wasm && wasm.memory) {
				const bytes = new TextEncoder().encode(text);
				const ptr = malloc(bytes.length);
				const uptr = ptr >>> 0;
				if (uptr + bytes.length <= wasm.memory.buffer.byteLength) {
					new Uint8Array(wasm.memory.buffer, uptr, bytes.length).set(bytes);
					fn(ptr, bytes.length);
				}
				free(ptr);
			} else if (window.__axil_ws && window.__axil_ws.send) {
				window.__axil_ws.send(text);
			}
		}

		if (window.FitAddon && window.FitAddon.FitAddon) {
			const fit = new window.FitAddon.FitAddon();
			term.loadAddon(fit);
			term.open(termEl);
			try { fit.fit(); } catch (_) {}
			window.addEventListener('resize', () => {
				try { fit.fit(); sendNaws(); } catch (_) {}
			});
		} else {
			term.open(termEl);
		}

		term.onResize(() => sendNaws());

		termEl.addEventListener('focusin', () => {
			const holder = document.getElementById('holder');
			if (holder) holder.classList.add('terminal-mode');
		});
		termEl.addEventListener('focusout', () => {
			const holder = document.getElementById('holder');
			if (holder) holder.classList.remove('terminal-mode');
		});

		term.onData((data) => {
			if (lineMode && !will_echo) {
				if (!data.includes('\x1b')) {
					for (const ch of data.replace(/\r\n/g, '\n')) {
						if (ch === '\r' || ch === '\n') {
							sendOut(line_buf + '\n');
							term.write('\r\n');
							line_buf = '';
						} else if (ch === '\x7f' || ch === '\b') {
							if (line_buf.length) {
								line_buf = line_buf.slice(0, -1);
								term.write('\b \b');
							}
						} else if (ch === ' ' || (ch > ' ' && ch < '\x7f') || ch > '\x9f') {
							if (line_buf.length < LINE_MAX) {
								line_buf += ch;
								term.write(ch);
							}
						}
					}
				}
				return;
			}
			sendOut(data);
		});

		// Subscribe to underlying text streams from the shared axil bus
		if (busUnsub) {
			busUnsub();
			busUnsub = null;
		}
		if (window.__axil_ws && window.__axil_ws.subscribe) {
			busUnsub = window.__axil_ws.subscribe((bytes, isText) => {
				if (!isText && bytes.length >= 2 && bytes[0] === 0x23 && bytes[1] === 0x62) {
					// Magic '#b' BCP protocol frame — handled by WASM BCP decoder, suppress from terminal
					return;
				}
				if (!isText && bytes.length > 0 && bytes[0] === 255) {
					// Telnet IAC negotiation — parse and suppress from terminal
					if (bytes.length >= 3) {
						if (bytes[1] === 252) { // WONT
							if (bytes[2] === 1) { // TELOPT_ECHO
								will_echo = false;
							}
						} else if (bytes[1] === 251) { // WILL
							if (bytes[2] === 1) { // TELOPT_ECHO
								will_echo = true;
							}
						} else if (bytes[1] === 253 && bytes[2] === 31) { // DO TELOPT_NAWS
							sendNaws();
						}
					}
					return;
				}

				// Clean ANSI or text stream
				const decoded = new TextDecoder().decode(bytes);
				term.write(decoded);
			});
		}
	}

	return {
		mount() {
			if (term) return;
			const termEl = (root && root.querySelector('[data-bud-terminal]')) || document.querySelector('[data-bud-terminal]');
			if (!termEl || termEl.__attached) return;
			termEl.__attached = true;
			if (window.Terminal) {
				attachTerm(termEl, window.Terminal);
			} else {
				const script = document.createElement('script');
				script.src = '/vendor/xterm.js';
				const link = document.createElement('link');
				link.rel = 'stylesheet';
				link.href = '/vendor/xterm.css';
				document.head.appendChild(link);
				script.onload = () => {
					if (window.Terminal) attachTerm(termEl, window.Terminal);
				};
				script.onerror = () => {
					const fallbackScript = document.createElement('script');
					fallbackScript.src = 'https://unpkg.com/@xterm/xterm@6.0.0/lib/xterm.js';
					const fallbackLink = document.createElement('link');
					fallbackLink.rel = 'stylesheet';
					fallbackLink.href = 'https://unpkg.com/@xterm/xterm@6.0.0/css/xterm.css';
					document.head.appendChild(fallbackLink);
					fallbackScript.onload = () => {
						if (window.Terminal) attachTerm(termEl, window.Terminal);
					};
					document.head.appendChild(fallbackScript);
				};
				document.head.appendChild(script);
			}
		},
		makeImports() {
			return {
				term_write(str_ptr, str_len) {
					if (!term) return;
					const exports = getWasm();
					if (!exports || !exports.memory) return;
					const text = readWasmStr(exports.memory, str_ptr, str_len);
					term.write(text);
				}
			};
		},
		makeTtyImports() {
			return this.makeImports();
		},
		destroy() {
			if (busUnsub) {
				busUnsub();
				busUnsub = null;
			}
			if (term) {
				try { term.dispose(); } catch (_) {}
				term = null;
			}
		}
	};
}

if (typeof window !== 'undefined') {
	window.__wasm_capability_registry = window.__wasm_capability_registry || {};
	const ttyFactory = (root, getWasm) => createAxilTtyBridge(root, getWasm);
	window.__wasm_capability_registry.tty = ttyFactory;
	window.__wasm_capability_registry['axil-tty'] = ttyFactory;
}
