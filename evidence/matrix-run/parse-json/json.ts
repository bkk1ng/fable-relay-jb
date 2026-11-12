/**
 * json.ts — JSON serializer and parser, TypeScript strict, zero dependencies.
 *
 * Exports:
 *   stringify(value, options?)  — serialize to JSON string
 *   parse(text, options?)       — parse JSON string to value
 *   JsonParseError              — error type with position, line, column
 *
 * Features:
 *   stringify: cycle detection, replacer (function|array), indent, bigint-as-string
 *   parse:     position-tracked errors with line/column, big-number-as-string, reviver
 */

// ─── Public types ─────────────────────────────────────────────────────────────

export type JsonPrimitive = string | number | boolean | null;
export type JsonValue    = JsonPrimitive | JsonObject | JsonArray;
export type JsonObject   = { [key: string]: JsonValue };
export type JsonArray    = JsonValue[];

export type ReplacerFn   = (key: string, value: unknown) => unknown;
export type ReplacerList = (string | number)[];
export type Replacer     = ReplacerFn | ReplacerList | null | undefined;

export interface StringifyOptions {
  /**
   * Function replacer filters/transforms each value; array replacer acts as an
   * allowlist of property names (same semantics as native JSON.stringify).
   */
  replacer?: Replacer;
  /** Indent amount: number of spaces (capped at 10) or an explicit string. */
  space?: string | number;
  /**
   * When true, BigInt values are serialized as their decimal string
   * representation instead of throwing.
   */
  bigintAsString?: boolean;
}

export interface ParseOptions {
  /**
   * When true, integers outside ±(2^53 − 1) are returned as their raw decimal
   * string instead of a lossy Number.
   */
  bigNumberAsString?: boolean;
  /**
   * Standard reviver: called bottom-up with (key, value) for every node.
   * Returning undefined deletes the property.
   */
  reviver?: (key: string, value: unknown) => unknown;
}

// ─── Error class ──────────────────────────────────────────────────────────────

export class JsonParseError extends Error {
  readonly position: number;
  readonly line: number;
  readonly column: number;

  constructor(
    message: string,
    position: number,
    line: number,
    column: number,
  ) {
    super(`${message} (line ${line}, column ${column}, position ${position})`);
    this.name = "JsonParseError";
    this.position = position;
    this.line     = line;
    this.column   = column;
    // Restore prototype chain (tsc targets < ES2015 need this).
    Object.setPrototypeOf(this, new.target.prototype);
  }
}

// ─── Stringify ────────────────────────────────────────────────────────────────

/**
 * Serialize `value` to a JSON string.
 * Returns `undefined` for top-level `undefined`, functions, and symbols,
 * matching the behaviour of native JSON.stringify.
 */
