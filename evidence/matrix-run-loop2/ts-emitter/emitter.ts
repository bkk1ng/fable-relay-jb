// emitter.ts — typed event emitter with wildcard support
// TypeScript strict, zero runtime dependencies

/** Signature for a specific event listener */
type EventListener<T> = (data: T) => void;

/** Signature for a wildcard listener — receives the event name and its data */
type WildcardListener<EventMap extends Record<string, unknown>> = <
  K extends keyof EventMap
>(
  event: K,
  data: EventMap[K]
) => void;

/** Internal storage for one event */
interface ListenerEntry<T> {
  fn: EventListener<T>;
  once: boolean;
}

/**
 * TypedEmitter<EventMap>
 *
 * EventMap must be a plain object whose keys are event names and whose
 * values are the corresponding payload types, e.g.:
 *
 *   interface MyEvents {
 *     connect: { host: string; port: number };
 *     disconnect: void;
 *     data: Uint8Array;
 *   }
 *
 *   const emitter = new TypedEmitter<MyEvents>();
 */
export class TypedEmitter<EventMap extends Record<string, unknown>> {
  // Specific-event listeners keyed by event name
  private readonly _listeners: {
    [K in keyof EventMap]?: Array<ListenerEntry<EventMap[K]>>;
  } = {};

  // Wildcard listeners receive every event
  private readonly _wildcards: Array<{
    fn: WildcardListener<EventMap>;
    once: boolean;
  }> = [];

  // ── Specific-event API ───────────────────────────────────────────────────

  /**
   * Register a persistent listener for `event`.
   * Returns `this` for chaining.
   */
  on<K extends keyof EventMap>(event: K, fn: EventListener<EventMap[K]>): this {
    this._addListener(event, fn, false);
    return this;
  }

  /**
   * Register a one-shot listener for `event`.
   * It is automatically removed after the first invocation.
   * Returns `this` for chaining.
   */
  once<K extends keyof EventMap>(
    event: K,
    fn: EventListener<EventMap[K]>
  ): this {
    this._addListener(event, fn, true);
    return this;
  }

  /**
   * Remove a previously registered listener for `event`.
   * If `fn` is omitted, all listeners for that event are removed.
   * Returns `this` for chaining.
   */
  off<K extends keyof EventMap>(
    event: K,
    fn?: EventListener<EventMap[K]>
  ): this {
    if (fn === undefined) {
      delete this._listeners[event];
    } else {
      const bucket = this._listeners[event];
      if (bucket) {
        const idx = bucket.findIndex((e) => e.fn === fn);
        if (idx !== -1) bucket.splice(idx, 1);
        if (bucket.length === 0) delete this._listeners[event];
      }
    }
    return this;
  }

  /**
   * Emit `event` with `data`, invoking all matching and wildcard listeners.
   *
   * Listener errors are isolated: if one listener throws, the error is
   * caught and re-emitted on the special `"error"` event (if `EventMap`
   * includes it and a listener is registered). Otherwise the error is
   * forwarded to `console.error` so remaining listeners still run.
   *
   * Returns `true` if at least one specific listener or wildcard was called.
   */
  emit<K extends keyof EventMap>(event: K, data: EventMap[K]): boolean {
    let called = false;

    // Collect specific-event entries; snapshot so once-removal doesn't
    // mutate the array mid-iteration.
    const bucket = this._listeners[event];
    if (bucket && bucket.length > 0) {
      const snapshot = bucket.slice();
      for (const entry of snapshot) {
        if (entry.once) this.off(event, entry.fn);
        called = true;
        this._safeCall(() => entry.fn(data), event);
      }
    }

    // Wildcard listeners
    if (this._wildcards.length > 0) {
      const snapshot = this._wildcards.slice();
      for (const entry of snapshot) {
        if (entry.once) {
          const idx = this._wildcards.indexOf(entry);
          if (idx !== -1) this._wildcards.splice(idx, 1);
        }
        called = true;
        this._safeCall(() => entry.fn(event, data), event);
      }
    }

    return called;
  }

  /**
   * Return the number of listeners registered for `event`,
   * not counting wildcard listeners.
   */
  listenerCount<K extends keyof EventMap>(event: K): number {
    return this._listeners[event]?.length ?? 0;
  }

  // ── Wildcard API ─────────────────────────────────────────────────────────

  /**
   * Register a persistent wildcard listener that receives every event.
   * Returns `this` for chaining.
   */
  onAny(fn: WildcardListener<EventMap>): this {
    this._wildcards.push({ fn, once: false });
    return this;
  }

  /**
   * Register a one-shot wildcard listener.
   * Returns `this` for chaining.
   */
  onceAny(fn: WildcardListener<EventMap>): this {
    this._wildcards.push({ fn, once: true });
    return this;
  }

  /**
   * Remove a previously registered wildcard listener.
   * If `fn` is omitted, all wildcards are removed.
   * Returns `this` for chaining.
   */
  offAny(fn?: WildcardListener<EventMap>): this {
    if (fn === undefined) {
      this._wildcards.length = 0;
    } else {
      const idx = this._wildcards.findIndex((e) => e.fn === fn);
      if (idx !== -1) this._wildcards.splice(idx, 1);
    }
    return this;
  }

  /** Total number of wildcard listeners currently registered. */
  wildcardCount(): number {
    return this._wildcards.length;
  }

  // ── Internals ────────────────────────────────────────────────────────────

