/**
 * json.ts — JSON serializer and parser, TypeScript strict, zero dependencies.
 *
 * Exports:
 *   stringify(value, options?)  — serialize to JSON string
 *   parse(input, options?)      — deserialize from JSON string
 *   CircularReferenceError      — thrown on cycles (default)
 *   JsonParseError              — thrown on malformed input, with position info
 *   runTests()                  — built-in self-test suite
 */

// ─── Stringify ────────────────────────────────────────────────────────────────

/** Function replacer: (key, value) => replacement, or array of allowed keys. */
export type Replacer =
  | ((key: string, value: unknown) => unknown)
  | ReadonlyArray<string | number>
  | null;

export interface StringifyOptions {
  replacer?: Replacer;
  /** Indent: number of spaces (clamped to 10), or a string (truncated to 10 chars). */
  space?: string | number;
  /**
   * What to do when a circular reference is encountered.
   *   "throw"  (default) — throws CircularReferenceError
   *   "null"             — serializes the back-reference as null
   */
  cyclicRef?: "throw" | "null";
}

export class CircularReferenceError extends Error {
  constructor(keyPath: string) {
    super(`Circular reference at key "${keyPath}"`);
    this.name = "CircularReferenceError";
  }
}

function resolveIndent(space: string | number | undefined): string {
  if (typeof space === "number") return " ".repeat(Math.min(Math.max(0, Math.floor(space)), 10));
  if (typeof space === "string") return space.slice(0, 10);
  return "";
}

const ESCAPE: Readonly<Record<string, string>> = {
  '"': '\\"',
  "\\": "\\\\",
  "\b": "\\b",
  "\f": "\\f",
  "\n": "\\n",
  "\r": "\\r",
  "\t": "\\t",
};

function escapeString(s: string): string {
  let out = '"';
  for (let i = 0; i < s.length; i++) {
    const ch = s[i];
    const code = s.charCodeAt(i);
    if (ESCAPE[ch] !== undefined) {
      out += ESCAPE[ch];
    } else if (code < 0x20) {
      out += "\\u" + code.toString(16).padStart(4, "0");
    } else {
      out += ch;
    }
  }
  return out + '"';
}

/**
 * Core recursive serializer.
 * Returns undefined for values that JSON omits (function, symbol, undefined at
 * object level). Array slots that are undefined serialize as "null".
 */
function serializeValue(
  value: unknown,
  key: string,
  replacerFn: ((key: string, value: unknown) => unknown) | null,
  allowedKeys: ReadonlySet<string> | null,
  seen: WeakSet<object>,
  indent: string,
  currentDepthIndent: string,
  cyclicRef: "throw" | "null"
): string | undefined {
  // toJSON hook (runs before replacer, matching JSON.stringify spec)
  if (
    value !== null &&
    typeof value === "object" &&
    "toJSON" in value &&
    typeof (value as { toJSON?: unknown }).toJSON === "function"
  ) {
    value = (value as { toJSON(key: string): unknown }).toJSON(key);
  }

  // Replacer function
  if (replacerFn !== null) {
    value = replacerFn(key, value);
  }

  // Primitives that serialize to nothing in object context
  if (value === undefined || typeof value === "function" || typeof value === "symbol") {
    return undefined;
  }

  if (value === null) return "null";
  if (typeof value === "boolean") return value ? "true" : "false";

  if (typeof value === "number") {
    // JSON does not support NaN or Infinity; spec says emit null
    return isFinite(value) ? String(value) : "null";
  }

  if (typeof value === "string") return escapeString(value);

  // BigInt: serialize as a quoted decimal string to preserve precision
  if (typeof value === "bigint") return '"' + value.toString() + '"';

  if (typeof value !== "object") return undefined; // should not reach here

  const obj = value as object;

  if (seen.has(obj)) {
    if (cyclicRef === "null") return "null";
    throw new CircularReferenceError(key);
  }
  seen.add(obj);

  const childIndent = indent ? currentDepthIndent + indent : "";
  let result: string;

  try {
    if (Array.isArray(obj)) {
      if (obj.length === 0) {
        result = "[]";
      } else {
        const items: string[] = [];
        for (let i = 0; i < obj.length; i++) {
          const v = serializeValue(
            (obj as unknown[])[i],
            String(i),
            replacerFn,
            allowedKeys,
            seen,
            indent,
            childIndent,
            cyclicRef
          );
          items.push(v === undefined ? "null" : v);
        }
        result = indent
          ? "[\n" + childIndent + items.join(",\n" + childIndent) + "\n" + currentDepthIndent + "]"
          : "[" + items.join(",") + "]";
      }
    } else {
      const pairs: string[] = [];
      for (const k of Object.keys(obj)) {
        if (allowedKeys !== null && !allowedKeys.has(k)) continue;
        const v = serializeValue(
          (obj as Record<string, unknown>)[k],
          k,
          replacerFn,
          allowedKeys,
          seen,
          indent,
          childIndent,
          cyclicRef
        );
        if (v !== undefined) {
          pairs.push(escapeString(k) + (indent ? ": " : ":") + v);
        }
      }
      result =
        pairs.length === 0
          ? "{}"
          : indent
          ? "{\n" + childIndent + pairs.join(",\n" + childIndent) + "\n" + currentDepthIndent + "}"
          : "{" + pairs.join(",") + "}";
    }
  } finally {
    seen.delete(obj);
  }

  return result;
}