export function stringify(
  value: unknown,
  options: StringifyOptions = {},
): string | undefined {
  const { replacer, space, bigintAsString = false } = options;

  // ── Normalize indent ──
  let indent = "";
  if (typeof space === "number") {
    indent = " ".repeat(Math.min(10, Math.max(0, Math.floor(space))));
  } else if (typeof space === "string") {
    indent = space.slice(0, 10);
  }

  // ── Normalize replacer ──
  let replacerFn: ReplacerFn | null = null;
  let allowList: Set<string> | null = null;

  if (typeof replacer === "function") {
    replacerFn = replacer;
  } else if (Array.isArray(replacer)) {
    allowList = new Set(replacer.map(String));
  }

  // Track object references for cycle detection.
  const seen = new WeakSet<object>();

  // ── Escape a string value ──
  function escapeString(s: string): string {
    let out = '"';
    for (let i = 0; i < s.length; i++) {
      const cp = s.charCodeAt(i);
      switch (cp) {
        case 0x22: out += '\\"';  break;
        case 0x5c: out += '\\\\'; break;
        case 0x08: out += '\\b';  break;
        case 0x09: out += '\\t';  break;
        case 0x0a: out += '\\n';  break;
        case 0x0c: out += '\\f';  break;
        case 0x0d: out += '\\r';  break;
        default:
          if (cp < 0x20) {
            out += "\\u" + cp.toString(16).padStart(4, "0");
          } else {
            out += s[i];
          }
      }
    }
    out += '"';
    return out;
  }

  // ── Serialize an array ──
  function serializeArray(arr: unknown[], depth: number): string {
    if (arr.length === 0) return "[]";

    const items: string[] = [];
    for (let i = 0; i < arr.length; i++) {
      const serialized = serializeValue(String(i), arr[i], depth + 1);
      // Array holes / undefined slots → "null" per spec.
      items.push(serialized ?? "null");
    }

    if (indent === "") {
      return "[" + items.join(",") + "]";
    }
    const inner = indent.repeat(depth + 1);
    const outer = indent.repeat(depth);
    return "[\n" + items.map(v => inner + v).join(",\n") + "\n" + outer + "]";
  }

  // ── Serialize an object ──
  function serializeObject(
    obj: Record<string, unknown>,
    depth: number,
  ): string {
    const pairs: string[] = [];

    for (const k of Object.keys(obj)) {
      if (allowList !== null && !allowList.has(k)) continue;

      const serialized = serializeValue(k, obj[k], depth + 1);
      if (serialized === undefined) continue; // drop undefined values

      const sep = indent === "" ? ":" : ": ";
      pairs.push(escapeString(k) + sep + serialized);
    }

    if (pairs.length === 0) return "{}";

    if (indent === "") {
      return "{" + pairs.join(",") + "}";
    }
    const inner = indent.repeat(depth + 1);
    const outer = indent.repeat(depth);
    return "{\n" + pairs.map(p => inner + p).join(",\n") + "\n" + outer + "}";
  }

  // ── Core recursive serializer ──
  function serializeValue(
    key: string,
    val: unknown,
    depth: number,
  ): string | undefined {
    // 1. Apply replacer function.
    if (replacerFn !== null) {
      val = replacerFn(key, val);
    }

    // 2. Call toJSON if present (before second replacer pass, same as spec).
    if (
      val !== null &&
      val !== undefined &&
      typeof val === "object" &&
      typeof (val as Record<string, unknown>)["toJSON"] === "function"
    ) {
      val = (val as { toJSON(k: string): unknown }).toJSON(key);
      if (replacerFn !== null) {
        val = replacerFn(key, val);
      }
    }

    // 3. Primitives that serialize to undefined (drop from output).
    if (val === undefined)              return undefined;
    if (typeof val === "function")      return undefined;
    if (typeof val === "symbol")        return undefined;

    // 4. Null / booleans.
    if (val === null)                   return "null";
    if (typeof val === "boolean")       return val ? "true" : "false";

    // 5. BigInt.
    if (typeof val === "bigint") {
      if (bigintAsString) return escapeString(val.toString());
      throw new TypeError(
        `Cannot serialize BigInt at key "${key}". ` +
        "Pass { bigintAsString: true } to encode as a decimal string.",
      );
    }

    // 6. Numbers.
    if (typeof val === "number") {
      if (!isFinite(val)) return "null"; // NaN / ±Infinity → null per spec.
      if (Object.is(val, -0)) return "0";
      return String(val);
    }

    // 7. Strings.
    if (typeof val === "string") return escapeString(val);

    // 8. Objects and arrays.
    const obj = val as object;
    if (seen.has(obj)) {
      throw new TypeError(
        `Circular reference detected at key "${key}".`,
      );
    }
    seen.add(obj);
    try {
      return Array.isArray(obj)
        ? serializeArray(obj, depth)
        : serializeObject(obj as Record<string, unknown>, depth);
    } finally {
      seen.delete(obj);
    }
  }

  // The root call: replacer receives key="" and the boxed value, per spec.
  let rootVal: unknown = value;
  if (replacerFn !== null) {
    rootVal = replacerFn("", value);
  }
  if (rootVal === undefined) return undefined;

  return serializeValue("", rootVal, 0);
}

// ─── Parser ───────────────────────────────────────────────────────────────────

/**
 * Parse a JSON string into a JavaScript value.
 * Throws `JsonParseError` with line/column/position on any syntax error.
 */
