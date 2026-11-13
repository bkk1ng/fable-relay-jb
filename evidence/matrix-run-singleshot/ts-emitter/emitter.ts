// Typed event emitter with wildcard support and isolated listener errors.
// No runtime dependencies — pure TypeScript strict-mode.

/** Wildcard event key: listeners registered here fire on every emit. */
export const WILDCARD = Symbol("*");

/**
 * Shape of an event map.  Keys are event names (string or symbol);
 * values are the tuple of arguments passed to listeners for that event.
 *
 * Example:
 *   interface MyEvents {
 *     connect: [host: string, port: number];
 *     disconnect: [];
 *     data: [payload: Uint8Array];
 *   }
 */
export type EventMap = Record<string | symbol, unknown[]>;

type Listener<Args extends unknown[]> = (...args: Args) => void;

/**
 * A listener entry.  `once` marks it for removal after the first call.
 */
interface Entry<Args extends unknown[]> {
  fn: Listener<Args>;
  once: boolean;
}

/**
 * Typed event emitter.
 *
 * @template Events  An EventMap describing every named event and its args.
 *
 * Wildcard listeners receive the event name as the first argument followed
 * by all event arguments:  (event: keyof Events, ...args) => void
 */
export class Emitter<Events extends EventMap> {
  // Named event listeners
  private readonly _listeners = new Map<
    keyof Events,
    Array<Entry<Events[keyof Events]>>
  >();

  // Wildcard listeners  — typed loosely because they accept any event
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  private readonly _wildcards: Array<Entry<any>> = [];

  // -------------------------------------------------------------------------
  // on
  // -------------------------------------------------------------------------

  /** Register a persistent listener for a named event. */
  on<K extends keyof Events>(event: K, fn: Listener<Events[K]>): this;

  /** Register a persistent wildcard listener.  Fires on every emit. */
  on(event: typeof WILDCARD, fn: (event: keyof Events, ...args: unknown[]) => void): this;

  on(
    event: keyof Events | typeof WILDCARD,
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    fn: Listener<any>,
  ): this {
    this._add(event, fn, false);
    return this;
  }

  // -------------------------------------------------------------------------
  // once
  // -------------------------------------------------------------------------

  /** Register a one-shot listener for a named event. */
  once<K extends keyof Events>(event: K, fn: Listener<Events[K]>): this;

  /** Register a one-shot wildcard listener. */
  once(event: typeof WILDCARD, fn: (event: keyof Events, ...args: unknown[]) => void): this;

  once(
    event: keyof Events | typeof WILDCARD,
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    fn: Listener<any>,
  ): this {
    this._add(event, fn, true);
    return this;
  }

  // -------------------------------------------------------------------------
  // off
  // -------------------------------------------------------------------------

  /** Remove a specific listener from a named event or the wildcard channel. */
  off<K extends keyof Events>(event: K, fn: Listener<Events[K]>): this;
  off(event: typeof WILDCARD, fn: (event: keyof Events, ...args: unknown[]) => void): this;

  off(
    event: keyof Events | typeof WILDCARD,
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    fn: Listener<any>,
  ): this {
    if (event === WILDCARD) {
      this._removeFrom(this._wildcards, fn);
      return this;
    }
    const bucket = this._listeners.get(event as keyof Events);
    if (bucket) {
      this._removeFrom(bucket, fn);
      if (bucket.length === 0) this._listeners.delete(event as keyof Events);
    }
    return this;
  }

  // -------------------------------------------------------------------------
  // emit
  // -------------------------------------------------------------------------