/**
 * Serialize a value to a JSON string.
 *
 * Returns `undefined` for values that have no JSON representation
 * (function, symbol, bare undefined), matching `JSON.stringify` behavior.
 */
export function stringify(value: unknown, options: StringifyOptions = {}): string | undefined {
  const { replacer = null, space, cyclicRef = "throw" } = options;
  const indent = resolveIndent(space);

  let replacerFn: ((key: string, value: unknown) => unknown) | null = null;
  let allowedKeys: ReadonlySet<string> | null = null;

  if (typeof replacer === "function") {
    replacerFn = replacer;
  } else if (Array.isArray(replacer)) {
    allowedKeys = new Set(replacer.map(String));
  }

  // Apply toJSON / replacer at the root level (key = "")
  let root: unknown = value;
  if (
    root !== null &&
    typeof root === "object" &&
    "toJSON" in root &&
    typeof (root as { toJSON?: unknown }).toJSON === "function"
  ) {
    root = (root as { toJSON(key: string): unknown }).toJSON("");
  }
  if (replacerFn !== null) {
    root = replacerFn("", root);
  }

  return serializeValue(root, "", null, allowedKeys, new WeakSet<object>(), indent, "", cyclicRef);
}

// ─── Parse ────────────────────────────────────────────────────────────────────

export interface ParseOptions {
  /**
   * When true, integers that cannot be exactly represented as a 64-bit float
   * (i.e. abs(value) > Number.MAX_SAFE_INTEGER) are returned as strings
   * instead of numbers.
   */
  bigNumberAsString?: boolean;
}

/** Thrown when input is not valid JSON. Carries position information. */
export class JsonParseError extends Error {
  /** Byte offset from the start of the input string. */
  readonly offset: number;
  /** 1-based line number. */
  readonly line: number;
  /** 1-based column number. */
  readonly column: number;

  constructor(message: string, offset: number, line: number, column: number) {
    super(`${message} (line ${line}, col ${column}, offset ${offset})`);
    this.name = "JsonParseError";
    this.offset = offset;
    this.line = line;
    this.column = column;
  }
}

class Parser {
  private readonly src: string;
  private pos = 0;
  private line = 1;
  private col = 1;
  private readonly bigNumberAsString: boolean;

  constructor(src: string, opts: ParseOptions) {
    this.src = src;
    this.bigNumberAsString = opts.bigNumberAsString === true;
  }

  // ── Utilities ──────────────────────────────────────────────────────────────

  private err(msg: string): never {
    throw new JsonParseError(msg, this.pos, this.line, this.col);
  }

  private peek(): string {
    return this.src[this.pos] ?? "";
  }

