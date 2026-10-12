// Hand-written type declaration for the Emscripten-generated module (kept when the engine is rebuilt).
export interface NetRescueModule {
  ccall(name: 'nr_api_call', returnType: 'string', argTypes: ['string'], args: [string]): string;
}
export interface ModuleOptions {
  locateFile?: (path: string, prefix: string) => string;
  wasmBinary?: ArrayBuffer | Uint8Array;
}
declare const createNetRescueEngine: (options?: ModuleOptions) => Promise<NetRescueModule>;
export default createNetRescueEngine;