  /**
   * Emit an event.  Every listener is called even if a previous one threw;
   * errors are collected and re-thrown as an AggregateError after all
   * listeners have run.
   */
  emit<K extends keyof Events>(event: K, ...args: Events[K]): void {
    const errors: unknown[] = [];

    // Named listeners
    const bucket = this._listeners.get(event);
    if (bucket) {
      // Snapshot before iteration so once-removals don't skip entries
      for (const entry of [...bucket]) {
        if (entry.once) this._removeFrom(bucket, entry.fn);
        try {
          entry.fn(...args);
        } catch (err) {
          errors.push(err);
        }
      }
      if (bucket.length === 0) this._listeners.delete(event);
    }

    // Wildcard listeners
    for (const entry of [...this._wildcards]) {
      if (entry.once) this._removeFrom(this._wildcards, entry.fn);
      try {
        entry.fn(event, ...args);
      } catch (err) {
        errors.push(err);
      }
    }

    if (errors.length === 1) throw errors[0];
    if (errors.length > 1) {
      throw new AggregateError(errors, "One or more event listeners threw");
    }
  }

  // -------------------------------------------------------------------------
  // listenerCount
  // -------------------------------------------------------------------------

  /** Number of listeners registered for a named event or the wildcard channel. */
  listenerCount<K extends keyof Events>(event: K): number;
  listenerCount(event: typeof WILDCARD): number;

  listenerCount(event: keyof Events | typeof WILDCARD): number {
    if (event === WILDCARD) return this._wildcards.length;
    return this._listeners.get(event as keyof Events)?.length ?? 0;
  }

  // -------------------------------------------------------------------------
  // Private helpers
  // -------------------------------------------------------------------------

  private _add(
    event: keyof Events | typeof WILDCARD,
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    fn: Listener<any>,
    once: boolean,
  ): void {
    if (event === WILDCARD) {
      this._wildcards.push({ fn, once });
      return;
    }
    let bucket = this._listeners.get(event as keyof Events);
    if (!bucket) {
      bucket = [];
      this._listeners.set(event as keyof Events, bucket);
    }
    bucket.push({ fn, once } as Entry<Events[keyof Events]>);
  }

  private _removeFrom(
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    bucket: Array<Entry<any>>,
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    fn: Listener<any>,
  ): void {
    const idx = bucket.findIndex((e) => e.fn === fn);
    if (idx !== -1) bucket.splice(idx, 1);
  }
}

// =============================================================================
// Self-test  (__main__ equivalent)
// =============================================================================