  private _addListener<K extends keyof EventMap>(
    event: K,
    fn: EventListener<EventMap[K]>,
    once: boolean
  ): void {
    if (!this._listeners[event]) {
      (this._listeners as Record<keyof EventMap, Array<ListenerEntry<unknown>>>)[
        event
      ] = [];
    }
    (this._listeners[event] as Array<ListenerEntry<EventMap[K]>>).push({
      fn,
      once,
    });
  }

  /**
   * Invoke `fn`, catching any thrown value.
   * Attempts to re-emit it as an `"error"` event; falls back to
   * `console.error` so other listeners in the same `emit` call are
   * not skipped.
   */
  private _safeCall(fn: () => void, sourceEvent: keyof EventMap): void {
    try {
      fn();
    } catch (err) {
      // Avoid infinite recursion: don't re-emit if we're already in an error
      if (sourceEvent === "error") {
        console.error("[TypedEmitter] error listener threw:", err);
        return;
      }
      // Try to deliver to any registered "error" listeners
      const errorBucket = this._listeners["error" as keyof EventMap];
      if (errorBucket && errorBucket.length > 0) {
        this.emit("error" as keyof EventMap, err as EventMap[keyof EventMap]);
      } else {
        console.error(
          `[TypedEmitter] unhandled error from "${String(sourceEvent)}" listener:`,
          err
        );
      }
    }
  }
}

// ── Self-test (__main__) ─────────────────────────────────────────────────────

if (
  typeof process !== "undefined" &&
  process.argv[1] &&
  (process.argv[1].endsWith("emitter.ts") ||
    process.argv[1].endsWith("emitter.js"))
) {
  interface TestEvents {
    greet: { name: string };
    count: number;
    done: void;
    error: unknown;
  }

  let passed = 0;
  let failed = 0;

  function assert(label: string, condition: boolean): void {
    if (condition) {
      console.log(`  ✓ ${label}`);
      passed++;
    } else {
      console.error(`  ✗ ${label}`);
      failed++;
    }
  }

  console.log("TypedEmitter self-test\n");

  // ── on / emit / off ──────────────────────────────────────────────────────
  {
    console.log("on / emit / off");
    const e = new TypedEmitter<TestEvents>();
    const received: string[] = [];
    const listener = (d: { name: string }) => received.push(d.name);

    e.on("greet", listener);
    e.emit("greet", { name: "Alice" });
    e.emit("greet", { name: "Bob" });
    assert("receives both events", received.join(",") === "Alice,Bob");

    e.off("greet", listener);
    e.emit("greet", { name: "Carol" });
    assert("off removes listener", received.length === 2);

    assert("listenerCount after off is 0", e.listenerCount("greet") === 0);
  }

  // ── once ─────────────────────────────────────────────────────────────────
  {
    console.log("\nonce");
    const e = new TypedEmitter<TestEvents>();
    let calls = 0;

    e.once("count", () => calls++);
    e.emit("count", 1);
    e.emit("count", 2);
    assert("once fires exactly once", calls === 1);
    assert("listenerCount after once fires is 0", e.listenerCount("count") === 0);
  }

  // ── multiple listeners, error isolation ──────────────────────────────────
  {
    console.log("\nerror isolation");
    const e = new TypedEmitter<TestEvents>();
    const log: string[] = [];

    e.on("count", () => { log.push("before"); throw new Error("boom"); });
    e.on("count", () => { log.push("after"); });

    // Suppress console.error noise from the unhandled error path
    const original = console.error;
    console.error = () => {};
    e.emit("count", 42);
    console.error = original;

    assert("listener after throwing one still runs", log.includes("after"));
    assert("listener before throwing one ran", log.includes("before"));
  }

  // ── wildcard ─────────────────────────────────────────────────────────────
  {
    console.log("\nwildcard onAny / offAny");
    const e = new TypedEmitter<TestEvents>();
    const log: Array<[string, unknown]> = [];
    const wc = (ev: keyof TestEvents, data: unknown) =>
      log.push([String(ev), data]);

    e.onAny(wc);
    e.emit("greet", { name: "X" });
    e.emit("count", 7);
    assert("wildcard sees greet", log[0]?.[0] === "greet");
    assert("wildcard sees count", log[1]?.[0] === "count");
    assert("wildcardCount is 1", e.wildcardCount() === 1);

    e.offAny(wc);
    e.emit("count", 99);
    assert("wildcard removed", log.length === 2);
  }

  // ── onceAny ───────────────────────────────────────────────────────────────
  {
    console.log("\nonceAny");
    const e = new TypedEmitter<TestEvents>();
    let calls = 0;
    e.onceAny(() => calls++);
    e.emit("count", 1);
    e.emit("count", 2);
    assert("onceAny fires exactly once", calls === 1);
    assert("wildcardCount after onceAny fires is 0", e.wildcardCount() === 0);
  }

  // ── off with no fn clears all listeners for that event ───────────────────
  {
    console.log("\noff(event) clears all");
    const e = new TypedEmitter<TestEvents>();
    e.on("count", () => {});
    e.on("count", () => {});
    assert("two listeners registered", e.listenerCount("count") === 2);
    e.off("count");
    assert("all cleared after off(event)", e.listenerCount("count") === 0);
  }

  // ── emit returns false when no listeners ─────────────────────────────────
  {
    console.log("\nemit return value");
    const e = new TypedEmitter<TestEvents>();
    assert("emit with no listeners returns false", e.emit("done", undefined) === false);
    e.on("done", () => {});
    assert("emit with listener returns true", e.emit("done", undefined) === true);
  }

  console.log(`\n${passed} passed, ${failed} failed`);
  process.exitCode = failed > 0 ? 1 : 0;
}
