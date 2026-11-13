/**
 * json.ts — zero-dependency strict-TypeScript JSON serializer and parser.
 *
 * stringify: cycle detection, function/array replacer, indentation, big-number-as-string
 * parse:     position-tracked errors (line/col/offset), big-number-as-string
 *
 * Self-tests run automatically when this module is executed directly:
 *   npx ts-node json.ts
 */

"use strict";

// ─── Public types ─────────────────────────────────────────────────────────────

export type JsonPrimitive = null | boolean | number | string;
export type JsonValue =
  | JsonPrimitive
  | JsonValue[]
  | { [key: string]: JsonValue };

export type ReplacerFn = (key: string, value: unknown) => unknown;
export type Replacer = ReplacerFn | string[] | null;

export interface StringifyOptions {
  /**
   * Indent amount.  A number inserts that many spaces; a string is used
   * verbatim.  Undefined / 0 / "" produces compact output.
   */
  space?: number | string;
  /**
   * Standard JSON replacer: an array of allowed keys, or a transform function
   * called for every key/value pair (parent key is "" for the root value).
   */
  replacer?: Replacer;
  /**
   * When true, integers outside ±Number.MAX_SAFE_INTEGER are written as
   * JSON strings instead of JSON numbers so they survive a round-trip without
   * precision loss.
   */
  bigNumberAsString?: boolean;
}

export interface ParseOptions {
  /**
   * When true, number tokens that represent integers outside
   * ±Number.MAX_SAFE_INTEGER are returned as strings rather than numbers.
   */
  bigNumberAsString?: boolean;
}

// ─── Errors ───────────────────────────────────────────────────────────────────

/** Thrown by parse() with precise source location. */
export class JsonParseError extends SyntaxError {
  /** Zero-based byte offset inside the source string. */
  readonly offset: number;
  /** One-based line number. */
  readonly line: number;
  /** One-based column number. */
  readonly column: number;

  constructor(
    message: string,
    offset: number,
    line: number,
    column: number,
  ) {
    super(`${message} (line ${line}, column ${column}, offset ${offset})`);
    this.name = "JsonParseError";
    this.offset = offset;
    this.line = line;
    this.column = column;
  }
}

// ─── Stringify ────────────────────────────────────────────────────────────────

function resolveIndent(space: number | string | undefined): string {
  if (space === undefined || space === null) return "";
  if (typeof space === "number") {
    const n = Math.min(10, Math.max(0, Math.floor(space)));
    return " ".repeat(n);
  }
  // String: use as-is, capped at 10 chars (spec behaviour)
  return String(space).slice(0, 10);
}

/** Escape a string into a valid JSON string literal (including the quotes). */
function escapeString(s: string): string {
  let out = '"';
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c === 0x22) { out += '\\"'; continue; }
    if (c === 0x5c) { out += "\\\\"; continue; }
    if (c === 0x08) { out += "\\b"; continue; }
    if (c === 0x0c) { out += "\\f"; continue; }
    if (c === 0x0a) { out += "\\n"; continue; }
    if (c === 0x0d) { out += "\\r"; continue; }
    if (c === 0x09) { out += "\\t"; continue; }
    if (c < 0x20) {
      out += "\\u" + c.toString(16).padStart(4, "0");
      continue;
    }
    out += s[i];
  }
  return out + '"';
}

/**
 * Serialize `value` to a JSON string.
 *
 * Returns `undefined` (not the string `"undefined"`) when the top-level value
 * is a function, symbol, or `undefined`, matching native JSON.stringify.
 */
