export interface AxilTtyBridge {
  mount(): void;
  makeImports(): {
    term_write(str_ptr: number, str_len: number): void;
  };
  makeTtyImports(): {
    term_write(str_ptr: number, str_len: number): void;
  };
  destroy(): void;
}

export declare function createAxilTtyBridge(root?: any, getWasm?: () => any): AxilTtyBridge;