  private advance(): string {
    const ch = this.src[this.pos++];
    if (ch === "\n") {
      this.line++;
      this.col = 1;
    } else {
      this.col++;
    }
    return ch;
  }

  private skipWS(): void {
    while (this.pos < this.src.length) {
      const ch = this.src[this.pos];
      if (ch === " " || ch === "\t" || ch === "\r" || ch === "\n") {
        this.advance();
      } else {
        break;
      }
    }
  }

  // ── Entry point ────────────────────────────────────────────────────────────

  parse(): unknown {
    this.skipWS();
    const val = this.parseValue();
    this.skipWS();
    if (this.pos < this.src.length) {
      this.err(`Unexpected token '${this.src[this.pos]}'`);
    }
    return val;
  }

  // ── Value dispatch ─────────────────────────────────────────────────────────

  private parseValue(): unknown {
    const ch = this.peek();
    switch (ch) {
      case '"': return this.parseString();
      case "{": return this.parseObject();
      case "[": return this.parseArray();
      case "t": return this.parseLiteral("true", true);
      case "f": return this.parseLiteral("false", false);
      case "n": return this.parseLiteral("null", null);
      case "":  this.err("Unexpected end of input");
      default:
        if (ch === "-" || (ch >= "0" && ch <= "9")) return this.parseNumber();
        this.err(`Unexpected token '${ch}'`);
    }
  }

  // ── Literals ───────────────────────────────────────────────────────────────

  private parseLiteral<T>(word: string, result: T): T {
    for (const ch of word) {
      if (this.src[this.pos] !== ch) {
        this.err(`Invalid token (expected '${word}')`);
      }
      this.advance();
    }
    return result;
  }

  // ── Number ─────────────────────────────────────────────────────────────────

  private parseNumber(): number | string {
    const start = this.pos;
    let isFloat = false;

    if (this.src[this.pos] === "-") this.advance();

    if (this.src[this.pos] === "0") {
      this.advance();
    } else if (this.src[this.pos] >= "1" && this.src[this.pos] <= "9") {
      while (this.pos < this.src.length && this.src[this.pos] >= "0" && this.src[this.pos] <= "9") {
        this.advance();
      }
    } else {
      this.err(`Invalid number starting with '${this.src[this.pos] ?? "EOF"}'`);
    }

    if (this.src[this.pos] === ".") {
      isFloat = true;
      this.advance();
      if (this.src[this.pos] < "0" || this.src[this.pos] > "9") {
        this.err("Expected digit after decimal point");
      }
      while (this.pos < this.src.length && this.src[this.pos] >= "0" && this.src[this.pos] <= "9") {
        this.advance();
      }
    }

    if (this.src[this.pos] === "e" || this.src[this.pos] === "E") {
      isFloat = true;
      this.advance();
      if (this.src[this.pos] === "+" || this.src[this.pos] === "-") this.advance();
      if (this.src[this.pos] < "0" || this.src[this.pos] > "9") {
        this.err("Expected digit in exponent");
      }
      while (this.pos < this.src.length && this.src[this.pos] >= "0" && this.src[this.pos] <= "9") {
        this.advance();
      }
    }

    const raw = this.src.slice(start, this.pos);

    // Big-integer path: integers outside Number.MAX_SAFE_INTEGER precision
    if (this.bigNumberAsString && !isFloat) {
      const big = BigInt(raw);
      if (big > BigInt(Number.MAX_SAFE_INTEGER) || big < BigInt(Number.MIN_SAFE_INTEGER)) {
        return raw;
      }
    }

    return Number(raw);
  }

  // ── String ─────────────────────────────────────────────────────────────────

