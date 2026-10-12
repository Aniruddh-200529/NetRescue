import '@testing-library/jest-dom/vitest';

// jsdom lacks ResizeObserver (used by Recharts and React Flow).
class ResizeObserverStub { observe() {} unobserve() {} disconnect() {} }
(globalThis as unknown as { ResizeObserver: unknown }).ResizeObserver = ResizeObserverStub;
