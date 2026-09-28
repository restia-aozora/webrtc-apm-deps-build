/** Opaque native handle. Only create() can create a valid instance. */
export interface ApmHandle {
  readonly kind: string;
}

/** Mono PCM16, 8/16/32/48 kHz; throws if initialization fails. */
export const create: (sampleRate: number, channels: number,
  noiseSuppression: boolean, gainControl: boolean) => ApmHandle;
/** Exactly 10 ms per call; samples are modified in place. */
export const processReverse: (handle: ApmHandle, samples: Int16Array) => number;
export const processCapture: (handle: ApmHandle, samples: Int16Array) => number;
/** Measured render-to-capture delay; clamped to 0..500 ms. */
export const setStreamDelay: (handle: ApmHandle, milliseconds: number) => void;
/** -1 = unknown, 0 = no voice, 1 = voice. */
export const getVoiceDetected: (handle: ApmHandle) => number;
/** Safe to repeat. Subsequent processing returns -1002. */
export const release: (handle: ApmHandle) => void;
