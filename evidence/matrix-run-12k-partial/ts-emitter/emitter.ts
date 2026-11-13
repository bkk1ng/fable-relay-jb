// emitter.ts — typed event emitter, TypeScript strict, zero runtime deps.
//
// Usage:
//   type MyEvents = { data: [Buffer]; error: [Error]; close: [] };
//   const ee = new EventEmitter<MyEvents>();
//   ee.on("data", (buf) => { … });          // typed args
//   ee.on("*", (name, …args) => { … });     // wildcard
//
// Compile:  tsc --strict emitter.ts
// Run test: node emitter.js  (or ts-node emitter.ts)

// ── Minimal ambient decls so the file compiles without @types/node ──────────
declare const module: { exports: Record<string, unknown> };
declare const require: { main: typeof module } | undefined;

// ── Core types ───────────────────────────────────────────────────────────────

/** Each key in the event map must resolve to a readonly tuple of arg types. */
type AnyArgs = readonly unknown[];

/** String keys of the event map. */
type EventKey<Map extends Record<string, AnyArgs>> = keyof Map & string;

/** A strongly-typed listener for a specific event. */
type SpecificListener<Args extends AnyArgs> = (...args: Args) => void;

/**
 * A wildcard listener fires on every emission.
 * Receives (eventName, ...originalArgs).
 */
type WildcardListener<Map extends Record<string, AnyArgs>> = (
  event: EventKey<Map>,
  ...args: unknown[]
) => void;

/** Internal storage record — erased to unknown[] after type checking. */
interface Entry {
  fn: (...args: unknown[]) => void;
  once: boolean;
}

// ── EventEmitter ─────────────────────────────────────────────────────────────

export class EventEmitter<Map extends Record<string, AnyArgs>> {
  private readonly _map = new Map<string, Entry[]>();
  private _wildcards: Array<{ fn: WildcardListener<Map>; once: boolean }> = [];

  // ── on ───────────────────────────────────────────────────────────────────

  on<K extends EventKey<Map>>(event: K, listener: SpecificListener<Map[K]>): this;
  on(event: "*", listener: WildcardListener<Map>): this;
  on(event: string, listener: (...args: unknown[]) => void): this {
    if (event === "*") {
      this._wildcards = [
        ...this._wildcards,
        { fn: listener as WildcardListener<Map>, once: false },
      ];
    } else {
      this._append(event, listener, false);
    }
    return this;
  }

  // ── once ─────────────────────────────────────────────────────────────────

  once<K extends EventKey<Map>>(event: K, listener: SpecificListener<Map[K]>): this;
  once(event: "*", listener: WildcardListener<Map>): this;
  once(event: string, listener: (...args: unknown[]) => void): this {
    if (event === "*") {
      this._wildcards = [
        ...this._wildcards,
        { fn: listener as WildcardListener<Map>, once: true },
      ];
    } else {
      this._append(event, listener, true);
    }
    return this;
  }

  // ── off ──────────────────────────────────────────────────────────────────

  off<K extends EventKey<Map>>(event: K, listener: SpecificListener<Map[K]>): this;
  off(event: "*", listener: WildcardListener<Map>): this;
  off(event: string, listener: (...args: unknown[]) => void): this {
    if (event === "*") {
      this._wildcards = this._wildcards.filter((e) => e.fn !== listener);
    } else {
      const list = this._map.get(event);
      if (list !== undefined) {
        const next = list.filter((e) => e.fn !== listener);
        next.length === 0 ? this._map.delete(event) : this._map.set(event, next);
      }
    }
    return this;
  }

  // ── emit ─────────────────────────────────────────────────────────────────

  emit<K extends EventKey<Map>>(event: K, ...args: Map[K]): boolean {
    const rawArgs = args as unknown[];
    const list = this._map.get(event as string);
    let had = false;

    if (list !== undefined && list.length > 0) {
      had = true;
      // Snapshot so once-removals inside the loop don't skip entries.
      for (const entry of list.slice()) {
        this._callSafe(entry.fn, rawArgs, event as string);
        if (entry.once) {
          this._drop(event as string, entry.fn);
        }
      }
    }

    if (this._wildcards.length > 0) {
      had = true;
      for (const entry of this._wildcards.slice()) {
        this._callSafe(
          entry.fn as (...a: unknown[]) => void,
          [event, ...rawArgs],
          event as string,
        );
        if (entry.once) {
          this._wildcards = this._wildcards.filter((e) => e.fn !== entry.fn);
        }
      }
    }

    return had;
  }

  // ── listenerCount ────────────────────────────────────────────────────────

  listenerCount<K extends EventKey<Map>>(event: K): number;
  listenerCount(event: "*"): number;
  listenerCount(event: string): number {
    if (event === "*") return this._wildcards.length;
    return this._map.get(event)?.length ?? 0;
  }

