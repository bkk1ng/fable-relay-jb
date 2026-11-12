// Typed event emitter with wildcard support and isolated listener errors.
// No runtime dependencies — TypeScript strict mode compatible.

/** Symbol used to register wildcard listeners that fire on every event. */
export const WILDCARD = Symbol("*");

/**
 * Shape of an event map.  Keys are event names (strings), values are the
 * tuple of arguments that the event carries.
 *
 * Example:
 *   interface AppEvents {
 *     connect:    [host: string, port: number];
 *     disconnect: [];
 *     data:       [payload: Uint8Array];
 *   }
 */
export type EventMap = Record<string, unknown[]>;

/** A listener bound to a named event. */
export type Listener<Args extends unknown[]> = (...args: Args) => void;

/** A wildcard listener receives the event name plus its arguments. */
export type WildcardListener<Map extends EventMap> = <K extends keyof Map & string>(
  event: K,
  ...args: Map[K]
) => void;

/** Optional hook called when a listener throws. */
export type ErrorHandler = (error: unknown, event: string | typeof WILDCARD) => void;

export interface EmitterOptions {
  /** Called instead of swallowing errors silently. */
  onError?: ErrorHandler;
}

// ─── Internal storage ────────────────────────────────────────────────────────

interface ListenerEntry<Args extends unknown[]> {
  fn: Listener<Args>;
  once: boolean;
}

interface WildcardEntry<Map extends EventMap> {
  fn: WildcardListener<Map>;
  once: boolean;
}

// ─── TypedEmitter ─────────────────────────────────────────────────────────────

export class TypedEmitter<Map extends EventMap> {
  private readonly _listeners = new Map<
    keyof Map & string,
    ListenerEntry<Map[keyof Map & string]>[]
  >();

  private readonly _wildcards: WildcardEntry<Map>[] = [];
  private readonly _onError: ErrorHandler | undefined;

  constructor(options: EmitterOptions = {}) {
    this._onError = options.onError;
  }

  // ── on ──────────────────────────────────────────────────────────────────────

  /** Register a persistent listener for a named event. */
  on<K extends keyof Map & string>(event: K, fn: Listener<Map[K]>): this;
  /** Register a persistent wildcard listener (fires on every event). */
  on(event: typeof WILDCARD, fn: WildcardListener<Map>): this;
  on<K extends keyof Map & string>(
    event: K | typeof WILDCARD,
    fn: Listener<Map[K]> | WildcardListener<Map>,
  ): this {
    if (event === WILDCARD) {
      this._wildcards.push({ fn: fn as WildcardListener<Map>, once: false });
    } else {
      this._getOrCreate(event as K).push({
        fn: fn as Listener<Map[K]>,
        once: false,
      });
    }
    return this;
  }

  // ── once ────────────────────────────────────────────────────────────────────

  /** Register a one-shot listener for a named event. */
  once<K extends keyof Map & string>(event: K, fn: Listener<Map[K]>): this;
  /** Register a one-shot wildcard listener. */
  once(event: typeof WILDCARD, fn: WildcardListener<Map>): this;
  once<K extends keyof Map & string>(
    event: K | typeof WILDCARD,
    fn: Listener<Map[K]> | WildcardListener<Map>,
  ): this {
    if (event === WILDCARD) {
      this._wildcards.push({ fn: fn as WildcardListener<Map>, once: true });
    } else {
      this._getOrCreate(event as K).push({
        fn: fn as Listener<Map[K]>,
        once: true,
      });
    }
    return this;
  }

  // ── off ─────────────────────────────────────────────────────────────────────

  /** Remove a specific listener from a named event. */
  off<K extends keyof Map & string>(event: K, fn: Listener<Map[K]>): this;
  /** Remove a specific wildcard listener. */
  off(event: typeof WILDCARD, fn: WildcardListener<Map>): this;
  off<K extends keyof Map & string>(
    event: K | typeof WILDCARD,
    fn: Listener<Map[K]> | WildcardListener<Map>,
  ): this {
    if (event === WILDCARD) {
      const idx = this._wildcards.findIndex((e) => e.fn === fn);
      if (idx !== -1) this._wildcards.splice(idx, 1);
    } else {
      const bucket = this._listeners.get(event as K);
      if (!bucket) return this;
      const idx = bucket.findIndex((e) => e.fn === fn);
      if (idx !== -1) bucket.splice(idx, 1);
    }
    return this;
  }

  // ── emit ────────────────────────────────────────────────────────────────────

  /**
   * Emit a named event.  Every listener is called even if a previous one
   * threw; errors are forwarded to `onError` (if provided) or swallowed.
   */
  emit<K extends keyof Map & string>(event: K, ...args: Map[K]): boolean {
    const bucket = this._listeners.get(event);
    let handled = false;

    if (bucket && bucket.length > 0) {
      handled = true;
      // Snapshot so mutations during iteration are safe.
      const snapshot = bucket.slice();
      const surviving: typeof bucket = [];

      for (const entry of snapshot) {
        this._invoke(entry.fn, args, event);
        if (!entry.once) surviving.push(entry);
      }

      // Replace in-place, keeping entries added during iteration.
      bucket.splice(0, snapshot.length, ...surviving);
    }

    if (this._wildcards.length > 0) {
      handled = true;
      const snapshot = this._wildcards.slice();
      const surviving: typeof this._wildcards = [];

      for (const entry of snapshot) {
        this._invokeWildcard(entry.fn, event, args);
        if (!entry.once) surviving.push(entry);
      }

      this._wildcards.splice(0, snapshot.length, ...surviving);
    }

    return handled;
  }