export function stringify(
  value: unknown,
  options: StringifyOptions = {},
): string | undefined {
  const indent = resolveIndent(options.space);
  const bigNum = options.bigNumberAsString ?? false;

  const replacer = options.replacer ?? null;
  const keyAllowSet: Set<string> | null = Array.isArray(replacer)
    ? new Set(replacer.map(String))
    : null;
  const replacerFn: ReplacerFn | null =
    typeof replacer === "function" ? replacer : null;

  // Tracks objects currently on the serialization stack for cycle detection.
  const inStack = new Set<object>();

  function ser(
    key: string,
    val: unknown,
    depth: number,
  ): string | undefined {
    // 1. Apply replacer function.
    if (replacerFn !== null) {
      val = replacerFn(key, val);
    }

    // 2. Unwrap objects that have a toJSON method (e.g. Date).
    if (
      val !== null &&
      typeof val === "object" &&
      typeof (val as Record<string, unknown>)["toJSON"] === "function"
    ) {
      val = (
        (val as Record<string, unknown>)["toJSON"] as (k: string) => unknown
      ).call(val, key);
    }

    // 3. Primitives.
    if (val === null) return "null";
    if (val === undefined) return undefined;
    if (typeof val === "boolean") return val ? "true" : "false";
    if (typeof val === "symbol" || typeof val === "function") return undefined;
    if (typeof val === "bigint") {
      throw new TypeError(
        "Do not know how to serialize a BigInt (use replacer to convert first)",
      );
    }

    if (typeof val === "number") {
      if (Number.isNaN(val) || !Number.isFinite(val)) {
        // Native JSON.stringify outputs "null" for these; we do the same.
        return "null";
      }
      if (bigNum && Number.isInteger(val) && !Number.isSafeInteger(val)) {
        // Represent as a quoted string to preserve precision.
        return escapeString(val.toFixed(0));
      }
      return String(val);
    }

    if (typeof val === "string") return escapeString(val);

    // 4. Object / array.
    if (typeof val === "object") {
      if (inStack.has(val)) {
        throw new TypeError(
          "Converting circular structure to JSON — cycle detected" +
            (key ? ` at property "${key}"` : " at the root"),
        );
      }
      inStack.add(val);
      try {
        if (Array.isArray(val)) return serArray(val, depth);
        return serObject(val as Record<string, unknown>, depth);
      } finally {
        inStack.delete(val);
      }
    }

    return undefined; // unreachable in well-typed TS, but guard for `any`
  }

  function serArray(arr: unknown[], depth: number): string {
    if (arr.length === 0) return "[]";

    const childDepth = depth + 1;
    const prefix = indent ? "\n" + indent.repeat(childDepth) : "";
    const suffix = indent ? "\n" + indent.repeat(depth) : "";
    const sep = indent ? ", " : ",";

    const parts: string[] = [];
    for (let i = 0; i < arr.length; i++) {
      const child = ser(String(i), arr[i], childDepth);
      parts.push(child === undefined ? "null" : child);
    }

    if (!indent) return "[" + parts.join(sep) + "]";
    return "[" + prefix + parts.join("," + prefix) + suffix + "]";
  }

  function serObject(obj: Record<string, unknown>, depth: number): string {
    const childDepth = depth + 1;
    const prefix = indent ? "\n" + indent.repeat(childDepth) : "";
    const suffix = indent ? "\n" + indent.repeat(depth) : "";
    const colon = indent ? ": " : ":";

    const keys: string[] = keyAllowSet
      ? Array.from(keyAllowSet).filter((k) => Object.prototype.hasOwnProperty.call(obj, k))
      : Object.keys(obj);

    const parts: string[] = [];
    for (const k of keys) {
      const valStr = ser(k, obj[k], childDepth);
      if (valStr === undefined) continue; // skip undefined / functions / symbols
      parts.push(escapeString(k) + colon + valStr);
    }

    if (parts.length === 0) return "{}";
    if (!indent) return "{" + parts.join(",") + "}";
    return "{" + prefix + parts.join("," + prefix) + suffix + "}";
  }

  return ser("", value, 0);
}

// ─── Parse ────────────────────────────────────────────────────────────────────

/**
 * Parse a JSON string into a JavaScript value.
 *
 * Throws JsonParseError (with line, column, and offset) on any syntax error.
 * Optionally keeps large integers as strings via `options.bigNumberAsString`.
 */