  private parseString(): string {
    // consume opening quote
    if (this.src[this.pos] !== '"') this.err("Expected '\"'");
    this.advance();

    let out = "";

    while (this.pos < this.src.length) {
      const ch = this.src[this.pos];

      if (ch === '"') {
        this.advance();
        return out;
      }

      if (ch === "\\") {
        this.advance();
        const esc = this.src[this.pos];
        this.advance();
        switch (esc) {
          case '"':  out += '"';  break;
          case "\\": out += "\\"; break;
          case "/":  out += "/";  break;
          case "b":  out += "\b"; break;
          case "f":  out += "\f"; break;
          case "n":  out += "\n"; break;
          case "r":  out += "\r"; break;
          case "t":  out += "\t"; break;
          case "u":  out += this.parseUnicodeEscape(); break;
          default:
            this.err(`Unknown escape sequence '\\${esc}'`);
        }
        continue;
      }

      if (ch.charCodeAt(0) < 0x20) {
        this.err(`Unescaped control character U+${ch.charCodeAt(0).toString(16).padStart(4, "0")} in string`);
      }

      out += ch;
      this.advance();
    }

    this.err("Unterminated string literal");
  }

  /**
   * Parse a \uXXXX escape (the leading \u has already been consumed).
   * Handles surrogate pairs (\uD800–\uDBFF followed by \uDC00–\uDFFF).
   */
  private parseUnicodeEscape(): string {
    const hex1 = this.readHex4();
    const cp1 = parseInt(hex1, 16);

    // High surrogate
    if (cp1 >= 0xd800 && cp1 <= 0xdbff) {
      if (this.src[this.pos] !== "\\" || this.src[this.pos + 1] !== "u") {
        this.err("Expected low surrogate after high surrogate");
      }
      this.pos += 2; this.col += 2; // consume \u
      const hex2 = this.readHex4();
      const cp2 = parseInt(hex2, 16);
      if (cp2 < 0xdc00 || cp2 > 0xdfff) {
        this.err(`Invalid low surrogate \\u${hex2}`);
      }
      return String.fromCodePoint(0x10000 + ((cp1 - 0xd800) << 10) + (cp2 - 0xdc00));
    }

    // Lone low surrogate is technically invalid but we emit it verbatim
    return String.fromCharCode(cp1);
  }

  private readHex4(): string {
    const hex = this.src.slice(this.pos, this.pos + 4);
    if (!/^[0-9a-fA-F]{4}$/.test(hex)) {
      this.err(`Invalid unicode escape \\u${hex}`);
    }
    this.pos += 4;
    this.col += 4;
    return hex;
  }

  // ── Array ──────────────────────────────────────────────────────────────────

  private parseArray(): unknown[] {
    this.advance(); // [
    this.skipWS();

    const arr: unknown[] = [];

    if (this.peek() === "]") {
      this.advance();
      return arr;
    }

    for (;;) {
      this.skipWS();
      arr.push(this.parseValue());
      this.skipWS();

      const ch = this.peek();
      if (ch === "]") {
        this.advance();
        return arr;
      }
      if (ch === ",") {
        this.advance();
        this.skipWS();
        if (this.peek() === "]") this.err("Trailing comma in array");
      } else {
        this.err(`Expected ',' or ']' but got '${ch || "EOF"}'`);
      }
    }
  }

  // ── Object ─────────────────────────────────────────────────────────────────

  private parseObject(): Record<string, unknown> {
    this.advance(); // {
    this.skipWS();

    const obj: Record<string, unknown> = {};

    if (this.peek() === "}") {
      this.advance();
      return obj;
    }

    for (;;) {
      this.skipWS();
      if (this.peek() !== '"') {
        this.err(`Expected string key but got '${this.peek() || "EOF"}'`);
      }
      const key = this.parseString();
      this.skipWS();
      if (this.src[this.pos] !== ":") {
        this.err(`Expected ':' after object key but got '${this.peek() || "EOF"}'`);
      }
      this.advance(); // :
      this.skipWS();
      obj[key] = this.parseValue();
      this.skipWS();

      const ch = this.peek();
      if (ch === "}") {
        this.advance();
        return obj;
      }
      if (ch === ",") {
        this.advance();
        this.skipWS();
        if (this.peek() === "}") this.err("Trailing comma in object");
      } else {
        this.err(`Expected ',' or '}' but got '${ch || "EOF"}'`);
      }
    }
  }
}

