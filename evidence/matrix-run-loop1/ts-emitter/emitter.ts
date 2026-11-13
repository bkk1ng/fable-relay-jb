// Typed event emitter with wildcard support and isolated listener errors.
// No runtime dependencies — TypeScript strict mode compatible.

/** Signature for a wildcard listener that receives the event name and its payload. */
export type WildcardListener<EventMap extends Record<string, unknown[]>> = <
  K extends keyof EventMap & string
>(
  event: K,
  ...args: EventMap[K]
) => void;

type AnyListener = (...args: unknown[]) => void;

export class TypedEmitter<EventMap extends Record<string, unknown[]>> {
  private readonly _listeners = new Map<string, Set<AnyListener>>();
  private readonly _onceListeners = new Map<string, Set<AnyListener>>();
  private readonly _wildcards = new Set<AnyListener>();
  private readonly _onceWildcards = new Set<AnyListener>();

  // ── on ────────────────────────────────────────────────────────────────────

  on<K extends keyof EventMap & string>(
    event: K,
    listener: (...args: EventMap[K]) => void
  ): this;
  on(event: "*", listener: WildcardListener<EventMap>): this;
  on(event: string, listener: AnyListener): this {
    if (event === "*") {
      this._wildcards.add(listener);
    } else {
      if (!this._listeners.has(event)) this._listeners.set(event, new Set());
      this._listeners.get(event)!.add(listener);
    }
    return this;
  }

  // ── once ──────────────────────────────────────────────────────────────────

  once<K extends keyof EventMap & string>(
    event: K,
    listener: (...args: EventMap[K]) => void
  ): this;
  once(event: "*", listener: WildcardListener<EventMap>): this;
  once(event: string, listener: AnyListener): this {
    if (event === "*") {
      this._onceWildcards.add(listener);
    } else {
      if (!this._onceListeners.has(event))
        this._onceListeners.set(event, new Set());
      this._onceListeners.get(event)!.add(listener);
    }
    return this;
  }

  // ── off ───────────────────────────────────────────────────────────────────

  off<K extends keyof EventMap & string>(
    event: K,
    listener: (...args: EventMap[K]) => void
  ): this;
  off(event: "*", listener: WildcardListener<EventMap>): this;
  off(event: string, listener: AnyListener): this {
    if (event === "*") {
      this._wildcards.delete(listener);
      this._onceWildcards.delete(listener);
    } else {
      this._listeners.get(event)?.delete(listener);
      this._onceListeners.get(event)?.delete(listener);
    }
    return this;
  }

  // ── emit ──────────────────────────────────────────────────────────────────

  emit<K extends keyof EventMap & string>(event: K, ...args: EventMap[K]): boolean {
    const errors: unknown[] = [];

    // Collect all listeners to invoke (snapshot before mutation).
    const persistent = [...(this._listeners.get(event) ?? [])];
    const once = [...(this._onceListeners.get(event) ?? [])];
    const wildcardPersistent = [...this._wildcards];
    const wildcardOnce = [...this._onceWildcards];

    // Remove one-time listeners before invoking (avoid re-entry issues).
    this._onceListeners.get(event)?.clear();
    this._onceWildcards.clear();

    for (const fn of persistent) {
      try {
        fn(...args);
      } catch (err) {
        errors.push(err);
      }
    }

    for (const fn of once) {
      try {
        fn(...args);
      } catch (err) {
        errors.push(err);
      }
    }

    // Wildcards receive the event name as the first argument.
    for (const fn of wildcardPersistent) {
      try {
        fn(event, ...args);
      } catch (err) {
        errors.push(err);
      }
    }

    for (const fn of wildcardOnce) {
      try {
        fn(event, ...args);
      } catch (err) {
        errors.push(err);
      }
    }

    // Surface collected errors after all listeners have run.
    if (errors.length === 1) throw errors[0];
    if (errors.length > 1) {
      throw new AggregateError(errors, `${errors.length} listener(s) threw during "${event}"`);
    }

    return (
      persistent.length + once.length + wildcardPersistent.length + wildcardOnce.length > 0
    );
  }

