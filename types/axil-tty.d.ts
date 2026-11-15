export interface TerminalOptions {
  proto?: string;
  port?: string | number;
  url?: string;
  lineMode?: boolean;
  sub?: {
    onMessage?: (ev: any, arr: any) => boolean;
    onOpen?: (term: any, ws: any) => void;
    onClose?: () => void;
    cols?: number;
    rows?: number;
  };
}

export interface TerminalSession {
  term: any;
  send: (text: string | Uint8Array) => void;
  write: (data: string) => void;
  resize: (cols: number, rows: number) => void;
  ws: any;
}

export declare function create(element: HTMLElement, options?: TerminalOptions): TerminalSession;

export declare class TerminalController {
  session: TerminalSession;
  constructor(element: HTMLElement, options?: TerminalOptions);
  write(data: string): void;
  send(data: string | Uint8Array): void;
  resize(cols: number, rows: number): void;
  close(): void;
  readonly terminal: any;
}

export { createAxilTtyBridge } from "../htdocs/axil-tty-cap.js";