/**
 * Parse a JSON string into a JavaScript value.
 *
 * Throws `JsonParseError` on invalid input, with `.line`, `.column`, and
 * `.offset` indicating where in the input the error occurred.
 */
export function parse(input: string, options: ParseOptions = {}): unknown {
  if (typeof input !== "string") {
    throw new TypeError("json.parse: input must be a string");
  }
  return new Parser(input, options).parse();
}

// ─── Self-tests ───────────────────────────────────────────────────────────────

type TestFn = () => void;

function runSuite(title: string, tests: Record<string, TestFn>): { passed: number; failed: number } {
  console.log(`\n${title}`);
  let passed = 0;
  let failed = 0;
  for (const [name, fn] of Object.entries(tests)) {
    try {
      fn();
      console.log(`  ✓ ${name}`);
      passed++;
    } catch (e) {
      console.error(`  ✗ ${name}`);
      console.error(`    ${(e as Error).message}`);
      failed++;
    }
  }
  return { passed, failed };
}

function eq(a: unknown, b: unknown, label: string): void {
  const as = JSON.stringify(a);
  const bs = JSON.stringify(b);
  if (as !== bs) throw new Error(`${label}\n    expected: ${bs}\n    got:      ${as}`);
}

function throws(fn: () => unknown, check?: (e: Error) => boolean, label?: string): void {
  try {
    fn();
  } catch (e) {
    if (check && !check(e as Error)) {
      throw new Error(`${label ?? "throws"}: error did not match predicate — ${(e as Error).message}`);
    }
    return;
  }
  throw new Error(`${label ?? "throws"}: expected an error but none was thrown`);
}

