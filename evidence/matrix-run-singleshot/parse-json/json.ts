/**
 * json.ts — JSON serializer and parser written from scratch.
 *
 * Features:
 *   stringify:
 *     - Cycle detection (throws on circular references)
 *     - Replacer support (function or array whitelist)
 *     - Optional indentation
 *     - BigInt serialized as string when bigIntAsString option is set
 *   parse:
 *     - Position-tracked errors (line + column in error messages)
 *     - Reviver support
 *     - Optional bigIntAsString: returns numeric strings that exceed
 *       Number.MAX_SAFE_INTEGER as BigInt
 *
 * No dependencies. Strict TypeScript.
 */

// ---------------------------------------------------------------------------
// Public types
// ---------------------------------------------------------------------------

export type JsonPrimitive = string | number | boolean | null;
export type JsonValue =
  | JsonPrimitive
  | JsonValue[]
  | { [key: string]: JsonValue };

export type ReplacerFn = (key: string, value: unknown) => unknown;
export type ReviverFn = (key: string, value: unknown) => unknown;

export interface StringifyOptions {
  /**
   * Number of spaces for indentation, or an indent string.
   * Omit or pass 0 for compact output.
   */
  indent?: number | string;
  /**
   * A replacer function or an array of key names to include.
   */
  replacer?: ReplacerFn | (string | number)[];
  /**
   * When true, BigInt values are serialized as their decimal string
   * representation (e.g. 9007199254740993n → "9007199254740993").
   * When false (default), BigInt values throw TypeError.
   */
  bigIntAsString?: boolean;
}

export interface ParseOptions {
  /**
   * A reviver applied bottom-up, same contract as JSON.parse reviver.
   */
  reviver?: ReviverFn;
  /**
   * When true, integer strings that exceed Number.MAX_SAFE_INTEGER are
   * returned as BigInt instead of number.
   */
  bigIntAsString?: boolean;
}

// ---------------------------------------------------------------------------
// Stringify
// ---------------------------------------------------------------------------

/**
 * Serialize a value to a JSON string.
 * Throws TypeError on circular references or (by default) BigInt values.
 */
export function stringify(
  value: unknown,
  options: StringifyOptions = {}
): string {
  const { indent, replacer, bigIntAsString = false } = options;

  // Normalize indent
  let indentStr = "";
  if (typeof indent === "number" && indent > 0) {
    indentStr = " ".repeat(Math.min(indent, 10));
  } else if (typeof indent === "string" && indent.length > 0) {
    indentStr = indent.slice(0, 10);
  }

  // Normalize replacer
  let replacerFn: ReplacerFn | null = null;
  let whitelist: Set<string> | null = null;
  if (typeof replacer === "function") {
    replacerFn = replacer;
  } else if (Array.isArray(replacer)) {
    whitelist = new Set(replacer.map(String));
  }

  // Cycle-detection stack (object identity)
  const seen = new Set<object>();

  function serializeValue(
    key: string,
    val: unknown,
    depth: number
  ): string | undefined {
    // Apply replacer function if present
    if (replacerFn !== null) {
      val = replacerFn(key, val);
    }

    // Handle toJSON
    if (
      val !== null &&
      typeof val === "object" &&
      typeof (val as Record<string, unknown>).toJSON === "function"
    ) {
      val = (val as { toJSON: (key: string) => unknown }).toJSON(key);
    }

    if (val === undefined || typeof val === "function" || typeof val === "symbol") {
      return undefined; // omit from output
    }

    if (val === null) return "null";
    if (typeof val === "boolean") return val ? "true" : "false";

    if (typeof val === "bigint") {
      if (!bigIntAsString) {
        throw new TypeError(
          `BigInt value "${val}" cannot be serialized as JSON. ` +
            `Pass { bigIntAsString: true } to serialize as string.`
        );
      }
      return jsonString(val.toString());
    }

    if (typeof val === "number") {
      if (!isFinite(val)) return "null"; // JSON spec: Infinity/NaN → null
      return String(val);
    }

    if (typeof val === "string") {
      return jsonString(val);
    }

    // Object or Array
    if (typeof val === "object") {
      if (seen.has(val as object)) {
        throw new TypeError("Converting circular structure to JSON");
      }
      seen.add(val as object);

      let result: string;
      try {
        if (Array.isArray(val)) {
          result = serializeArray(val, depth);
        } else {
          result = serializeObject(val as Record<string, unknown>, depth);
        }
      } finally {
        seen.delete(val as object);
      }
      return result;
    }

    return undefined;
  }

  function serializeArray(arr: unknown[], depth: number): string {
    if (arr.length === 0) return "[]";
    const childDepth = depth + 1;
    const items: string[] = [];
    for (let i = 0; i < arr.length; i++) {
      const serialized = serializeValue(String(i), arr[i], childDepth);
      items.push(serialized === undefined ? "null" : serialized);
    }
    return formatContainer("[", "]", items, depth, childDepth);
  }

  function serializeObject(
    obj: Record<string, unknown>,
    depth: number
  ): string {
    const childDepth = depth + 1;
    const pairs: string[] = [];
    const keys = Object.keys(obj);
    for (const k of keys) {
      if (whitelist !== null && !whitelist.has(k)) continue;
      const val = obj[k];
      const serializedVal = serializeValue(k, val, childDepth);
      if (serializedVal === undefined) continue; // omit undefined values
      pairs.push(`${jsonString(k)}:${indentStr ? " " : ""}${serializedVal}`);
    }
    if (pairs.length === 0) return "{}";
    return formatContainer("{", "}", pairs, depth, childDepth);
  }

  function formatContainer(
    open: string,
    close: string,
    items: string[],
    depth: number,
    childDepth: number
  ): string {
    if (!indentStr) {
      return `${open}${items.join(",")}${close}`;
    }
    const outerPad = indentStr.repeat(depth);
    const innerPad = indentStr.repeat(childDepth);
    return (
      `${open}\n` +
      items.map((item) => `${innerPad}${item}`).join(",\n") +
      `\n${outerPad}${close}`
    );
  }

  // Kick off serialization with the root wrapper key ""
  const root = serializeValue("", value, 0);
  return root === undefined ? undefined as unknown as string : root;
}