if (process.argv[1] === new URL(import.meta.url).pathname) {
  let passed = 0;
  let failed = 0;

  function assert(label: string, condition: boolean): void {
    if (condition) {
      console.log(`  ✓  ${label}`);
      passed++;
    } else {
      console.error(`  ✗  ${label}`);
      failed++;
    }
  }

  // ---------------------------------------------------------------------------
  // Setup
  // ---------------------------------------------------------------------------

  interface TestEvents {
    greet: [name: string];
    count: [n: number];
    empty: [];
  }

  console.log("\nTyped Emitter — self-test\n");

  // ---------------------------------------------------------------------------
  // on / emit
  // ---------------------------------------------------------------------------

  {
    const em = new Emitter<TestEvents>();
    const received: string[] = [];
    em.on("greet", (name) => received.push(name));
    em.emit("greet", "Alice");
    em.emit("greet", "Bob");
    assert("on: receives both emits", received.join(",") === "Alice,Bob");
  }

  // ---------------------------------------------------------------------------
  // once
  // ---------------------------------------------------------------------------

  {
    const em = new Emitter<TestEvents>();
    let calls = 0;
    em.once("count", () => calls++);
    em.emit("count", 1);
    em.emit("count", 2);
    assert("once: fires exactly once", calls === 1);
    assert("once: auto-removed after firing", em.listenerCount("count") === 0);
  }

  // ---------------------------------------------------------------------------
  // off
  // ---------------------------------------------------------------------------

  {
    const em = new Emitter<TestEvents>();
    let calls = 0;
    const fn = (): void => { calls++; };
    em.on("count", fn);
    em.emit("count", 1);
    em.off("count", fn);
    em.emit("count", 2);
    assert("off: stops delivery after removal", calls === 1);
    assert("off: listenerCount drops to 0", em.listenerCount("count") === 0);
  }

  // ---------------------------------------------------------------------------
  // listenerCount
  // ---------------------------------------------------------------------------

  {
    const em = new Emitter<TestEvents>();
    em.on("greet", () => {});
    em.on("greet", () => {});
    assert("listenerCount: 2 after two on()", em.listenerCount("greet") === 2);
  }

  // ---------------------------------------------------------------------------
  // wildcard
  // ---------------------------------------------------------------------------

  {
    const em = new Emitter<TestEvents>();
    const seen: Array<[keyof TestEvents, unknown]> = [];
    em.on(WILDCARD, (event, ...args) => seen.push([event as keyof TestEvents, args[0]]));
    em.emit("greet", "Carol");
    em.emit("count", 42);
    assert("wildcard: catches greet", seen[0]?.[0] === "greet" && seen[0]?.[1] === "Carol");
    assert("wildcard: catches count", seen[1]?.[0] === "count" && seen[1]?.[1] === 42);
  }

  // ---------------------------------------------------------------------------
  // wildcard once
  // ---------------------------------------------------------------------------

  {
    const em = new Emitter<TestEvents>();
    let calls = 0;
    em.once(WILDCARD, () => calls++);
    em.emit("greet", "Dave");
    em.emit("greet", "Eve");
    assert("wildcard once: fires only once", calls === 1);
    assert("wildcard once: removed after firing", em.listenerCount(WILDCARD) === 0);
  }

  // ---------------------------------------------------------------------------
  // wildcard off
  // ---------------------------------------------------------------------------

  {
    const em = new Emitter<TestEvents>();
    let calls = 0;
    const fn = (): void => { calls++; };
    em.on(WILDCARD, fn);
    em.emit("greet", "Frank");
    em.off(WILDCARD, fn);
    em.emit("greet", "Grace");
    assert("wildcard off: stops after removal", calls === 1);
  }

  // ---------------------------------------------------------------------------
  // Error isolation — one throw must not stop other listeners
  // ---------------------------------------------------------------------------

  {
    const em = new Emitter<TestEvents>();
    const log: string[] = [];
    em.on("greet", () => { log.push("before"); throw new Error("boom"); });
    em.on("greet", () => { log.push("after"); });

    let caught = false;
    try {
      em.emit("greet", "Heidi");
    } catch {
      caught = true;
    }
    assert("error isolation: second listener still runs", log.includes("after"));
    assert("error isolation: error is re-thrown", caught);
  }

  // ---------------------------------------------------------------------------
  // AggregateError when multiple listeners throw
  // ---------------------------------------------------------------------------

  {
    const em = new Emitter<TestEvents>();
    em.on("empty", () => { throw new Error("first"); });
    em.on("empty", () => { throw new Error("second"); });

    let agg: AggregateError | null = null;
    try {
      em.emit("empty");
    } catch (err) {
      if (err instanceof AggregateError) agg = err;
    }
    assert("multi-error: AggregateError thrown", agg !== null);
    assert("multi-error: contains both errors", agg?.errors.length === 2);
  }

  // ---------------------------------------------------------------------------
  // Wildcard error isolation
  // ---------------------------------------------------------------------------

  {
    const em = new Emitter<TestEvents>();
    const log: string[] = [];
    em.on(WILDCARD, () => { throw new Error("wildcard boom"); });
    em.on("greet", () => { log.push("named ran"); });

    try { em.emit("greet", "Ivan"); } catch { /* expected */ }
    // Named fires before wildcards, so "named ran" should be in the log
    assert("wildcard error: named listener still ran", log.includes("named ran"));
  }

  // ---------------------------------------------------------------------------
  // No cross-event leakage
  // ---------------------------------------------------------------------------

  {
    const em = new Emitter<TestEvents>();
    let countFired = false;
    em.on("count", () => { countFired = true; });
    em.emit("greet", "Judy");
    assert("no leakage: count listener not called on greet emit", !countFired);
  }

  // ---------------------------------------------------------------------------
  // Summary
  // ---------------------------------------------------------------------------

  console.log(`\n${passed} passed, ${failed} failed\n`);
  if (failed > 0) process.exit(1);
}