  // ── private ──────────────────────────────────────────────────────────────

  private _append(
    event: string,
    fn: (...args: unknown[]) => void,
    once: boolean,
  ): void {
    const existing = this._map.get(event) ?? [];
    this._map.set(event, [...existing, { fn, once }]);
  }

  private _drop(event: string, fn: (...args: unknown[]) => void): void {
    const list = this._map.get(event);
    if (list === undefined) return;
    const next = list.filter((e) => e.fn !== fn);
    next.length === 0 ? this._map.delete(event) : this._map.set(event, next);
  }

  /**
   * Error isolation: a throwing listener is caught and reported, but every
   * remaining listener in the same emit call still runs.
   */
  private _callSafe(
    fn: (...args: unknown[]) => void,
    args: unknown[],
    event: string,
  ): void {
    try {
      fn(...args);
    } catch (err) {
      // Surface the error asynchronously so it doesn't swallow silently,
      // but doesn't interrupt sibling listeners.
      Promise.resolve().then(() => {
        console.error(`[EventEmitter] uncaught error in "${event}" listener:`, err);
      });
    }
  }
}

// ── __main__ ─────────────────────────────────────────────────────────────────

const isMain =
  typeof require !== "undefined" && require !== null && require.main === module;

if (isMain) {
  // Simple assertion helper
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

  // ── Define an event map for the tests ──────────────────────────────────
  type TestMap = {
    ping: [string];
    tick: [number, number];
    close: [];
  };

  const ee = new EventEmitter<TestMap>();

  console.log("on / emit / off");
  {
    const got: string[] = [];
    const handler = (s: string) => got.push(s);

    ee.on("ping", handler);
    ee.emit("ping", "a");
    ee.emit("ping", "b");
    ee.off("ping", handler);
    ee.emit("ping", "c"); // should not reach got

    assert("on fires twice", got.length === 2);
    assert("off removes handler", !got.includes("c"));
    assert('listenerCount after off is 0', ee.listenerCount("ping") === 0);
  }

  console.log("\nonce");
  {
    let count = 0;
    ee.once("ping", () => { count++; });

    assert("listenerCount before first emit is 1", ee.listenerCount("ping") === 1);
    ee.emit("ping", "x");
    ee.emit("ping", "y");
    assert("once fires exactly once", count === 1);
    assert("listenerCount after firing is 0", ee.listenerCount("ping") === 0);
  }

  console.log("\nmultiple listeners — ordering and isolation");
  {
    const log: number[] = [];

    ee.on("tick", (_a, _b) => { log.push(1); });
    ee.on("tick", () => { throw new Error("boom"); }); // must not stop listener 3
    ee.on("tick", (_a, _b) => { log.push(3); });

    ee.emit("tick", 0, 1);

    assert("listener 1 ran", log.includes(1));
    assert("listener 3 ran despite listener 2 throwing", log.includes(3));
    assert("exactly 2 successful listeners ran", log.length === 2);

    // clean up
    ee.off("tick", () => {}); // noop — unknown ref, that's fine
    // reset by creating a fresh emitter for remaining tests
  }

  console.log("\nwildcard");
  {
    const ee2 = new EventEmitter<TestMap>();
    const seen: Array<[string, unknown[]]> = [];

    ee2.on("*", (name, ...args) => {
      seen.push([name, args]);
    });

    ee2.emit("ping", "hello");
    ee2.emit("close");
    ee2.emit("tick", 7, 8);

    assert("wildcard saw 3 events", seen.length === 3);
    assert('wildcard first event name is "ping"', seen[0]![0] === "ping");
    assert("wildcard first event args correct", seen[0]![1][0] === "hello");
    assert('wildcard third event name is "tick"', seen[2]![0] === "tick");
    assert("wildcard listenerCount is 1", ee2.listenerCount("*") === 1);
  }

  console.log("\nwildcard once");
  {
    const ee3 = new EventEmitter<TestMap>();
    let wCount = 0;
    ee3.once("*", () => { wCount++; });

    ee3.emit("ping", "first");
    ee3.emit("ping", "second");

    assert("wildcard once fires exactly once", wCount === 1);
    assert("wildcard listenerCount is 0 after firing", ee3.listenerCount("*") === 0);
  }

  console.log("\nemit returns correct boolean");
  {
    const ee4 = new EventEmitter<TestMap>();
    assert("emit with no listeners returns false", ee4.emit("close") === false);
    ee4.on("close", () => {});
    assert("emit with a listener returns true", ee4.emit("close") === true);
  }

  console.log(
    `\n${passed + failed} tests: ${passed} passed, ${failed} failed`,
  );

  if (failed > 0) process.exit(1);
}
