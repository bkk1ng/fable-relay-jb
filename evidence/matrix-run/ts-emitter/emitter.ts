/**
 * emitter.ts — typed event emitter
 *
 * TypeScript strict, zero runtime dependencies.
 *
 * Compile:  tsc --strict emitter.ts
 * Run test: ts-node emitter.ts   (or node emitter.js after compile)
 *
 * Example:
 *   type Events = { data: [payload: string]; done: [] };
 *   const ee = new Emitter<Events>();
 *   ee.on("data", (s) => console.log(s));
 *   ee.emit("data", "hello");
 */

// ── Types ────────────────────────────────────────────────────────────────────

/** Each key maps to a tuple of argument types for that event. */
export type EventMap = Record<string, unknown[]>;

/** Sentinel key: listeners registered here fire for every emitted event. */
export const WILDCARD = "*" as const;
export type Wildcard = typeof WILDCARD;

/** Listener for a specific named event. */
export type Listener<Args extends unknown[]> = (...args: Args) => void;

/**
 * Wildcard listener.
 * Receives the event name as the first argument, then the event's own arguments.
 */
export type WildcardListener = (event: string, ...args: unknown[]) => void;

// Internal alias — the type stored in the untyped collections.
type AnyListener = (...args: unknown[]) => void;

// ── Error isolation helper ────────────────────────────────────────────────────

/**
 * Re-throws `err` after the current call stack has unwound so that a single
 * listener failure cannot prevent subsequent listeners from running.
 */
function deferThrow(err: unknown): void {
  Promise.resolve().then(() => {
    throw err;
  });
}

// ── Emitter ──────────────────────────────────────────────────────────────────

/**
 * A fully typed event emitter.
 *
 * @template Events  Object whose keys are event names and whose values are
 *   tuples describing argument types, e.g. `{ ping: [id: number] }`.
 */
export class Emitter<Events extends EventMap> {
  /** Named event → ordered listener list. */
  private readonly _named = new Map<string, AnyListener[]>();

  /** Named event → set of listeners registered with `once`. */
  private readonly _once = new Map<string, Set<AnyListener>>();

  /** Wildcard listeners, in registration order. */
  private readonly _wild: WildcardListener[] = [];

  /** Subset of `_wild` registered with `once`. */
  private readonly _onceWild = new Set<WildcardListener>();

  // ── on ────────────────────────────────────────────────────────────────────

  /** Register a persistent listener for a named event. */
  on<K extends keyof Events & string>(
    event: K,
    listener: Listener<Events[K]>,
  ): this;
  /** Register a persistent wildcard listener (fires for every event). */
  on(event: Wildcard, listener: WildcardListener): this;
  on(event: string, listener: AnyListener): this {
    if (event === WILDCARD) {
      this._wild.push(listener as WildcardListener);
    } else {
      let list = this._named.get(event);
      if (!list) {
        list = [];
        this._named.set(event, list);
      }
      list.push(listener);
    }
    return this;
  }

  // ── once ──────────────────────────────────────────────────────────────────

  /** Register a one-shot listener for a named event. */
  once<K extends keyof Events & string>(
    event: K,
    listener: Listener<Events[K]>,
  ): this;
  /** Register a one-shot wildcard listener. */
  once(event: Wildcard, listener: WildcardListener): this;
  once(event: string, listener: AnyListener): this {
    if (event === WILDCARD) {
      this._wild.push(listener as WildcardListener);
      this._onceWild.add(listener as WildcardListener);
    } else {
      let list = this._named.get(event);
      if (!list) {
        list = [];
        this._named.set(event, list);
      }
      list.push(listener);

      let set = this._once.get(event);
      if (!set) {
        set = new Set();
        this._once.set(event, set);
      }
      set.add(listener);
    }
    return this;
  }

  // ── off ───────────────────────────────────────────────────────────────────

  /** Remove a specific listener from a named event. */
  off<K extends keyof Events & string>(
    event: K,
    listener: Listener<Events[K]>,
  ): this;
  /** Remove a specific wildcard listener. */
  off(event: Wildcard, listener: WildcardListener): this;
  off(event: string, listener: AnyListener): this {
    if (event === WILDCARD) {
      const idx = this._wild.indexOf(listener as WildcardListener);
      if (idx !== -1) this._wild.splice(idx, 1);
      this._onceWild.delete(listener as WildcardListener);
    } else {
      const list = this._named.get(event);
      if (list) {
        const idx = list.indexOf(listener);
        if (idx !== -1) list.splice(idx, 1);
      }
      this._once.get(event)?.delete(listener);
    }
    return this;
  }

  // ── emit ──────────────────────────────────────────────────────────────────

  /**
   * Emit an event, invoking all matching listeners.
   *
   * Listener errors are caught and re-thrown asynchronously so that every
   * listener in the batch is called regardless of whether a previous one threw.
   *
   * @returns `true` if at least one listener was invoked.
   */
  emit<K extends keyof Events & string>(event: K, ...args: Events[K]): boolean {
    const rawArgs = args as unknown[];

    // Snapshots prevent mutation-during-iteration bugs.
    const named = this._named.get(event)?.slice() ?? [];
    const once  = this._once.get(event);
    const wilds = this._wild.slice();

    // Named listeners.
    for (const fn of named) {
      try {
        fn(...rawArgs);
      } catch (err) {
        deferThrow(err);
      }
      // Remove one-shot listeners after their first invocation.
      if (once?.has(fn)) {
        once.delete(fn);
        this._spliceNamed(event, fn);
      }
    }

    // Wildcard listeners.
    for (const fn of wilds) {
      try {
        fn(event, ...rawArgs);
      } catch (err) {
        deferThrow(err);
      }
      if (this._onceWild.has(fn)) {
        this._onceWild.delete(fn);
        const idx = this._wild.indexOf(fn);
        if (idx !== -1) this._wild.splice(idx, 1);
      }
    }

    return named.length > 0 || wilds.length > 0;
  }