export function parse(source: string, options: ParseOptions = {}): JsonValue {
  const bigNum = options.bigNumberAsString ?? false;

  let pos = 0;    // current byte offset
  let line = 1;   // current 1-based line
  let col = 1;    // current 1-based column

  // ── Low-level helpers ──────────────────────────────────────────────────────

  function current(): string { return source[pos] ?? ""; }
  function eof(): boolean { return pos >= source.length; }

  function advance(): string {
    const c = source[pos++];
    if (c === "\n") { line++; col = 1; }
    else { col++; }
    return c;
  }

  function err(msg: string): never {
    throw new JsonParseError(msg, pos, line, col);
  }

  function expect(ch: string): void {
    if (current() !== ch) {
      err(`Expected '${ch}' but got ${eof() ? "end of input" : `'${current()}'`}`);
    }
    advance();
  }

  function skipWhitespace(): void {
    while (!eof()) {
      const c = current();
      if (c === " " || c === "\t" || c === "\r" || c === "\n") advance();
      else break;
    }
  }

  // ── Value dispatcher ───────────────────────────────────────────────────────

  function parseValue(): JsonValue {
    skipWhitespace();
    if (eof()) err("Unexpected end of input, expected a value");

    const c = current();
    if (c === "{") return parseObject();
    if (c === "[") return parseArray();
    if (c === '"') return parseString();
    if (c === "t") return parseLiteral("true", true);
    if (c === "f") return parseLiteral("false", false);
    if (c === "n") return parseLiteral("null", null);
    if (c === "-" || (c >= "0" && c <= "9")) return parseNumber();

    err(
      `Unexpected token '${c}'` +
        (c === "'" ? " — single-quoted strings are not valid JSON" : ""),
    );
  }

  // ── Literals ───────────────────────────────────────────────────────────────

  function parseLiteral<T>(word: string, result: T): T {
    for (let i = 0; i < word.length; i++) {
      if (current() !== word[i]) {
        err(
          `Invalid literal: expected '${word}' but got unexpected character '${current()}'`,
        );
      }
      advance();
    }
    return result;
  }

  // ── Numbers ────────────────────────────────────────────────────────────────

  function parseNumber(): number | string {
    const start = pos;
    let isInteger = true;

    // Optional leading minus.
    if (current() === "-") advance();

    // Integer part.
    if (eof() || current() < "0" || current() > "9") {
      err("Invalid number: digit expected after '-'");
    }
    if (current() === "0") {
      advance();
      // Leading zero must not be followed by another digit.
      if (!eof() && current() >= "0" && current() <= "9") {
        err("Invalid number: leading zeros are not allowed");
      }
    } else {
      while (!eof() && current() >= "0" && current() <= "9") advance();
    }

    // Optional fraction.
    if (!eof() && current() === ".") {
      isInteger = false;
      advance();
      if (eof() || current() < "0" || current() > "9") {
        err("Invalid number: digit expected after decimal point");
      }
      while (!eof() && current() >= "0" && current() <= "9") advance();
    }

    // Optional exponent.
    if (!eof() && (current() === "e" || current() === "E")) {
      isInteger = false;
      advance();
      if (!eof() && (current() === "+" || current() === "-")) advance();
      if (eof() || current() < "0" || current() > "9") {
        err("Invalid number: digit expected in exponent");
      }
      while (!eof() && current() >= "0" && current() <= "9") advance();
    }

    const raw = source.slice(start, pos);
    const num = Number(raw);

    // bigNumberAsString: return the raw text for integers that cannot be
    // represented exactly as a 64-bit float.
    if (bigNum && isInteger && !Number.isSafeInteger(num)) {
      return raw; // return as string
    }

    return num;
  }

  // ── Strings ────────────────────────────────────────────────────────────────

  function parseString(): string {
    expect('"');
    let out = "";

    while (true) {
      if (eof()) err("Unterminated string — reached end of input");

      const c = current();

      if (c === '"') { advance(); break; }

      if (c === "\\") {
        advance();
        if (eof()) err("Unterminated escape sequence at end of input");
        const e = current(); advance();
        switch (e) {
          case '"': out += '"'; break;
          case "\\": out += "\\"; break;
          case "/": out += "/"; break;
          case "b": out += "\b"; break;
          case "f": out += "\f"; break;
          case "n": out += "\n"; break;
          case "r": out += "\r"; break;
          case "t": out += "\t"; break;
          case "u":
            out += parseUnicodeEscape();
            break;
          default:
            err(`Invalid escape sequence '\\${e}'`);
        }
        continue;
      }

      // Control characters must be escaped.
      if (c.charCodeAt(0) < 0x20) {
        err(
          `Unescaped control character U+${c.charCodeAt(0).toString(16).padStart(4, "0")} in string`,
        );
      }

      out += c;
      advance();
    }

    return out;
  }

  function parseUnicodeEscape(): string {
    // Read exactly 4 hex digits.
    let hex = "";
    for (let i = 0; i < 4; i++) {
      if (eof()) err("Unexpected end of input inside \\u escape");
      const c = current();
      if (!((c >= "0" && c <= "9") || (c >= "a" && c <= "f") || (c >= "A" && c <= "F"))) {
        err(`Invalid hex digit '${c}' in \\u escape`);
      }
      hex += c;
      advance();
    }

    const code = parseInt(hex, 16);

    // Handle surrogate pairs for characters outside the BMP.
    if (code >= 0xd800 && code <= 0xdbff) {
      // High surrogate — must be followed by \uLowSurrogate.
      if (current() !== "\\") err("Expected low surrogate after high surrogate");
      advance();
      if (current() !== "u") err("Expected \\u for low surrogate");
      advance();
      let hex2 = "";
      for (let i = 0; i < 4; i++) {
        if (eof()) err("Unexpected end of input inside low surrogate \\u escape");
        const c = current();
        if (!((c >= "0" && c <= "9") || (c >= "a" && c <= "f") || (c >= "A" && c <= "F"))) {
          err(`Invalid hex digit '${c}' in low surrogate \\u escape`);
        }
        hex2 += c;
        advance();
      }
      const low = parseInt(hex2, 16);
      if (low < 0xdc00 || low > 0xdfff) {
        err(`Invalid low surrogate U+${low.toString(16).padStart(4, "0")}`);
      }
      return String.fromCodePoint(0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00));
    }

    if (code >= 0xdc00 && code <= 0xdfff) {
      err(`Unexpected low surrogate U+${code.toString(16).padStart(4, "0")} without preceding high surrogate`);
    }

    return String.fromCharCode(code);
  }

  // ── Arrays ─────────────────────────────────────────────────────────────────

  function parseArray(): JsonValue[] {
    expect("[");
    const arr: JsonValue[] = [];

    skipWhitespace();
    if (!eof() && current() === "]") { advance(); return arr; }

    while (true) {
      arr.push(parseValue());
      skipWhitespace();

      if (eof()) err("Unterminated array — expected ',' or ']'");

      if (current() === "]") { advance(); break; }
      if (current() !== ",") {
        err(`Expected ',' or ']' but got '${current()}'`);
      }
      advance();

      // Trailing comma: reject per spec.
      skipWhitespace();
      if (!eof() && current() === "]") {
        err("Trailing comma before ']' is not valid JSON");
      }
    }

    return arr;
  }

  // ── Objects ────────────────────────────────────────────────────────────────

  function parseObject(): { [key: string]: JsonValue } {
    expect("{");
    const obj: { [key: string]: JsonValue } = {};

    skipWhitespace();
    if (!eof() && current() === "}") { advance(); return obj; }

    while (true) {
      skipWhitespace();
      if (eof() || current() !== '"') {
        err(
          eof()
            ? "Unterminated object — expected a key"
            : `Object key must be a string, got '${current()}'`,
        );
      }
      const key = parseString();

      skipWhitespace();
      expect(":");

      const value = parseValue();
      obj[key] = value;

      skipWhitespace();
      if (eof()) err("Unterminated object — expected ',' or '}'");

      if (current() === "}") { advance(); break; }
      if (current() !== ",") {
        err(`Expected ',' or '}' but got '${current()}'`);
      }
      advance();

      // Trailing comma: reject per spec.
      skipWhitespace();
      if (!eof() && current() === "}") {
        err("Trailing comma before '}' is not valid JSON");
      }
    }

    return obj;
  }

  // ── Entry point ────────────────────────────────────────────────────────────

  const result = parseValue();
  skipWhitespace();
  if (!eof()) {
    err(`Unexpected token '${current()}' after top-level value`);
  }
  return result;
}

