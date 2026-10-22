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

	function attachTerm(termEl, TerminalClass) {
		term = new TerminalClass({
			cursorBlink: true,
			theme: { background: '#0a0a0c', foreground: '#e0e0e0' }
		});
		if (window.FitAddon && window.FitAddon.FitAddon) {
			const fit = new window.FitAddon.FitAddon();
			term.loadAddon(fit);
			term.open(termEl);
			try { fit.fit(); } catch (_) {}
			window.addEventListener('resize', () => {
				try { fit.fit(); } catch (_) {}
			});
		} else {
			term.open(termEl);
		}

		term.onData((data) => {
			const wasm = getWasm();
			const fn = getExport(wasm, ['wasm_term_on_input']);
			const malloc = getExport(wasm, ['malloc']);
			const free = getExport(wasm, ['free']);
			if (fn && malloc && free && wasm.memory) {
				const bytes = new TextEncoder().encode(data);
				const ptr = malloc(bytes.length);
				if (ptr) {
					new Uint8Array(wasm.memory.buffer, ptr, bytes.length).set(bytes);
					fn(ptr, bytes.length);
					free(ptr);
				}
			} else if (window.__axil_ws && window.__axil_ws.send) {
				window.__axil_ws.send(data);
			}
		});

		// Subscribe to underlying text streams from the shared axil bus if WASM isn't handling it
		if (window.__axil_ws && window.__axil_ws.subscribe) {
			busUnsub = window.__axil_ws.subscribe((bytes, isText) => {
				// Only display if text or not magic '#b'
				if (isText) {
					term.write(new TextDecoder().decode(bytes));
				} else if (bytes.length < 2 || bytes[0] !== 0x23 || bytes[1] !== 0x62) {
					// Non-BCP binary text stream
					term.write(new TextDecoder().decode(bytes));
				}
			});
		}
	}

	return {
		mount() {
			const termEl = root ? root.querySelector('[data-bud-terminal]') : null;
			if (termEl && !term) {
				if (window.Terminal) {
					attachTerm(termEl, window.Terminal);
				} else {
					const script = document.createElement('script');
					script.src = 'https://unpkg.com/@xterm/xterm@6.0.0/lib/xterm.js';
					const link = document.createElement('link');
					link.rel = 'stylesheet';
					link.href = 'https://unpkg.com/@xterm/xterm@6.0.0/css/xterm.css';
					document.head.appendChild(link);
					script.onload = () => {
						if (window.Terminal) attachTerm(termEl, window.Terminal);
					};
					document.head.appendChild(script);
				}
			}
		},
		makeTtyImports() {
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
	let sharedTtyBridge = null;
	window.__wasm_capability_registry.tty = (root, getWasm) => {
		if (!sharedTtyBridge) sharedTtyBridge = createAxilTtyBridge(root, getWasm);
		const b = sharedTtyBridge;
		return {
			mount() { b.mount(); },
			destroy() { b.destroy(); },
			makeImports() { return b.makeTtyImports(); }
		};
	};
}