export function runTests(): void {
  let totalPassed = 0;
  let totalFailed = 0;

  function suite(title: string, tests: Record<string, TestFn>): void {
    const { passed, failed } = runSuite(title, tests);
    totalPassed += passed;
    totalFailed += failed;
  }

  // ── stringify: primitives ────────────────────────────────────────────────
  suite("stringify / primitives", {
    "null": () => eq(stringify(null), "null", "null"),
    "true": () => eq(stringify(true), "true", "bool"),
    "false": () => eq(stringify(false), "false", "bool"),
    "integer": () => eq(stringify(42), "42", "int"),
    "float": () => eq(stringify(3.14), "3.14", "float"),
    "NaN → null": () => eq(stringify(NaN), "null", "NaN"),
    "Infinity → null": () => eq(stringify(Infinity), "null", "Inf"),
    "-Infinity → null": () => eq(stringify(-Infinity), "null", "-Inf"),
    "string basic": () => eq(stringify("hello"), '"hello"', "str"),
    "string escapes": () => eq(stringify('a\nb\tc"d\\e'), '"a\\nb\\tc\\"d\\\\e"', "escapes"),
    "string control chars": () => eq(stringify("\x01\x1f"), '"\\u0001\\u001f"', "ctrl"),
    "undefined → undefined": () => eq(stringify(undefined), undefined, "undef"),
    "function → undefined": () => eq(stringify(() => 0), undefined, "fn"),
    "bigint → quoted string": () => eq(stringify(BigInt("99999999999999999999")), '"99999999999999999999"', "bigint"),
  });

  // ── stringify: arrays & objects ──────────────────────────────────────────
  suite("stringify / arrays & objects", {
    "empty array": () => eq(stringify([]), "[]", "[]"),
    "empty object": () => eq(stringify({}), "{}", "{}"),
    "array": () => eq(stringify([1, "x", null, true]), '[1,"x",null,true]', "arr"),
    "undefined in array → null": () => eq(stringify([1, undefined, 3]), "[1,null,3]", "arr-undef"),
    "nested": () => eq(stringify({ a: [1, 2], b: { c: 3 } }), '{"a":[1,2],"b":{"c":3}}', "nested"),
    "toJSON called": () => {
      const obj = { x: 1, toJSON: () => ({ x: 99 }) };
      eq(stringify(obj), '{"x":99}', "toJSON");
    },
    "undefined property omitted": () => {
      eq(stringify({ a: 1, b: undefined, c: 3 }), '{"a":1,"c":3}', "undef-prop");
    },
  });

  // ── stringify: indentation ───────────────────────────────────────────────
  suite("stringify / indentation", {
    "numeric space": () => {
      const r = stringify({ a: 1 }, { space: 2 });
      eq(r, '{\n  "a": 1\n}', "indent-2");
    },
    "string space": () => {
      const r = stringify([1, 2], { space: "\t" });
      eq(r, '[\n\t1,\n\t2\n]', "tab");
    },
    "space clamped to 10": () => {
      const r = stringify({ a: 1 }, { space: 100 });
      // 10 spaces
      eq(r, '{\n' + " ".repeat(10) + '"a": 1\n}', "clamp-10");
    },
  });

  // ── stringify: replacer ──────────────────────────────────────────────────
  suite("stringify / replacer", {
    "function replacer": () => {
      const r = stringify({ a: 1, b: 2 }, {
        replacer: (k, v) => (k === "b" ? undefined : v),
      });
      eq(r, '{"a":1}', "fn-repl");
    },
    "array replacer": () => {
      const r = stringify({ a: 1, b: 2, c: 3 }, { replacer: ["a", "c"] });
      eq(r, '{"a":1,"c":3}', "arr-repl");
    },
    "replacer transforms values": () => {
      const r = stringify({ n: 5 }, {
        replacer: (k, v) => (typeof v === "number" ? v * 2 : v),
      });
      eq(r, '{"n":10}', "xform");
    },
  });

  // ── stringify: cycle detection ───────────────────────────────────────────
  suite("stringify / cycle detection", {
    "throws on cycle (default)": () => {
      const a: Record<string, unknown> = { x: 1 };
      a["self"] = a;
      throws(() => stringify(a), (e) => e instanceof CircularReferenceError, "cycle-throw");
    },
    "null on cycle": () => {
      const a: Record<string, unknown> = { x: 1 };
      a["self"] = a;
      const r = stringify(a, { cyclicRef: "null" });
      eq(r, '{"x":1,"self":null}', "cycle-null");
    },
    "nested cycle": () => {
      const a: Record<string, unknown> = {};
      const b: Record<string, unknown> = { parent: a };
      a["child"] = b;
      throws(() => stringify(a), (e) => e instanceof CircularReferenceError, "nested-cycle");
    },
  });

  // ── parse: primitives ────────────────────────────────────────────────────
  suite("parse / primitives", {
    "null": () => eq(parse("null"), null, "null"),
    "true": () => eq(parse("true"), true, "true"),
    "false": () => eq(parse("false"), false, "false"),
    "integer": () => eq(parse("42"), 42, "int"),
    "negative": () => eq(parse("-7"), -7, "neg"),
    "float": () => eq(parse("3.14"), 3.14, "float"),
    "exponent": () => eq(parse("1e3"), 1000, "exp"),
    "exponent negative": () => eq(parse("2.5e-2"), 0.025, "exp-neg"),
    "string": () => eq(parse('"hello"'), "hello", "str"),
    "string escapes": () => eq(parse('"\\n\\t\\r"'), "\n\t\r", "escapes"),
    "string unicode": () => eq(parse('"\\u0041"'), "A", "unicode"),
    "surrogate pair": () => eq(parse('"\\uD83D\\uDE00"'), "😀", "surrogate"),
  });

  // ── parse: arrays & objects ──────────────────────────────────────────────
  suite("parse / arrays & objects", {
    "empty array": () => eq(parse("[]"), [], "[]"),
    "empty object": () => eq(parse("{}"), {}, "{}"),
    "array": () => eq(parse('[1,"x",null,true]'), [1, "x", null, true], "arr"),
    "nested": () => eq(parse('{"a":[1,2],"b":{"c":3}}'), { a: [1, 2], b: { c: 3 } }, "nested"),
    "whitespace tolerant": () => eq(parse(' { "x" : 1 } '), { x: 1 }, "ws"),
  });

  // ── parse: position-tracked errors ──────────────────────────────────────
  suite("parse / position-tracked errors", {
    "reports offset": () => {
      try {
        parse("{ bad }");
        throw new Error("should have thrown");
      } catch (e) {
        if (!(e instanceof JsonParseError)) throw new Error("wrong error type");
        if (e.offset !== 2) throw new Error(`expected offset 2, got ${e.offset}`);
      }
    },
    "reports line and column": () => {
      try {
        parse('{\n  "a": bad\n}');
        throw new Error("should have thrown");
      } catch (e) {
        if (!(e instanceof JsonParseError)) throw new Error("wrong error type");
        if (e.line !== 2) throw new Error(`expected line 2, got ${e.line}`);
        if (e.column !== 7) throw new Error(`expected col 7, got ${e.column}`);
      }
    },
    "trailing comma in array": () => {
      throws(() => parse("[1,2,]"), (e) => e instanceof JsonParseError && /trailing/i.test(e.message), "trail-arr");
    },
    "trailing comma in object": () => {
      throws(() => parse('{"a":1,}'), (e) => e instanceof JsonParseError && /trailing/i.test(e.message), "trail-obj");
    },
    "unexpected token": () => {
      throws(() => parse("@"), (e) => e instanceof JsonParseError, "tok");
    },
    "unterminated string": () => {
      throws(() => parse('"hello'), (e) => e instanceof JsonParseError, "unterm");
    },
    "invalid escape": () => {
      throws(() => parse('"\\q"'), (e) => e instanceof JsonParseError, "esc");
    },
    "unescaped control char": () => {
      throws(() => parse('"\x00"'), (e) => e instanceof JsonParseError, "ctrl");
    },
    "trailing content": () => {
      throws(() => parse("1 2"), (e) => e instanceof JsonParseError, "trail");
    },
    "empty input": () => {
      throws(() => parse(""), (e) => e instanceof JsonParseError, "empty");
    },
  });

  // ── parse: big-number handling ───────────────────────────────────────────
  suite("parse / bigNumberAsString", {
    "safe integer stays number": () => {
      const v = parse("9007199254740991", { bigNumberAsString: true });
      eq(typeof v, "number", "type");
      eq(v, 9007199254740991, "val");
    },
    "unsafe integer → string": () => {
      const v = parse("99999999999999999999", { bigNumberAsString: true });
      eq(v, "99999999999999999999", "big-int");
    },
    "unsafe negative → string": () => {
      const v = parse("-99999999999999999999", { bigNumberAsString: true });
      eq(v, "-99999999999999999999", "big-neg");
    },
    "float not affected": () => {
      const v = parse("1.5e20", { bigNumberAsString: true });
      eq(typeof v, "number", "float-type");
    },
    "big number off by default": () => {
      const v = parse("99999999999999999999");
      eq(typeof v, "number", "default-off");
    },
  });

  // ── round-trip ───────────────────────────────────────────────────────────
  suite("round-trip", {
    "primitives": () => {
      for (const v of [null, true, false, 0, 1, -1, 3.14, "", "hello", "a\nb"]) {
        const rt = parse(stringify(v) as string);
        eq(rt, v, `round-trip ${JSON.stringify(v)}`);
      }
    },
    "complex object": () => {
      const obj = { a: [1, 2, null], b: { c: "hi\nthere", d: false }, e: 0 };
      eq(parse(stringify(obj) as string), obj, "complex");
    },
    "indented parse": () => {
      const obj = { x: [1, 2, 3] };
      eq(parse(stringify(obj, { space: 2 }) as string), obj, "indented");
    },
  });

  // ── summary ──────────────────────────────────────────────────────────────
  console.log(`\n${"─".repeat(44)}`);
  console.log(`  ${totalPassed} passed, ${totalFailed} failed`);
  if (totalFailed > 0) {
    process.exit(1);
  }
}

// Run when invoked directly: ts-node json.ts  or  node json.js
const isMain =
  typeof process !== "undefined" &&
  typeof require !== "undefined" &&
  require.main === module;

if (isMain) {
  runTests();
}