// ─── Self-tests ───────────────────────────────────────────────────────────────

type TestResult = { name: string; ok: boolean; error?: string };
const results: TestResult[] = [];

function test(name: string, fn: () => void): void {
  try {
    fn();
    results.push({ name, ok: true });
  } catch (e) {
    results.push({ name, ok: false, error: String(e) });
  }
}

function assert(cond: boolean, msg?: string): void {
  if (!cond) throw new Error(msg ?? "Assertion failed");
}

function assertEqual(a: unknown, b: unknown): void {
  const as = JSON.stringify(a);
  const bs = JSON.stringify(b);
  if (as !== bs) throw new Error(`Expected ${as} to equal ${bs}`);
}

function assertThrows(fn: () => unknown, check?: (e: unknown) => boolean): void {
  let threw = false;
  try { fn(); }
  catch (e) {
    threw = true;
    if (check && !check(e)) throw new Error(`Exception did not pass check: ${e}`);
  }
  if (!threw) throw new Error("Expected an exception but none was thrown");
}

// ── Stringify tests ────────────────────────────────────────────────────────

test("stringify: null and booleans", () => {
  assertEqual(stringify(null), "null");
  assertEqual(stringify(true), "true");
  assertEqual(stringify(false), "false");
});

test("stringify: numbers", () => {
  assertEqual(stringify(0), "0");
  assertEqual(stringify(-1), "-1");
  assertEqual(stringify(3.14), "3.14");
  assertEqual(stringify(1e20), "100000000000000000000");
});