  // ── listenerCount ─────────────────────────────────────────────────────────

  /** Returns the number of listeners registered for the given event (or wildcard). */
  listenerCount(event: (keyof Events & string) | Wildcard): number {
    if ((event as string) === WILDCARD) return this._wild.length;
    return this._named.get(event as string)?.length ?? 0;
  }

  // ── private helpers ───────────────────────────────────────────────────────

  private _spliceNamed(event: string, fn: AnyListener): void {
    const list = this._named.get(event);
    if (!list) return;
    const idx = list.indexOf(fn);
    if (idx !== -1) list.splice(idx, 1);
  }
}

// ── Self-test ─────────────────────────────────────────────────────────────────
// Run directly:  ts-node emitter.ts
// Requires @types/node for the `require` / `module` globals.

if (typeof require !== "undefined" && typeof module !== "undefined" && require.main === module) {
  type TestEvents = {
    greet: [name: string];
    ping:  [id: number, ts: number];
    done:  [];
  };

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

  const ee = new Emitter<TestEvents>();

  // ── basic on / emit / off ───────────────────────────────────────────────

  console.log("\non / emit / off");

  const received: string[] = [];
  const greetFn: Listener<[string]> = (name) => { received.push(name); };

  ee.on("greet", greetFn);
  ee.emit("greet", "alice");
  ee.emit("greet", "bob");
  assert("two emits → two receives",        received.length === 2);
  assert("first payload",                   received[0] === "alice");
  assert("second payload",                  received[1] === "bob");
  assert("listenerCount after on",          ee.listenerCount("greet") === 1);

  ee.off("greet", greetFn);
  ee.emit("greet", "carol");
  assert("no call after off",               received.length === 2);
  assert("listenerCount after off",         ee.listenerCount("greet") === 0);

  // ── once ───────────────────────────────────────────────────────────────

  console.log("\nonce");

  let onceFired = 0;
  ee.once("done", () => { onceFired++; });
  assert("listenerCount before first emit", ee.listenerCount("done") === 1);
  ee.emit("done");
  assert("fired once",                      onceFired === 1);
  assert("listenerCount after first emit",  ee.listenerCount("done") === 0);
  ee.emit("done");
  assert("did not fire a second time",      onceFired === 1);

  // ── multiple listeners ─────────────────────────────────────────────────

  console.log("\nmultiple listeners");

  const order: number[] = [];
  ee.on("ping", () => { order.push(1); });
  ee.on("ping", () => { order.push(2); });
  ee.on("ping", () => { order.push(3); });
  ee.emit("ping", 0, 0);
  assert("all three called",                order.length === 3);
  assert("called in registration order",    order.join(",") === "1,2,3");
  assert("listenerCount = 3",               ee.listenerCount("ping") === 3);

  // ── wildcard ───────────────────────────────────────────────────────────

  console.log("\nwildcard");

  const wildEvents: string[] = [];
  const wildFn: WildcardListener = (ev) => { wildEvents.push(ev); };
  ee.on(WILDCARD, wildFn);

  ee.emit("greet", "dave");
  ee.emit("done");
  assert("wildcard fired for greet",        wildEvents.includes("greet"));
  assert("wildcard fired for done",         wildEvents.includes("done"));
  assert("listenerCount wildcard = 1",      ee.listenerCount(WILDCARD) === 1);

  ee.off(WILDCARD, wildFn);
  const countBefore = wildEvents.length;
  ee.emit("greet", "eve");
  assert("wildcard silent after off",       wildEvents.length === countBefore);

  // ── wildcard once ──────────────────────────────────────────────────────

  console.log("\nwildcard once");

  let wildOnceFired = 0;
  ee.once(WILDCARD, () => { wildOnceFired++; });
  ee.emit("done");
  assert("wildcard once fires once",        wildOnceFired === 1);
  ee.emit("done");
  assert("wildcard once silent after",      wildOnceFired === 1);

  // ── error isolation ────────────────────────────────────────────────────

  console.log("\nerror isolation");

  const afterThrow: boolean[] = [];
  ee.on("done", () => { throw new Error("boom"); });
  ee.on("done", () => { afterThrow.push(true); });
  ee.emit("done"); // the Error is re-thrown async; next listener must still run
  assert("listener after thrower still ran", afterThrow.length === 1);

  // ── emit return value ──────────────────────────────────────────────────

  console.log("\nemit return value");

  const fresh = new Emitter<TestEvents>();
  assert("no listeners → false",   fresh.emit("greet", "x") === false);
  fresh.on("greet", () => { /* */ });
  assert("one listener → true",    fresh.emit("greet", "x") === true);

  // ── summary ────────────────────────────────────────────────────────────

  console.log(`\n${passed} passed, ${failed} failed\n`);
  if (failed > 0) process.exit(1);
}
