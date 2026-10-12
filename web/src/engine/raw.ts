// Loads the C engine (compiled to WebAssembly) and exposes its single JSON entry point nr_api_call.
import createNetRescueEngine, { type ModuleOptions } from './generated/netrescue_engine.mjs';

export type RawCall = (requestJson: string) => string;

export async function loadRawEngine(options: ModuleOptions = {}): Promise<RawCall> {
  const module = await createNetRescueEngine(options);
  return (requestJson: string) => module.ccall('nr_api_call', 'string', ['string'], [requestJson]);
}