test("stringify: NaN and Infinity become null", () => {
  assertEqual(stringify(NaN), "null");
  assertEqual(stringify(Infinity), "null");
  assertEqual(stringify(-Infinity), "null");
});

test("stringify: strings with escapes", () => {
  assertEqual(stringify(""), '""');
  assertEqual(stringify("hello"), '"hello"');
  assertEqual(stringify('say "hi"'), '"say \\"hi\\""');
  assertEqual(stringify("line\nbreak"), '"line\\nbreak"');
  assertEqual(stringify("tab\there"), '"tab\\there"');
  assertEqual(stringify("back\\slash"), '"back\\\\slash"');
  assertEqual(stringify("\u0000"), '"\\u0000"');
  assertEqual(stringify("\u001f"), '"\\u001f"');
});

test("stringify: arrays", () => {
  assertEqual(stringify([]), "[]");
  assertEqual(stringify([1, 2, 3]), "[1,2,3]");
  assertEqual(stringify([null, true, "x"]), '[null,true,"x"]');
  // Undefined inside arrays becomes null
  assertEqual(stringify([undefined, 1]), "[null,1]");
});

test("stringify: objects", () => {
  assertEqual(stringify({}), "{}");
  assertEqual(stringify({ a: 1 }), '{"a":1}');
  assertEqual(stringify({ a: undefined }), "{}");
  assertEqual(stringify({ a: 1, b: "x" }), '{"a":1,"b":"x"}');
});

test("stringify: nested structures", () => {
  const val = { a: [1, { b: true }, null] };
  assertEqual(stringify(val), '{"a":[1,{"b":true},null]}');
});

test("stringify: indentation (number)", () => {
  const val = { a: 1 };
  const out = stringify(val, { space: 2 });
  assertEqual(out, '{\n  "a": 1\n}');
});

test("stringify: indentation (string)", () => {
  const out = stringify([1, 2], { space: "\t" });
  assertEqual(out, "[\n\t1,\n\t2\n]");
});

test("stringify: function replacer", () => {
  const out = stringify({ a: 1, b: 2 }, {
    replacer: (key, val) => key === "b" ? undefined : val,
  });
  assertEqual(out, '{"a":1}');
});

test("stringify: function replacer — transform values", () => {
  const out = stringify({ x: 42 }, {
    replacer: (_key, val) => typeof val === "number" ? val * 2 : val,
  });
  assertEqual(out, '{"x":84}');
});

test("stringify: array replacer (key filter)", () => {
  const out = stringify({ a: 1, b: 2, c: 3 }, { replacer: ["a", "c"] });
  assertEqual(out, '{"a":1,"c":3}');
});

test("stringify: cycle detection", () => {
  const obj: Record<string, unknown> = {};
  obj["self"] = obj;
  assertThrows(() => stringify(obj), (e) =>
    e instanceof TypeError && /circular/i.test(String(e)));
});

test("stringify: cycle detection in array", () => {
  const arr: unknown[] = [1];
  arr.push(arr);
  assertThrows(() => stringify(arr), (e) =>
    e instanceof TypeError && /circular/i.test(String(e)));
});

test("stringify: does not falsely flag shared references as cycles", () => {
  const shared = { v: 1 };
  // shared appears twice but is not a cycle
  const out = stringify([shared, shared]);
  assertEqual(out, '[{"v":1},{"v":1}]');
});

test("stringify: toJSON support", () => {
  const val = {
    x: 1,
    toJSON: () => "serialized",
  };
  assertEqual(stringify(val), '"serialized"');
});

test("stringify: Date.toJSON", () => {
  const d = new Date("2024-01-15T00:00:00.000Z");
  const out = stringify(d);
  assert(typeof out === "string" && out.includes("2024-01-15"), out);
});

test("stringify: bigNumberAsString — unsafe integer", () => {
  const big = 9007199254740993; // MAX_SAFE_INTEGER + 2
  const out = stringify(big, { bigNumberAsString: true });
  assertEqual(out, '"9007199254740993"');
});