  // ── listenerCount ─────────────────────────────────────────────────────────

  listenerCount<K extends keyof EventMap & string>(event: K): number;
  listenerCount(event: "*"): number;
  listenerCount(event: string): number {
    if (event === "*") {
      return this._wildcards.size + this._onceWildcards.size;
    }
    return (
      (this._listeners.get(event)?.size ?? 0) +
      (this._onceListeners.get(event)?.size ?? 0)
    );
  }
}

// ── Self-test (__main__) ───────────────────────────────────────────────────

if (import.meta.url === `file://${process.argv[1]}`) {
  type TestEvents = {
    greet: [name: string];
    count: [value: number];
    done: [];
  };

  const emitter = new TypedEmitter<TestEvents>();
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

  // 1. Basic on/emit
  {
    const received: string[] = [];
    emitter.on("greet", (name) => received.push(name));
    emitter.emit("greet", "Alice");
    emitter.emit("greet", "Bob");
    assert("on/emit fires listener for each emit", received.join(",") === "Alice,Bob");
  }

  // 2. once fires exactly once
  {
    let calls = 0;
    emitter.once("count", () => calls++);
    emitter.emit("count", 1);
    emitter.emit("count", 2);
    assert("once fires exactly once", calls === 1);
  }

  // 3. off removes listener
  {
    let calls = 0;
    const fn = () => calls++;
    emitter.on("done", fn);
    emitter.emit("done");
    emitter.off("done", fn);
    emitter.emit("done");
    assert("off removes listener", calls === 1);
  }

  // 4. listenerCount
  {
    const e2 = new TypedEmitter<TestEvents>();
    const a = () => {};
    const b = () => {};
    e2.on("greet", a);
    e2.once("greet", b);
    assert("listenerCount counts on + once", e2.listenerCount("greet") === 2);
    e2.off("greet", a);
    assert("listenerCount after off", e2.listenerCount("greet") === 1);
  }

  // 5. Wildcard listener
  {
    const e3 = new TypedEmitter<TestEvents>();
    const seen: Array<[string, unknown]> = [];
    e3.on("*", (evt, ...args) => seen.push([evt, args[0]]));
    e3.emit("greet", "Carol");
    e3.emit("count", 42);
    assert(
      "wildcard receives all events",
      seen[0][0] === "greet" && seen[0][1] === "Carol" &&
      seen[1][0] === "count" && seen[1][1] === 42
    );
  }

  // 6. Wildcard once
  {
    const e4 = new TypedEmitter<TestEvents>();
    let wcCalls = 0;
    e4.once("*", () => wcCalls++);
    e4.emit("greet", "Dave");
    e4.emit("greet", "Eve");
    assert("wildcard once fires exactly once", wcCalls === 1);
  }

  // 7. Listener errors are isolated — others still run
  {
    const e5 = new TypedEmitter<TestEvents>();
    let secondRan = false;
    e5.on("greet", () => { throw new Error("boom"); });
    e5.on("greet", () => { secondRan = true; });
    let threw = false;
    try {
      e5.emit("greet", "test");
    } catch {
      threw = true;
    }
    assert("error in one listener does not block others", secondRan);
    assert("errors are re-thrown after all listeners run", threw);
  }

  // 8. Multiple errors produce AggregateError
  {
    const e6 = new TypedEmitter<TestEvents>();
    e6.on("greet", () => { throw new Error("first"); });
    e6.on("greet", () => { throw new Error("second"); });
    let isAggregate = false;
    try {
      e6.emit("greet", "multi");
    } catch (err) {
      isAggregate = err instanceof AggregateError && (err as AggregateError).errors.length === 2;
    }
    assert("multiple errors produce AggregateError", isAggregate);
  }

  // 9. emit returns false when no listeners
  {
    const e7 = new TypedEmitter<TestEvents>();
    assert("emit returns false with no listeners", e7.emit("done") === false);
    e7.on("done", () => {});
    assert("emit returns true with a listener", e7.emit("done") === true);
  }

  console.log(`\n${passed} passed, ${failed} failed`);
  if (failed > 0) process.exit(1);
}