export function parse(text: string, options: ParseOptions = {}): unknown {
  const { bigNumberAsString = false, reviver } = options;

  const src = String(text);
  let pos       = 0;
  let line      = 1;
  let lineStart = 0;

  // ── Position helpers ──
  function col(): number {
    return pos - lineStart + 1;
  }

  function fail(msg: string): never {
    throw new JsonParseError(msg, pos, line, col());
  }

  function eat(): string {
    const ch = src[pos++];
    if (ch === "\n") { line++; lineStart = pos; }
    return ch;
  }

  function skipWS(): void {
    while (pos < src.length) {
      const ch = src[pos];
      if (ch === " " || ch === "\t" || ch === "\r" || ch === "\n") {
        eat();
      } else {
        break;
      }
    }
  }

  // ── String ──
  function parseString(): string {
    if (src[pos] !== '"') fail(`Expected '"', got '${src[pos] ?? "EOF"}'`);
    pos++; // opening quote

    let out = "";
    while (pos < src.length) {
      const ch = src[pos];

      if (ch === '"') { pos++; return out; }  // closing quote

      if (ch === "\\") {
        pos++; // backslash
        if (pos >= src.length) fail("Unexpected end inside escape sequence");
        const esc = src[pos++];
        switch (esc) {
          case '"':  out += '"';  break;
          case "\\": out += "\\"; break;
          case "/":  out += "/";  break;
          case "b":  out += "\b"; break;
          case "f":  out += "\f"; break;
          case "n":  out += "\n"; break;
          case "r":  out += "\r"; break;
          case "t":  out += "\t"; break;
          case "u": {
            if (pos + 4 > src.length) fail("Incomplete \\u escape");
            const hex = src.slice(pos, pos + 4);
            if (!/^[0-9a-fA-F]{4}$/.test(hex)) {
              fail(`Invalid \\u escape: \\u${hex}`);
            }
            const hi = parseInt(hex, 16);
            pos += 4;

            // Handle surrogate pairs (𝌆 etc.).
            if (hi >= 0xd800 && hi <= 0xdbff) {
              if (src[pos] === "\\" && src[pos + 1] === "u") {
                const lo4 = src.slice(pos + 2, pos + 6);
                if (/^[0-9a-fA-F]{4}$/.test(lo4)) {
                  const lo = parseInt(lo4, 16);
                  if (lo >= 0xdc00 && lo <= 0xdfff) {
                    const full =
                      0x10000 + ((hi - 0xd800) << 10) + (lo - 0xdc00);
                    out += String.fromCodePoint(full);
                    pos += 6;
                    break;
                  }
                }
              }
              // Lone high surrogate — emit as-is (lenient, matches V8).
            }
            out += String.fromCharCode(hi);
            break;
          }
          default:
            fail(`Invalid escape character: \\${esc}`);
        }
        continue;
      }

      // Unescaped control characters are illegal.
      if (src.charCodeAt(pos) < 0x20) {
        fail(
          `Unexpected control character U+${src.charCodeAt(pos)
            .toString(16)
            .padStart(4, "0")} in string`,
        );
      }

      out += ch;
      pos++;
    }
    fail("Unterminated string");
  }

  // ── Number ──
  function parseNumber(): number | string {
    const start = pos;

    if (src[pos] === "-") pos++;

    if (pos >= src.length) fail("Invalid number: unexpected end");

    if (src[pos] === "0") {
      pos++;
    } else if (src[pos] >= "1" && src[pos] <= "9") {
      while (pos < src.length && src[pos] >= "0" && src[pos] <= "9") pos++;
    } else {
      fail(`Invalid number: unexpected character '${src[pos]}'`);
    }

    // Fraction.
    if (pos < src.length && src[pos] === ".") {
      pos++;
      if (pos >= src.length || src[pos] < "0" || src[pos] > "9") {
        fail("Invalid number: expected digit after '.'");
      }
      while (pos < src.length && src[pos] >= "0" && src[pos] <= "9") pos++;
    }

    // Exponent.
    if (pos < src.length && (src[pos] === "e" || src[pos] === "E")) {
      pos++;
      if (pos < src.length && (src[pos] === "+" || src[pos] === "-")) pos++;
      if (pos >= src.length || src[pos] < "0" || src[pos] > "9") {
        fail("Invalid number: expected digit in exponent");
      }
      while (pos < src.length && src[pos] >= "0" && src[pos] <= "9") pos++;
    }

    const raw = src.slice(start, pos);
    const num = Number(raw);

    if (bigNumberAsString) {
      // Only integer numerals can overflow safely; non-integer large numbers
      // keep their floating-point representation (e.g. 1e400 → Infinity → 0).
      if (
        !raw.includes(".") &&
        !raw.includes("e") &&
        !raw.includes("E") &&
        (num > Number.MAX_SAFE_INTEGER || num < Number.MIN_SAFE_INTEGER)
      ) {
        return raw;
      }
    }

    return num;
  }

  // ── Literals ──
  function parseLiteral(word: string, result: boolean | null): boolean | null {
    for (let i = 0; i < word.length; i++) {
      if (pos >= src.length || src[pos] !== word[i]) {
        fail(`Expected '${word}', got invalid token`);
      }
      pos++;
    }
    return result;
  }

  // ── Array ──
  function parseArray(): unknown[] {
    pos++; // '['
    skipWS();

    const arr: unknown[] = [];
    if (pos < src.length && src[pos] === "]") { pos++; return arr; }

    while (true) {
      skipWS();
      arr.push(parseValue());
      skipWS();
      if (pos >= src.length) fail("Unterminated array");
      const ch = src[pos];
      if (ch === "]") { pos++; break; }
      if (ch !== ",") fail(`Expected ',' or ']', got '${ch}'`);
      pos++; // ','
    }
    return arr;
  }

  // ── Object ──
  function parseObject(): Record<string, unknown> {
    pos++; // '{'
    skipWS();

    const obj: Record<string, unknown> = {};
    if (pos < src.length && src[pos] === "}") { pos++; return obj; }

    while (true) {
      skipWS();
      if (pos >= src.length || src[pos] !== '"') {
        fail(
          `Expected string key, got '${src[pos] ?? "EOF"}'`,
        );
      }
      const key = parseString();
      skipWS();

      if (pos >= src.length || src[pos] !== ":") {
        fail(`Expected ':' after key "${key}", got '${src[pos] ?? "EOF"}'`);
      }
      pos++; // ':'
      skipWS();

      obj[key] = parseValue();
      skipWS();

      if (pos >= src.length) fail("Unterminated object");
      const ch = src[pos];
      if (ch === "}") { pos++; break; }
      if (ch !== ",") fail(`Expected ',' or '}', got '${ch}'`);
      pos++; // ','
    }
    return obj;
  }

  // ── Entry point ──
  function parseValue(): unknown {
    skipWS();
    if (pos >= src.length) fail("Unexpected end of JSON input");

    const ch = src[pos];
    if (ch === '"') return parseString();
    if (ch === "{") return parseObject();
    if (ch === "[") return parseArray();
    if (ch === "t") return parseLiteral("true",  true);
    if (ch === "f") return parseLiteral("false", false);
    if (ch === "n") return parseLiteral("null",  null);
    if (ch === "-" || (ch >= "0" && ch <= "9")) return parseNumber();
    fail(`Unexpected token '${ch}'`);
  }

  const root = parseValue();
  skipWS();

  if (pos < src.length) {
    fail(`Unexpected token '${src[pos]}' after JSON value`);
  }

  // ── Reviver (bottom-up, per spec) ──
  if (reviver) {
    function walk(val: unknown, key: string): unknown {
      if (Array.isArray(val)) {
        for (let i = 0; i < val.length; i++) {
          const child = walk(val[i], String(i));
          if (child === undefined) {
            // spec: delete the index
            delete (val as unknown[])[i];
          } else {
            (val as unknown[])[i] = child;
          }
        }
      } else if (val !== null && typeof val === "object") {
        const obj = val as Record<string, unknown>;
        for (const k of Object.keys(obj)) {
          const child = walk(obj[k], k);
          if (child === undefined) {
            delete obj[k];
          } else {
            obj[k] = child;
          }
        }
      }
      return reviver(key, val);
    }
    return walk(root, "");
  }

  return root;
}