test("stringify: bigNumberAsString — safe integer stays numeric", () => {
  const safe = 9007199254740991; // MAX_SAFE_INTEGER
  const out = stringify(safe, { bigNumberAsString: true });
  assertEqual(out, "9007199254740991");
});

test("stringify: bigNumberAsString — float stays numeric even if large", () => {
  const f = 1.5e20; // not an integer
  const out = stringify(f, { bigNumberAsString: true });
  assertEqual(out, "150000000000000000000");
});

test("stringify: bigint throws", () => {
  assertThrows(() => stringify(42n as unknown), (e) => e instanceof TypeError);
});

test("stringify: undefined at root returns undefined", () => {
  assertEqual(stringify(undefined), undefined);
});

test("stringify: function at root returns undefined", () => {
  assertEqual(stringify(() => 1), undefined);
});

// ── Parse tests ────────────────────────────────────────────────────────────

test("parse: null, true, false", () => {
  assertEqual(parse("null"), null);
  assertEqual(parse("true"), true);
  assertEqual(parse("false"), false);
});

test("parse: integers", () => {
  assertEqual(parse("0"), 0);
  assertEqual(parse("42"), 42);
  assertEqual(parse("-7"), -7);
  assertEqual(parse("1000000"), 1000000);
});

test("parse: floats", () => {
  assertEqual(parse("3.14"), 3.14);
  assertEqual(parse("-0.5"), -0.5);
  assertEqual(parse("1e10"), 1e10);
  assertEqual(parse("1.5E-3"), 1.5e-3);
  assertEqual(parse("2.5e+2"), 250);
});

test("parse: strings", () => {
  assertEqual(parse('""'), "");
  assertEqual(parse('"hello"'), "hello");
  assertEqual(parse('"line\\nbreak"'), "line\nbreak");
  assertEqual(parse('"tab\\there"'), "tab\there");
  assertEqual(parse('"quote: \\""'), 'quote: "');
  assertEqual(parse('"slash: \\/"'), "slash: /");
  assertEqual(parse('"\\u0041"'), "A");
  assertEqual(parse('"\\u00e9"'), "é");
});

test("parse: surrogate pairs", () => {
  // U+1F600 GRINNING FACE encoded as a surrogate pair
  const emoji = parse('"\\uD83D\\uDE00"');
  assertEqual(emoji, "\uD83D\uDE00");
});

test("parse: arrays", () => {
  assertEqual(parse("[]"), []);
  assertEqual(parse("[1,2,3]"), [1, 2, 3]);
  assertEqual(parse('["a",null,true]'), ["a", null, true]);
  assertEqual(parse("[[1,[2]],3]"), [[1, [2]], 3]);
});

test("parse: objects", () => {
  assertEqual(parse("{}"), {});
  assertEqual(parse('{"a":1}'), { a: 1 });
  assertEqual(parse('{"a":1,"b":"x"}'), { a: 1, b: "x" });
});

test("parse: whitespace tolerance", () => {
  assertEqual(parse("  null  "), null);
  assertEqual(parse("  [ 1 , 2 ]  "), [1, 2]);
  assertEqual(parse('{ "k" : "v" }'), { k: "v" });
});

test("parse: nested", () => {
  const src = '{"a":[1,{"b":true},null]}';
  assertEqual(parse(src), { a: [1, { b: true }, null] });
});

test("parse: position in error — basic offset", () => {
  let caught: JsonParseError | undefined;
  try { parse("{bad}"); }
  catch (e) { if (e instanceof JsonParseError) caught = e; }
  assert(caught !== undefined, "should throw JsonParseError");
  assert(caught!.offset === 1, `offset should be 1, got ${caught!.offset}`);
  assert(caught!.line === 1, `line should be 1, got ${caught!.line}`);
  assert(caught!.column === 2, `column should be 2, got ${caught!.column}`);
});

test("parse: position in error — multi-line", () => {
  let caught: JsonParseError | undefined;
  try { parse('{\n  "a": BAD\n}'); }
  catch (e) { if (e instanceof JsonParseError) caught = e; }
  assert(caught !== undefined);
  assert(caught!.line === 2, `line should be 2, got ${caught!.line}`);
});

test("parse: error on trailing comma in array", () => {
  assertThrows(() => parse("[1,2,]"), (e) => e instanceof JsonParseError);
});