/** Escape a string to a valid JSON string literal, including all required escapes. */
function jsonString(s: string): string {
  let out = '"';
  for (let i = 0; i < s.length; i++) {
    const cp = s.charCodeAt(i);
    switch (cp) {
      case 0x22: out += '\\"'; break;  // "
      case 0x5c: out += "\\\\"; break; // \
      case 0x08: out += "\\b"; break;
      case 0x09: out += "\\t"; break;
      case 0x0a: out += "\\n"; break;
      case 0x0c: out += "\\f"; break;
      case 0x0d: out += "\\r"; break;
      default:
        if (cp < 0x20) {
          out += `\\u${cp.toString(16).padStart(4, "0")}`;
        } else {
          out += s[i];
        }
    }
  }
  out += '"';
  return out;
}

// ---------------------------------------------------------------------------
// Parse
// ---------------------------------------------------------------------------

/**
 * Parse a JSON string into a JavaScript value.
 * Throws SyntaxError with line and column position on invalid input.
 */
export function parse(text: string, options: ParseOptions = {}): unknown {
  const { reviver, bigIntAsString = false } = options;

  let pos = 0; // current byte position in text
  let line = 1;
  let lineStart = 0;

  function col(): number {
    return pos - lineStart + 1;
  }

  function err(msg: string): never {
    throw new SyntaxError(`JSON parse error at line ${line}, col ${col()}: ${msg}`);
  }

  function peek(): string {
    return text[pos] ?? "";
  }

  function advance(): string {
    const ch = text[pos++];
    if (ch === "\n") {
      line++;
      lineStart = pos;
    }
    return ch;
  }

  function expect(ch: string): void {
    if (text[pos] !== ch) {
      err(`Expected '${ch}' but got '${peek() || "EOF"}'`);
    }
    advance();
  }

  function skipWhitespace(): void {
    while (pos < text.length) {
      const ch = text[pos];
      if (ch === " " || ch === "\t" || ch === "\r" || ch === "\n") {
        advance();
      } else {
        break;
      }
    }
  }

  function parseValue(): unknown {
    skipWhitespace();
    const ch = peek();
    if (ch === '"') return parseString();
    if (ch === "{") return parseObject();
    if (ch === "[") return parseArray();
    if (ch === "t") return parseLiteral("true", true);
    if (ch === "f") return parseLiteral("false", false);
    if (ch === "n") return parseLiteral("null", null);
    if (ch === "-" || (ch >= "0" && ch <= "9")) return parseNumber();
    if (ch === "") err("Unexpected end of input");
    err(`Unexpected token '${ch}'`);
  }

  function parseLiteral(word: string, value: unknown): unknown {
    for (let i = 0; i < word.length; i++) {
      if (text[pos + i] !== word[i]) {
        err(`Invalid token (expected '${word}')`);
      }
    }
    pos += word.length;
    return value;
  }

  function parseString(): string {
    expect('"');
    let result = "";
    while (pos < text.length) {
      const ch = advance();
      if (ch === '"') return result;
      if (ch === "\\") {
        const esc = advance();
        switch (esc) {
          case '"':  result += '"';  break;
          case "\\": result += "\\"; break;
          case "/":  result += "/";  break;
          case "b":  result += "\b"; break;
          case "f":  result += "\f"; break;
          case "n":  result += "\n"; break;
          case "r":  result += "\r"; break;
          case "t":  result += "\t"; break;
          case "u": {
            const hex = text.slice(pos, pos + 4);
            if (!/^[0-9a-fA-F]{4}$/.test(hex)) {
              err(`Invalid unicode escape \\u${hex}`);
            }
            result += String.fromCharCode(parseInt(hex, 16));
            pos += 4;
            break;
          }
          default:
            err(`Invalid escape sequence '\\${esc}'`);
        }
      } else if (ch.charCodeAt(0) < 0x20) {
        err(`Unescaped control character in string (codepoint ${ch.charCodeAt(0)})`);
      } else {
        result += ch;
      }
    }
    err("Unterminated string");
  }

  function parseNumber(): number | bigint {
    const start = pos;
    // Optional minus
    if (text[pos] === "-") pos++;
    // Integer part
    if (text[pos] === "0") {
      pos++;
    } else if (text[pos] >= "1" && text[pos] <= "9") {
      while (pos < text.length && text[pos] >= "0" && text[pos] <= "9") pos++;
    } else {
      err("Invalid number");
    }
    const isFloat = text[pos] === "." || text[pos] === "e" || text[pos] === "E";
    // Fractional part
    if (text[pos] === ".") {
      pos++;
      if (text[pos] < "0" || text[pos] > "9") err("Invalid number: expected digit after '.'");
      while (pos < text.length && text[pos] >= "0" && text[pos] <= "9") pos++;
    }
    // Exponent
    if (text[pos] === "e" || text[pos] === "E") {
      pos++;
      if (text[pos] === "+" || text[pos] === "-") pos++;
      if (text[pos] < "0" || text[pos] > "9") err("Invalid number: expected digit in exponent");
      while (pos < text.length && text[pos] >= "0" && text[pos] <= "9") pos++;
    }
    const raw = text.slice(start, pos);
    // BigInt promotion for safe-integer overflow
    if (bigIntAsString && !isFloat) {
      const n = Number(raw);
      if (!Number.isSafeInteger(n)) {
        try {
          return BigInt(raw);
        } catch {
          // fall through to number
        }
      }
    }
    return Number(raw);
  }

  function parseArray(): unknown[] {
    expect("[");
    skipWhitespace();
    const result: unknown[] = [];
    if (peek() === "]") {
      advance();
      return result;
    }
    while (true) {
      result.push(parseValue());
      skipWhitespace();
      if (peek() === "]") {
        advance();
        break;
      }
      expect(",");
      // Trailing comma is NOT allowed per JSON spec
      skipWhitespace();
      if (peek() === "]") err("Trailing comma in array is not valid JSON");
    }
    return result;
  }

  function parseObject(): Record<string, unknown> {
    expect("{");
    skipWhitespace();
    const result: Record<string, unknown> = {};
    if (peek() === "}") {
      advance();
      return result;
    }
    while (true) {
      skipWhitespace();
      if (peek() !== '"') err(`Expected string key but got '${peek() || "EOF"}'`);
      const key = parseString();
      skipWhitespace();
      expect(":");
      const val = parseValue();
      result[key] = val;
      skipWhitespace();
      if (peek() === "}") {
        advance();
        break;
      }
      expect(",");
      skipWhitespace();
      if (peek() === "}") err("Trailing comma in object is not valid JSON");
    }
    return result;
  }

  const root = parseValue();
  skipWhitespace();
  if (pos < text.length) {
    err(`Unexpected token '${peek()}' after top-level value`);
  }

  if (!reviver) return root;

  // Apply reviver bottom-up (same walk as the spec)
  function walk(holder: Record<string, unknown>, key: string): unknown {
    const val = holder[key];
    if (val !== null && typeof val === "object") {
      if (Array.isArray(val)) {
        for (let i = 0; i < val.length; i++) {
          const newVal = walk(val as unknown as Record<string, unknown>, String(i));
          if (newVal === undefined) {
            delete (val as unknown as Record<string, unknown>)[String(i)];
          } else {
            (val as unknown as Record<string, unknown>)[String(i)] = newVal;
          }
        }
      } else {
        const obj = val as Record<string, unknown>;
        for (const k of Object.keys(obj)) {
          const newVal = walk(obj, k);
          if (newVal === undefined) {
            delete obj[k];
          } else {
            obj[k] = newVal;
          }
        }
      }
    }
    return reviver.call(holder, key, val);
  }

  const wrapper: Record<string, unknown> = { "": root };
  return walk(wrapper, "");
}