// ─── Self-tests ───────────────────────────────────────────────────────────────

function runTests(): void {
  let passed = 0;
  let failed = 0;

  function assert(description: string, condition: boolean): void {
    if (condition) {
      console.log(`  ✓ ${description}`);
      passed++;
    } else {
      console.error(`  ✗ FAIL: ${description}`);
      failed++;
    }
  }

  function assertThrows(
    description: string,
    fn: () => unknown,
    check?: (e: unknown) => boolean,
  ): void {
    try {
      fn();
      console.error(`  ✗ FAIL (no throw): ${description}`);
      failed++;
    } catch (e) {
      if (check && !check(e)) {
        console.error(
          `  ✗ FAIL (wrong error): ${description} → ${String(e)}`,
        );
        failed++;
      } else {
        console.log(`  ✓ ${description}`);
        passed++;
      }
    }
  }

  function eq(a: unknown, b: unknown): boolean {
    return JSON.stringify(a) === JSON.stringify(b);
  }

  // ── Stringify: primitives ──
  console.log("\nstringify — primitives");
  assert("null",             stringify(null) === "null");
  assert("true",             stringify(true) === "true");
  assert("false",            stringify(false) === "false");
  assert("integer",          stringify(42) === "42");
  assert("float",            stringify(3.14) === "3.14");
  assert("negative",         stringify(-7) === "-7");
  assert("zero",             stringify(0) === "0");
  assert("-0 → 0",           stringify(-0) === "0");
  assert("NaN → null",       stringify(NaN) === "null");
  assert("Infinity → null",  stringify(Infinity) === "null");
  assert("-Infinity → null", stringify(-Infinity) === "null");
  assert("string",           stringify("hello") === '"hello"');
  assert("empty string",     stringify("") === '""');
  assert("undefined → undef",stringify(undefined) === undefined);

  // ── Stringify: string escaping ──
  console.log("\nstringify — string escaping");
  assert("quote",        stringify('"') === '"\\""');
  assert("backslash",    stringify("\\") === '"\\\\"');
  assert("tab",          stringify("\t") === '"\\t"');
  assert("newline",      stringify("\n") === '"\\n"');
  assert("cr",           stringify("\r") === '"\\r"');
  assert("backspace",    stringify("\b") === '"\\b"');
  assert("formfeed",     stringify("\f") === '"\\f"');
  assert("control U+001F", stringify("\x1f") === '"\\u001f"');
  assert("unicode passthrough", stringify("∑") === '"∑"');

  // ── Stringify: objects & arrays ──
  console.log("\nstringify — objects & arrays");
  assert("empty array",  stringify([]) === "[]");
  assert("empty object", stringify({}) === "{}");
  assert("array",        stringify([1, 2, 3]) === "[1,2,3]");
  assert("object",       stringify({ a: 1, b: 2 }) === '{"a":1,"b":2}');
  assert("nested",
    stringify({ x: [1, { y: true }] }) === '{"x":[1,{"y":true}]}');
  assert("undefined in object dropped",
    stringify({ a: 1, b: undefined }) === '{"a":1}');
  assert("function in object dropped",
    stringify({ a: 1, b: () => 0 }) === '{"a":1}');
  assert("undefined in array → null",
    stringify([1, undefined, 3]) === "[1,null,3]");

  // ── Stringify: indentation ──
  console.log("\nstringify — indentation");
  const indented = stringify({ a: [1, 2] }, { space: 2 });
  assert("2-space indent contains newline", (indented ?? "").includes("\n"));
  assert("2-space indent correct",
    indented === '{\n  "a": [\n    1,\n    2\n  ]\n}');
  assert("string space",
    stringify({ x: 1 }, { space: "\t" }) === '{\n\t"x": 1\n}');
  assert("space capped at 10",
    // 20 spaces → still 10
    (stringify({ x: 1 }, { space: 20 }) ?? "").includes("          ") &&
    !(stringify({ x: 1 }, { space: 20 }) ?? "").includes("           "));

  // ── Stringify: replacer function ──
  console.log("\nstringify — replacer function");
  const rfResult = stringify(
    { a: 1, b: 2, c: 3 },
    { replacer: (k, v) => (k === "b" ? undefined : v) },
  );
  assert("replacer fn drops key b", rfResult === '{"a":1,"c":3}');

  const rfDouble = stringify(
    { n: 5 },
    { replacer: (k, v) => (typeof v === "number" ? v * 2 : v) },
  );
  assert("replacer fn transforms numbers", rfDouble === '{"n":10}');

  // ── Stringify: replacer array ──
  console.log("\nstringify — replacer array");
  const raResult = stringify({ a: 1, b: 2, c: 3 }, { replacer: ["a", "c"] });
  assert("replacer array keeps only listed keys", raResult === '{"a":1,"c":3}');

  // ── Stringify: cycle detection ──
  console.log("\nstringify — cycle detection");
  const cyc: Record<string, unknown> = { x: 1 };
  cyc["self"] = cyc;
  assertThrows(
    "circular reference throws TypeError",
    () => stringify(cyc),
    e => e instanceof TypeError && (e as TypeError).message.includes("Circular"),
  );

  const cycArr: unknown[] = [1, 2];
  cycArr.push(cycArr);
  assertThrows(
    "circular array throws TypeError",
    () => stringify(cycArr),
    e => e instanceof TypeError,
  );

  // ── Stringify: bigint ──
  console.log("\nstringify — bigint");
  assertThrows(
    "bigint throws without option",
    () => stringify(BigInt(9999)),
    e => e instanceof TypeError,
  );
  assert(
    "bigint serialized as string with option",
    stringify(BigInt("12345678901234567890"), { bigintAsString: true }) ===
      '"12345678901234567890"',
  );

  // ── Stringify: toJSON ──
  console.log("\nstringify — toJSON");
  const withToJSON = {
    a: 1,
    toJSON() { return { b: 2 }; },
  };
  assert("toJSON called", stringify(withToJSON) === '{"b":2}');

  // ── Parse: primitives ──
  console.log("\nparse — primitives");
  assert("null",   parse("null") === null);
  assert("true",   parse("true") === true);
  assert("false",  parse("false") === false);
  assert("integer", parse("42") === 42);
  assert("float",   parse("3.14") === 3.14);
  assert("negative", parse("-7") === -7);
  assert("zero",    parse("0") === 0);
  assert("string",  parse('"hello"') === "hello");
  assert("empty string", parse('""') === "");
  assert("whitespace padded", parse("  42  ") === 42);

  // ── Parse: numbers ──
  console.log("\nparse — numbers");
  assert("exponent", parse("1e3") === 1000);
  assert("neg exponent", parse("1.5e-2") === 0.015);
  assert("capital E", parse("2E5") === 200000);
  assert("positive exponent sign", parse("2e+3") === 2000);

  // ── Parse: strings ──
  console.log("\nparse — strings");
  assert("escape quote",    parse('"\\""') === '"');
  assert("escape backslash",parse('"\\\\')  + '"' === '"\\\\',
    parse('"\\\\"') === "\\");
  assert("escape tab",      parse('"\\t"') === "\t");
  assert("escape newline",  parse('"\\n"') === "\n");
  assert("escape unicode",  parse('"\\u0041"') === "A");
  assert("escape surrogate pair", parse('"\\uD83D\\uDE00"') === "😀");
  assert("solidus escape",  parse('"a\\/b"') === "a/b");

  // ── Parse: arrays & objects ──
  console.log("\nparse — arrays & objects");
  assert("empty array",  eq(parse("[]"), []));
  assert("empty object", eq(parse("{}"), {}));
  assert("array",        eq(parse("[1,2,3]"), [1, 2, 3]));
  assert("object",       eq(parse('{"a":1,"b":true}'), { a: 1, b: true }));
  assert("nested",
    eq(parse('{"x":[1,{"y":false}]}'), { x: [1, { y: false }] }));
  assert("whitespace in object",
    eq(parse('{ "a" : 1 , "b" : 2 }'), { a: 1, b: 2 }));

  // ── Parse: errors with position tracking ──
  console.log("\nparse — position-tracked errors");

  assertThrows(
    "trailing garbage reports position",
    () => parse('42 garbage'),
    e => e instanceof JsonParseError && (e as JsonParseError).position === 3,
  );

  assertThrows(
    "unterminated string",
    () => parse('"hello'),
    e => e instanceof JsonParseError,
  );

  assertThrows(
    "invalid escape",
    () => parse('"\\q"'),
    e => e instanceof JsonParseError &&
      (e as JsonParseError).message.includes("Invalid escape"),
  );

  assertThrows(
    "missing colon",
    () => parse('{"a" 1}'),
    e => e instanceof JsonParseError,
  );

  assertThrows(
    "trailing comma in object",
    () => parse('{"a":1,}'),
    e => e instanceof JsonParseError,
  );

  assertThrows(
    "trailing comma in array",
    () => parse('[1,2,]'),
    e => e instanceof JsonParseError,
  );

  assertThrows(
    "bare identifier",
    () => parse('undefined'),
    e => e instanceof JsonParseError,
  );

  assertThrows(
    "line/column on multi-line error",
    () => parse('{\n  "a": !invalid\n}'),
    e => {
      const err = e as JsonParseError;
      return (
        err instanceof JsonParseError &&
        err.line === 2 &&
        err.column === 7
      );
    },
  );

  // ── Parse: big-number-as-string ──
  console.log("\nparse — bigNumberAsString");
  const bigInt = "99999999999999999999";
  assert(
    "big integer returned as string",
    parse(bigInt, { bigNumberAsString: true }) === bigInt,
  );
  assert(
    "normal integer still a number",
    parse("42", { bigNumberAsString: true }) === 42,
  );
  assert(
    "MAX_SAFE_INTEGER still a number",
    parse(String(Number.MAX_SAFE_INTEGER), { bigNumberAsString: true }) ===
      Number.MAX_SAFE_INTEGER,
  );
  assert(
    "float not affected by bigNumberAsString",
    parse("1.23456789012345678", { bigNumberAsString: true }) ===
      1.23456789012345678,
  );

  // ── Parse: reviver ──
  console.log("\nparse — reviver");
  const dateRevived = parse(
    '{"created":"2024-01-15"}',
    {
      reviver: (k, v) =>
        k === "created" && typeof v === "string" ? new Date(v) : v,
    },
  ) as { created: Date };
  assert(
    "reviver converts string to Date",
    dateRevived.created instanceof Date,
  );

  const dropB = parse('{"a":1,"b":2,"c":3}', {
    reviver: (k, v) => k === "b" ? undefined : v,
  }) as Record<string, unknown>;
  assert("reviver returning undefined drops key",
    !("b" in dropB) && dropB["a"] === 1 && dropB["c"] === 3);

  const sumArrayReviver = parse('[1,2,3]', {
    reviver: (k, v) => {
      if (k === "") {
        // root — v is the final array
        return (v as number[]).reduce((a, b) => a + b, 0);
      }
      return v;
    },
  });
  assert("reviver on root replaces value", sumArrayReviver === 6);

  // ── Round-trip ──
  console.log("\nround-trip");
  const complex = {
    name: "test",
    nums: [1, -2, 3.14, 0],
    nested: { flag: true, nil: null },
    special: "tab:\there",
    empty: {},
    arr: [],
  };
  assert(
    "complex object round-trips",
    eq(parse(stringify(complex) ?? ""), complex),
  );

  const withUnicode = { emoji: "😀", snowman: "☃", math: "∫∑∞" };
  assert(
    "unicode round-trips",
    eq(parse(stringify(withUnicode) ?? ""), withUnicode),
  );

  // ── Summary ──
  console.log(
    `\n${"─".repeat(40)}\n  ${passed} passed, ${failed} failed\n`,
  );

  if (failed > 0) {
    process.exitCode = 1;
  }
}

// Run when executed directly: `npx ts-node json.ts` or `deno run json.ts`
// In Deno, import.meta.main is true; in Node with ts-node, require.main check
// is unreliable, so we check a common pattern.
if (
  typeof process !== "undefined" &&
  // ts-node sets argv[1] to the script path
  (process.argv[1]?.endsWith("json.ts") ||
   process.argv[1]?.endsWith("json.js"))
) {
  runTests();
}

export { runTests };