test("parse: error on trailing comma in object", () => {
  assertThrows(() => parse('{"a":1,}'), (e) => e instanceof JsonParseError);
});

test("parse: error on leading zero", () => {
  assertThrows(() => parse("01"), (e) => e instanceof JsonParseError);
});

test("parse: error on bare string", () => {
  assertThrows(() => parse("hello"), (e) => e instanceof JsonParseError);
});

test("parse: error on single-quoted string", () => {
  assertThrows(() => parse("'hello'"), (e) => e instanceof JsonParseError);
});

test("parse: error on unterminated string", () => {
  assertThrows(() => parse('"no end'), (e) => e instanceof JsonParseError);
});

test("parse: error on invalid escape", () => {
  assertThrows(() => parse('"\\q"'), (e) => e instanceof JsonParseError);
});

test("parse: error on control character in string", () => {
  assertThrows(() => parse('"a\nb"'), (e) => e instanceof JsonParseError);
});

test("parse: error on trailing content", () => {
  assertThrows(() => parse("1 2"), (e) => e instanceof JsonParseError);
});

test("parse: error on empty input", () => {
  assertThrows(() => parse(""), (e) => e instanceof JsonParseError);
});

test("parse: error on unterminated array", () => {
  assertThrows(() => parse("[1,2"), (e) => e instanceof JsonParseError);
});

test("parse: error on unterminated object", () => {
  assertThrows(() => parse('{"a":1'), (e) => e instanceof JsonParseError);
});

test("parse: error on numeric key in object", () => {
  assertThrows(() => parse("{1:2}"), (e) => e instanceof JsonParseError);
});

test("parse: bigNumberAsString — unsafe integer returned as string", () => {
  const val = parse("9007199254740993", { bigNumberAsString: true });
  assertEqual(val, "9007199254740993");
  assert(typeof val === "string");
});

test("parse: bigNumberAsString — safe integer stays number", () => {
  const val = parse("9007199254740991", { bigNumberAsString: true });
  assertEqual(val, 9007199254740991);
  assert(typeof val === "number");
});

test("parse: bigNumberAsString — float stays number even if large", () => {
  const val = parse("1.5e20", { bigNumberAsString: true });
  assert(typeof val === "number");
});

test("parse: bigNumberAsString — negative unsafe integer as string", () => {
  const val = parse("-9007199254740993", { bigNumberAsString: true });
  assertEqual(val, "-9007199254740993");
  assert(typeof val === "string");
});

test("parse: bigNumberAsString — large int inside object", () => {
  const val = parse('{"id":9007199254740993}', { bigNumberAsString: true }) as Record<string, JsonValue>;
  assert(typeof val["id"] === "string");
  assertEqual(val["id"], "9007199254740993");
});

// ── Round-trip tests ───────────────────────────────────────────────────────

test("round-trip: basic object", () => {
  const src = { a: 1, b: [true, null, "x"], c: { d: 3.14 } };
  const rt = parse(stringify(src)!);
  assertEqual(rt, src);
});

test("round-trip: special string characters", () => {
  const src = { msg: 'say "hello"\n\tthere\\you' };
  const rt = parse(stringify(src)!);
  assertEqual(rt, src);
});

test("round-trip: empty containers", () => {
  assertEqual(parse(stringify([])!), []);
  assertEqual(parse(stringify({})!), {});
});

test("round-trip: deeply nested", () => {
  const src = { a: { b: { c: { d: [1, [2, [3]]] } } } };
  assertEqual(parse(stringify(src)!), src);
});

// ── Print report ───────────────────────────────────────────────────────────

function runReport(): void {
  let passed = 0;
  let failed = 0;

  for (const r of results) {
    if (r.ok) {
      passed++;
      console.log(`  ✓  ${r.name}`);
    } else {
      failed++;
      console.error(`  ✗  ${r.name}`);
      console.error(`       ${r.error}`);
    }
  }

  const total = passed + failed;
  console.log(`\n${passed}/${total} tests passed${failed > 0 ? ` — ${failed} FAILED` : ""}`);

  if (failed > 0) process.exit(1);
}

// Run when executed directly (ts-node json.ts or node json.js)
// In Node.js / ts-node the entry-point module has no parent; check argv[1].
const isMain =
  typeof process !== "undefined" &&
  typeof require !== "undefined" &&
  require.main === module;

if (isMain) {
  runReport();
}