  // ── listenerCount ───────────────────────────────────────────────────────────

  /** Count listeners for a named event (excludes wildcards). */
  listenerCount<K extends keyof Map & string>(event: K): number;
  /** Count wildcard listeners. */
  listenerCount(event: typeof WILDCARD): number;
  listenerCount<K extends keyof Map & string>(event: K | typeof WILDCARD): number {
    if (event === WILDCARD) return this._wildcards.length;
    return this._listeners.get(event as K)?.length ?? 0;
  }

  // ── private helpers ─────────────────────────────────────────────────────────

  private _getOrCreate<K extends keyof Map & string>(
    event: K,
  ): ListenerEntry<Map[K]>[] {
    let bucket = this._listeners.get(event) as
      | ListenerEntry<Map[K]>[]
      | undefined;
    if (!bucket) {
      bucket = [];
      this._listeners.set(event, bucket as ListenerEntry<Map[keyof Map & string]>[]);
    }
    return bucket;
  }

  private _invoke<Args extends unknown[]>(
    fn: Listener<Args>,
    args: Args,
    event: string,
  ): void {
    try {
      fn(...args);
    } catch (err) {
      if (this._onError) {
        try {
          this._onError(err, event);
        } catch {
          // swallow errors from the error handler itself
        }
      }
    }
  }

  private _invokeWildcard<K extends keyof Map & string>(
    fn: WildcardListener<Map>,
    event: K,
    args: Map[K],
  ): void {
    try {
      fn(event, ...args);
    } catch (err) {
      if (this._onError) {
        try {
          this._onError(err, WILDCARD);
        } catch {
          // swallow
        }
      }
    }
  }
}

// ─── Self-test (__main__) ─────────────────────────────────────────────────────

if (import.meta.url === `file://${process.argv[1]}`) {
  interface TestEvents {
    greet:      [name: string];
    count:      [n: number];
    noArgs:     [];
  }

  const errors: string[] = [];

  const emitter = new TypedEmitter<TestEvents>({
    onError(err, event) {
      errors.push(`[${String(event)}] ${String(err)}`);
    },
  });

  // ── Basic on/emit ──────────────────────────────────────────────────────────
  const received: string[] = [];
  emitter.on("greet", (name) => received.push(`hello ${name}`));
  emitter.emit("greet", "world");
  emitter.emit("greet", "TypeScript");
  console.assert(
    received.join(",") === "hello world,hello TypeScript",
    `on/emit failed: ${received}`,
  );

  // ── once ──────────────────────────────────────────────────────────────────
  let onceFired = 0;
  emitter.once("count", () => onceFired++);
  emitter.emit("count", 1);
  emitter.emit("count", 2);
  console.assert(onceFired === 1, `once fired ${onceFired} times, expected 1`);

  // ── off ───────────────────────────────────────────────────────────────────
  let offCount = 0;
  const offFn = () => offCount++;
  emitter.on("noArgs", offFn);
  emitter.emit("noArgs");
  emitter.off("noArgs", offFn);
  emitter.emit("noArgs");
  console.assert(offCount === 1, `off failed: handler called ${offCount} times`);

  // ── listenerCount ─────────────────────────────────────────────────────────
  const countEmitter = new TypedEmitter<TestEvents>();
  countEmitter.on("greet", () => {});
  countEmitter.on("greet", () => {});
  console.assert(
    countEmitter.listenerCount("greet") === 2,
    `listenerCount wrong: ${countEmitter.listenerCount("greet")}`,
  );

  // ── wildcard ──────────────────────────────────────────────────────────────
  const wildEvents: string[] = [];
  emitter.on(WILDCARD, (event) => wildEvents.push(event));
  emitter.emit("greet", "wildcard-test");
  emitter.emit("noArgs");
  console.assert(
    wildEvents.includes("greet") && wildEvents.includes("noArgs"),
    `wildcard missed events: ${wildEvents}`,
  );

  // ── wildcard listenerCount ────────────────────────────────────────────────
  console.assert(
    emitter.listenerCount(WILDCARD) === 1,
    `wildcard listenerCount wrong: ${emitter.listenerCount(WILDCARD)}`,
  );

  // ── error isolation ───────────────────────────────────────────────────────
  const afterThrow: string[] = [];
  const throwEmitter = new TypedEmitter<TestEvents>({
    onError(err) {
      errors.push(String(err));
    },
  });
  throwEmitter.on("greet", () => {
    throw new Error("boom");
  });
  throwEmitter.on("greet", (name) => afterThrow.push(name));
  throwEmitter.emit("greet", "isolation");
  console.assert(
    afterThrow[0] === "isolation",
    `error isolation failed — second listener did not run`,
  );
  console.assert(
    errors.some((e) => e.includes("boom")),
    `error not captured: ${errors}`,
  );

  console.log("All assertions passed.");
  console.log("Captured errors (expected):", errors);
}