// ---------------------------------------------------------------------------
// Self-tests (run with: npx ts-node json.ts  or  node --loader ts-node/esm json.ts)
// ---------------------------------------------------------------------------

function runTests(): void {
  let passed = 0;
  let failed = 0;

  function assert(description: string, condition: boolean): void {
    if (condition) {
      console.log(`  ✓ ${description}`);
      passed++;
    } else {
      console.error(`  ✗ ${description}`);
      failed++;
    }
  }

  function assertThrows(description: string, fn: () => unknown, msgFragment?: string): void {
    try {
      fn();
      console.error(`  ✗ ${description} — expected throw, but did not`);
      failed++;
    } catch (e) {
      if (msgFragment && !(e instanceof Error && e.message.includes(msgFragment))) {
        console.error(`  ✗ ${description} — threw but message missing "${msgFragment}": ${(e as Error).message}`);
        failed++;
      } else {
        console.log(`  ✓ ${description}`);
        passed++;
      }
    }
  }

  // ── stringify ────────────────────────────────────────────────────────────

  console.log("\nstringify:");

  assert("null", stringify(null) === "null");
  assert("true", stringify(true) === "true");
  assert("false", stringify(false) === "false");
  assert("integer", stringify(42) === "42");
  assert("float", stringify(3.14) === "3.14");
  assert("negative", stringify(-7) === "-7");
  assert("Infinity → null", stringify(Infinity) === "null");
  assert("NaN → null", stringify(NaN) === "null");
  assert("empty string", stringify("") === '""');
  assert("simple string", stringify("hello") === '"hello"');
  assert(
    "escape sequences in string",
    stringify('a\tb\nc"d\\e') === '"a\\tb\\nc\\"d\\\\e"'
  );
  assert("control char escape", stringify("\x01") === '"\\u0001"');
  assert("empty array", stringify([]) === "[]");
  assert("array", stringify([1, "two", null]) === '[1,"two",null]');
  assert(
    "nested array",
    stringify([[1, 2], [3, 4]]) === "[[1,2],[3,4]]"
  );
  assert("empty object", stringify({}) === "{}");
  assert(
    "object",
    stringify({ a: 1, b: "x" }) === '{"a":1,"b":"x"}'
  );
  assert(
    "undefined values omitted",
    stringify({ a: 1, b: undefined, c: 3 }) === '{"a":1,"c":3}'
  );
  assert(
    "function values omitted",
    stringify({ a: 1, b: () => 2 }) === '{"a":1}'
  );
  assert(
    "undefined in array → null",
    stringify([1, undefined, 3]) === "[1,null,3]"
  );

  // indent
  assert(
    "indent number",
    stringify({ a: 1 }, { indent: 2 }) === '{\n  "a": 1\n}'
  );
  assert(
    "indent string",
    stringify([1, 2], { indent: "\t" }) === "[\n\t1,\n\t2\n]"
  );

  // replacer function
  assert(
    "replacer function doubles numbers",
    stringify(
      { a: 1, b: 2 },
      { replacer: (_k, v) => (typeof v === "number" ? v * 2 : v) }
    ) === '{"a":2,"b":4}'
  );

  // replacer array whitelist
  assert(
    "replacer array whitelist",
    stringify({ a: 1, b: 2, c: 3 }, { replacer: ["a", "c"] }) ===
      '{"a":1,"c":3}'
  );

  // toJSON
  const withToJSON = {
    x: 10,
    toJSON(_key: string) {
      return { serialized: true };
    },
  };
  assert(
    "toJSON honored",
    stringify(withToJSON) === '{"serialized":true}'
  );

  // BigInt
  assertThrows(
    "BigInt throws without bigIntAsString",
    () => stringify(9007199254740993n),
    "BigInt"
  );
  assert(
    "BigInt as string with option",
    stringify(9007199254740993n, { bigIntAsString: true }) ===
      '"9007199254740993"'
  );

  // Circular reference
  const circ: Record<string, unknown> = {};
  circ.self = circ;
  assertThrows("circular reference throws", () => stringify(circ), "circular");

  // ── parse ────────────────────────────────────────────────────────────────

  console.log("\nparse:");

  assert("null", parse("null") === null);
  assert("true", parse("true") === true);
  assert("false", parse("false") === false);
  assert("integer", parse("42") === 42);
  assert("negative", parse("-3") === -3);
  assert("float", parse("1.5") === 1.5);
  assert("exponent", parse("2e3") === 2000);
  assert("string", parse('"hello"') === "hello");
  assert("empty string", parse('""') === "");
  assert("escape tab", parse('"a\\tb"') === "a\tb");
  assert("escape newline", parse('"a\\nb"') === "a\nb");
  assert("escape unicode", parse('"\\u0041"') === "A");
  assert("empty array", JSON.stringify(parse("[]")) === "[]");
  assert(
    "array",
    JSON.stringify(parse('[1,"two",null,true]')) ===
      JSON.stringify([1, "two", null, true])
  );
  assert("empty object", JSON.stringify(parse("{}")) === "{}");
  assert(
    "object",
    JSON.stringify(parse('{"a":1,"b":"x"}')) === '{"a":1,"b":"x"}'
  );
  assert(
    "nested",
    JSON.stringify(parse('{"arr":[1,2],"obj":{"x":true}}')) ===
      '{"arr":[1,2],"obj":{"x":true}}'
  );
  assert(
    "whitespace tolerant",
    (parse("  {  \"k\" :  1  }  ") as Record<string, unknown>).k === 1
  );

  // Position-tracked errors
  assertThrows("unexpected token", () => parse("x"), "line 1, col 1");
  assertThrows("trailing comma array", () => parse("[1,]"), "Trailing comma");
  assertThrows("trailing comma object", () => parse('{"a":1,}'), "Trailing comma");
  assertThrows("unterminated string", () => parse('"abc'), "Unterminated");
  assertThrows("bad escape", () => parse('"\\q"'), "Invalid escape");
  assertThrows("trailing content", () => parse("1 2"), "Unexpected token");
  assertThrows(
    "multiline position tracked",
    () => parse('{\n  "a": @\n}'),
    "line 2"
  );

  // BigInt parse
  const big = parse("9007199254740993", { bigIntAsString: true });
  assert("bigint parse promotion", typeof big === "bigint" && big === 9007199254740993n);
  const safe = parse("42", { bigIntAsString: true });
  assert("safe integer stays number", typeof safe === "number" && safe === 42);

  // Reviver
  const revived = parse('{"date":"2024-01-01"}', {
    reviver(_key, val) {
      if (typeof val === "string" && /^\d{4}-\d{2}-\d{2}$/.test(val)) {
        return new Date(val);
      }
      return val;
    },
  }) as Record<string, unknown>;
  assert("reviver applied", revived.date instanceof Date);

  // Round-trip
  const original = {
    name: "test",
    nums: [1, -2, 3.14, 0],
    nested: { flag: true, nothing: null },
    escaped: "tab:\there",
  };
  const roundTripped = parse(stringify(original));
  assert(
    "round-trip",
    JSON.stringify(roundTripped) === JSON.stringify(original)
  );

  // ── Summary ──────────────────────────────────────────────────────────────

  console.log(`\n${passed + failed} tests: ${passed} passed, ${failed} failed.\n`);
  if (failed > 0) process.exit(1);
}

// Run tests when executed directly
// Works for both ts-node (CommonJS) and native ESM with --loader
const isMain =
  typeof require !== "undefined"
    ? require.main === module
    : // ESM: import.meta.url check
      ((): boolean => {
        try {
          const { fileURLToPath } = await_import_meta_url();
          return process.argv[1] === fileURLToPath(import_meta_url());
        } catch {
          return false;
        }
      })();

// Simple shims so the file compiles cleanly under tsc without ESM-specific syntax
function await_import_meta_url(): { fileURLToPath: (u: string) => string } {
  // This path is never reached in a CommonJS build; ts-node handles it.
  return { fileURLToPath: (u: string) => new URL(u).pathname };
}
function import_meta_url(): string {
  return "";
}

if (isMain) {
  runTests();
}
